#include "torrent.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <switch.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <set>

#include "bencode.hpp"
#include "http.hpp"
#include "json.hpp"
#include "log.hpp"
#include "thread.hpp"

namespace {

constexpr int kBlock = 16 * 1024;
constexpr size_t kMaxPeers = 55;
constexpr size_t kBackgroundPeers = 25;  // a download nobody watches needs fewer connections
// Socket memory is one pool for the whole app: peer sockets get small fixed buffers instead of growing
// to 256 KB each, or a busy download left none for the app's own requests ("Couldn't connect").
constexpr int kPeerRecvBuf = 64 * 1024, kPeerSendBuf = 32 * 1024;
// Most peers trackers hand out are stale and never answer, so attempts must be many and short:
// a live peer answers a SYN well under a second. Raise kMaxConnecting if the log shows a long queue.
constexpr int kMaxConnecting = 30;
constexpr uint64_t kConnectTimeoutMs = 4000;    // TCP connect / uTP SYN
constexpr uint64_t kHandshakeTimeoutMs = 10000;  // connected, waiting for the BitTorrent handshake
constexpr int kAnnounceThreads = 4;  // trackers asked at once (each blocks a socket session while waiting)
constexpr int64_t kPrefetch = 8LL << 20;  // bytes fetched early around the resume point
constexpr int kUploadSlots = 6;               // peers we upload to (tit-for-tat), plus one optimistic
constexpr int64_t kLimitedUploadPerSec = 100 * 1024;
constexpr size_t kPipeline = 32;                // outstanding block requests per peer
constexpr int64_t kCacheBudget = 256LL << 20;   // RAM for downloaded + in-progress pieces
constexpr int64_t kBackgroundBudget = 48LL << 20;  // the same for a download nobody watches (it is on the SD card)
constexpr int64_t kReadAhead = 64LL << 20;      // high-priority window after the read head
constexpr int64_t kInitialBuffer = 3LL << 20;   // start of file needed before playback starts
constexpr int64_t kTailBuffer = 1LL << 20;      // end of file needed too (container index)
constexpr uint64_t kBlockTimeoutMs = 12000;
constexpr uint64_t kStallMs = 2000;  // a block playback waits on is asked from another peer after this
constexpr int64_t kUrgent = 8LL << 20;  // bytes after the read position whose stalled blocks get asked twice

const char* const kDefaultTrackers[] = {
    "udp://tracker.opentrackr.org:1337/announce",
    "udp://open.stealth.si:80/announce",
    "udp://tracker.torrent.eu.org:451/announce",
    "udp://exodus.desync.com:6969/announce",
    "udp://open.demonii.com:1337/announce",
    "udp://explodie.org:6969/announce",
    "udp://tracker.openbittorrent.com:6969/announce",
    "http://tracker.opentrackr.org:1337/announce",
};

uint64_t nowMs() {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

void put32(std::string& s, uint32_t v) {
    s += (char)(v >> 24);
    s += (char)(v >> 16);
    s += (char)(v >> 8);
    s += (char)v;
}

void put64(std::string& s, uint64_t v) {
    put32(s, (uint32_t)(v >> 32));
    put32(s, (uint32_t)v);
}

uint32_t get32(const char* p) {
    const uint8_t* u = (const uint8_t*)p;
    return ((uint32_t)u[0] << 24) | ((uint32_t)u[1] << 16) | ((uint32_t)u[2] << 8) | u[3];
}

struct Endpoint {
    uint32_t ip = 0;    // network byte order
    uint16_t port = 0;  // host byte order
    bool operator<(const Endpoint& o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
};

std::vector<Endpoint> parseCompact(const std::string& s) {
    std::vector<Endpoint> out;
    for (size_t i = 0; i + 6 <= s.size(); i += 6) {
        Endpoint e;
        memcpy(&e.ip, s.data() + i, 4);
        e.port = (uint16_t)(((uint8_t)s[i + 4] << 8) | (uint8_t)s[i + 5]);
        if (e.ip && e.port) out.push_back(e);
    }
    return out;
}

bool hexToBytes(const std::string& hex, uint8_t* out, size_t n) {
    if (hex.size() != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

std::string percentAll(const uint8_t* p, size_t n) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < n; i++) {
        out += '%';
        out += hex[p[i] >> 4];
        out += hex[p[i] & 15];
    }
    return out;
}

std::vector<Endpoint> httpAnnounce(const std::string& url, const uint8_t* hash, const uint8_t* peerId) {
    std::string q = url + (url.find('?') == std::string::npos ? "?" : "&");
    q += "info_hash=" + percentAll(hash, 20) + "&peer_id=" + percentAll(peerId, 20) +
         "&port=6881&uploaded=0&downloaded=0&left=1&compact=1&numwant=80&event=started";
    std::string body, err;
    if (!httpGet(q, body, err, 15, true)) return {};
    BValue v;
    size_t pos = 0;
    if (!bdecode(body, pos, v)) return {};
    const BValue* peers = v.get("peers");
    if (!peers) return {};
    if (peers->type == BValue::Str) return parseCompact(peers->s);
    std::vector<Endpoint> out;
    for (auto& p : peers->list) {
        Endpoint e;
        e.ip = inet_addr(p.getStr("ip").c_str());
        e.port = (uint16_t)p.getInt("port");
        if (e.ip && e.ip != INADDR_NONE && e.port) out.push_back(e);
    }
    return out;
}

std::vector<Endpoint> udpAnnounce(const std::string& url, const uint8_t* hash, const uint8_t* peerId) {
    std::string rest = url.substr(6);  // after "udp://"
    std::string hostport = rest.substr(0, rest.find('/'));
    size_t colon = hostport.rfind(':');
    if (colon == std::string::npos) return {};
    std::string host = hostport.substr(0, colon), port = hostport.substr(colon + 1);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return {};
    sockaddr_in addr;
    memcpy(&addr, res->ai_addr, sizeof(addr));
    freeaddrinfo(res);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return {};

    auto exchange = [&](const std::string& req, std::string& resp, size_t minLen) {
        for (int attempt = 0; attempt < 2; attempt++) {
            sendto(fd, req.data(), req.size(), 0, (sockaddr*)&addr, sizeof(addr));
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 4000) > 0) {
                char buf[4096];
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n >= (ssize_t)minLen) {
                    resp.assign(buf, (size_t)n);
                    return true;
                }
            }
        }
        return false;
    };

    uint32_t tid = 0;
    randomGet(&tid, sizeof(tid));
    std::string req, resp;
    put64(req, 0x41727101980ULL);  // protocol magic
    put32(req, 0);                 // action: connect
    put32(req, tid);
    if (!exchange(req, resp, 16) || get32(resp.data()) != 0 || get32(resp.data() + 4) != tid) {
        close(fd);
        return {};
    }
    std::string conn = resp.substr(8, 8);

    randomGet(&tid, sizeof(tid));
    uint32_t key = 0;
    randomGet(&key, sizeof(key));
    req = conn;
    put32(req, 1);  // action: announce
    put32(req, tid);
    req.append((const char*)hash, 20);
    req.append((const char*)peerId, 20);
    put64(req, 0);  // downloaded
    put64(req, 1);  // left
    put64(req, 0);  // uploaded
    put32(req, 2);  // event: started
    put32(req, 0);  // ip
    put32(req, key);
    put32(req, 0xFFFFFFFF);  // num_want: default
    req += (char)(6881 >> 8);
    req += (char)(6881 & 0xFF);
    bool ok = exchange(req, resp, 20) && get32(resp.data()) == 1 && get32(resp.data() + 4) == tid;
    close(fd);
    return ok ? parseCompact(resp.substr(20)) : std::vector<Endpoint>();
}

// uTP (BEP 29) connection state: a reliable byte stream over UDP, used when TCP fails.
struct UtpConn {
    uint16_t recvId = 0, sendId = 0, seq = 1, ack = 0;
    bool connected = false;
    uint32_t peerWnd = 1 << 20, replyDelay = 0;
    struct Out {
        uint16_t seq;
        std::string pkt;
        uint64_t sentAt;
        int tries;
    };
    std::deque<Out> unacked;
    std::map<uint16_t, std::string> ooo;  // out-of-order payloads by sequence number
    size_t inFlight() const {
        size_t n = 0;
        for (auto& o : unacked) n += o.pkt.size();
        return n;
    }
};

struct Peer {
    int fd = -1;
    Endpoint ep;
    int state = 0;  // 0 connecting, 1 handshake sent, 2 ready
    bool dead = false;
    uint64_t since = 0, lastRecv = 0;
    std::string in, out;
    bool choked = true, interestedSent = false;
    bool amChoking = true, peerInterested = false;  // our side of the choke protocol
    uint64_t chokedSince = 0, lastSend = 0;
    int64_t recvWindow = 0;  // bytes they sent us since the last rechoke
    std::vector<uint8_t> bits;  // raw bitfield (kept even before we know the piece count)
    std::shared_ptr<UtpConn> utp;  // set for uTP peers (fd is -1)
    int utMetadata = 0, utPex = 0;
    struct Req {
        int piece, block;
        uint64_t at;
    };
    std::vector<Req> reqs;

    bool has(int p) const {
        size_t b = (size_t)p / 8;
        return b < bits.size() && (bits[b] & (0x80 >> (p % 8)));
    }
    void setHave(int p) {
        size_t b = (size_t)p / 8;
        if (bits.size() <= b) bits.resize(b + 1);
        bits[b] |= (uint8_t)(0x80 >> (p % 8));
    }
    bool requested(int piece, int block) const {
        for (auto& r : reqs)
            if (r.piece == piece && r.block == block) return true;
        return false;
    }
};

struct PieceBuf {
    std::string data;
    std::vector<uint8_t> blocks;  // 0 missing, 1 requested, 2 received
    std::vector<uint64_t> askedAt;  // when each block was last requested
    int received = 0;
};

}  // namespace

struct Torrent::Impl : std::enable_shared_from_this<Torrent::Impl> {
    uint8_t infoHash[20] = {};
    uint8_t peerId[20] = {};
    std::string hashHex;
    std::vector<std::string> trackers;
    int wantFile = -1;

    std::mutex m;
    std::condition_variable cv;
    std::atomic<bool> stopping{false};
    bool started = false, engineDone = false;
    std::string error;

    // Saving the selected file to the SD card (optional)
    std::string saveDir, saveInfo, saveError;
    std::string dataPath, sidecarPath;
    bool saving = false, complete = false, sidecarDirty = false;
    bool pendingAlloc = false, resumedSave = false;
    FILE* fp = nullptr;
    std::mutex fileMutex;          // guards fp: the engine writes, the player reads
    std::vector<uint8_t> saved;    // per piece: 1 = its part of the file is on the SD card
    int64_t savedBytes = 0;
    uint64_t lastSidecar = 0;
    std::deque<std::pair<int, std::string>> writeQueue;  // verified pieces waiting to be written

    std::atomic<int> announcing{0};  // tracker threads still running
    double prefetchAt = -1;          // resume point as a fraction of the file, fetched early

    std::set<Endpoint> known;
    std::deque<Endpoint> candidates;
    std::map<Endpoint, int> attempts;
    std::list<Peer> peers;
    uint64_t lastAnnounce = 0, lastMetaReq = 0, lastRechoke = 0, lastOptimistic = 0, lastDiag = 0;
    uint64_t lastDhtLookup = 0;
    int64_t uploaded = 0;
    std::atomic<bool> yielding{false};  // see Torrent::yieldNetwork
    std::atomic<bool> watched{true};    // see Torrent::setWatched
    int64_t budget() const { return watched ? kCacheBudget : kBackgroundBudget; }
    bool wasYielding = false;
    std::atomic<int> uploadMode{1};  // 0 off, 1 limited, 2 normal (Settings > Torrent uploading)
    uint64_t upWindowStart = 0;
    int64_t upWindowBytes = 0;

    // uTP and DHT share nothing with the TCP sockets: one UDP socket each.
    int utpFd = -1, dhtFd = -1;
    std::set<Endpoint> tryUtp, utpTried;  // TCP failed: try these over uTP once
    uint8_t dhtId[20] = {};
    struct DhtNode {
        uint8_t id[20];
        Endpoint ep;
        bool queried = false;
    };
    std::vector<DhtNode> dhtNodes;
    std::set<Endpoint> dhtKnown;
    std::map<std::string, uint64_t> dhtPending;  // transaction id -> sent time
    uint16_t dhtTx = 0;
    int dhtPeersFound = 0;
    Endpoint optimistic;

    bool haveMeta = false;
    int64_t metaSize = 0;
    std::string metaBuf;
    std::vector<bool> metaGot;

    int64_t pieceLen = 0, totalLen = 0;
    int numPieces = 0;
    std::string hashes;
    std::string fileName;
    int64_t fileOff = 0, fileLen = 0;
    int firstPiece = 0, lastPiece = 0;

    std::map<int, std::string> cache;  // verified pieces
    int64_t cacheBytes = 0;
    std::map<int, PieceBuf> active;  // pieces being downloaded
    int64_t activeBytes = 0;
    int head = 0;  // piece the player is reading
    std::vector<int> order;

    int64_t downloaded = 0, speedBase = 0;
    double speed = 0;
    uint64_t speedAt = 0;

    // ------------------------------------------------------------ peers & trackers

    void addPeers(const std::vector<Endpoint>& eps) {
        for (auto& e : eps)
            if (known.insert(e).second) candidates.push_back(e);
    }

    void announceAll() {
        lastAnnounce = nowMs();
        if (announcing > 0) return;  // the last round is still going
        auto urls = std::make_shared<std::vector<std::string>>();
        for (auto& url : trackers)
            if (url.compare(0, 6, "udp://") == 0 || url.compare(0, 4, "http") == 0) urls->push_back(url);
        auto next = std::make_shared<std::atomic<size_t>>(0);
        auto self = shared_from_this();
        for (int w = 0; w < kAnnounceThreads && w < (int)urls->size(); w++) {
            announcing++;
            bool ok = startThread(
                [self, urls, next]() {
                    for (size_t i; !self->stopping && (i = (*next)++) < urls->size();) {
                        const std::string& url = (*urls)[i];
                        std::vector<Endpoint> eps = url.compare(0, 6, "udp://") == 0
                                                        ? udpAnnounce(url, self->infoHash, self->peerId)
                                                        : httpAnnounce(url, self->infoHash, self->peerId);
                        std::lock_guard<std::mutex> lock(self->m);
                        if (!eps.empty()) logMsg("torrent: %s gave %zu peers", url.c_str(), eps.size());
                        self->addPeers(eps);
                    }
                    self->announcing--;
                },
                256 * 1024);
            if (!ok) announcing--;
        }
    }

    int countState(int st) const {
        int n = 0;
        for (auto& p : peers)
            if (p.state == st && !p.dead) n++;
        return n;
    }

    void connectMore(uint64_t now) {
        if (yielding) return;  // the app needs the network now: no new connections either
        size_t maxPeers = watched ? kMaxPeers : kBackgroundPeers;
        int connecting = countState(0);
        while (connecting < kMaxConnecting && peers.size() < maxPeers && !candidates.empty()) {
            Endpoint e = candidates.front();
            candidates.pop_front();
            if (attempts[e]++ >= 2) continue;
            if (tryUtp.count(e)) {  // TCP failed before: try uTP
                tryUtp.erase(e);
                utpTried.insert(e);
                utpConnect(e, now);
                connecting++;
                continue;
            }
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) break;
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &kPeerRecvBuf, sizeof(kPeerRecvBuf));
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &kPeerSendBuf, sizeof(kPeerSendBuf));
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(e.port);
            a.sin_addr.s_addr = e.ip;
            int rc = connect(fd, (sockaddr*)&a, sizeof(a));
            if (rc < 0 && errno != EINPROGRESS) {
                close(fd);
                continue;
            }
            Peer p;
            p.fd = fd;
            p.ep = e;
            p.since = p.lastRecv = now;
            peers.push_back(std::move(p));
            connecting++;
        }
    }

    // ------------------------------------------------------------ wire protocol

    void sendMsg(Peer& p, uint8_t id, const std::string& payload) {
        put32(p.out, (uint32_t)(1 + payload.size()));
        p.out += (char)id;
        p.out += payload;
    }

    void sendExt(Peer& p, uint8_t extId, const std::string& body) {
        std::string pl(1, (char)extId);
        pl += body;
        sendMsg(p, 20, pl);
    }

    void sendHandshake(Peer& p) {
        p.out += (char)19;
        p.out += "BitTorrent protocol";
        char reserved[8] = {0};
        reserved[5] = 0x10;  // extension protocol (BEP 10)
        p.out.append(reserved, 8);
        p.out.append((const char*)infoHash, 20);
        p.out.append((const char*)peerId, 20);
    }

    void sendExtHandshake(Peer& p) {
        BValue h = BValue::dictionary();
        BValue mm = BValue::dictionary();
        mm.dict["ut_metadata"] = BValue::integer(1);
        mm.dict["ut_pex"] = BValue::integer(2);
        h.dict["m"] = mm;
        h.dict["v"] = BValue::string("StremNX 0.1");
        sendExt(p, 0, bencode(h));
    }

    void requestMetadata(Peer& p) {
        for (size_t i = 0; i < metaGot.size(); i++) {
            if (metaGot[i]) continue;
            BValue r = BValue::dictionary();
            r.dict["msg_type"] = BValue::integer(0);
            r.dict["piece"] = BValue::integer((int64_t)i);
            sendExt(p, (uint8_t)p.utMetadata, bencode(r));
        }
    }

    void flush(Peer& p) {
        if (p.utp) {
            utpFlush(p, nowMs());
            return;
        }
        while (!p.out.empty()) {
            ssize_t n = send(p.fd, p.out.data(), p.out.size(), 0);
            if (n > 0) {
                p.out.erase(0, (size_t)n);
                p.lastSend = nowMs();
            } else {
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) p.dead = true;
                return;
            }
        }
    }

    int64_t pieceSize(int piece) const {
        return piece == numPieces - 1 ? totalLen - (int64_t)piece * pieceLen : pieceLen;
    }

    // ------------------------------------------------------------ metadata (BEP 9)

    void finishMetadata() {
        uint8_t h[20];
        sha1CalculateHash(h, metaBuf.data(), metaBuf.size());
        if (memcmp(h, infoHash, 20) != 0) {
            logMsg("torrent: metadata hash mismatch, retrying");
            std::fill(metaGot.begin(), metaGot.end(), false);
            return;
        }
        if (!saveDir.empty() && !fileExists(metaPath())) {
            ensureDir(saveDir);
            writeFile(metaPath(), metaBuf.data(), metaBuf.size());
        }
        size_t pos = 0;
        BValue info;
        if (!bdecode(metaBuf, pos, info) || info.type != BValue::Dict) {
            error = "invalid torrent metadata";
            return;
        }
        pieceLen = info.getInt("piece length");
        hashes = info.getStr("pieces");
        numPieces = (int)(hashes.size() / 20);
        if (pieceLen <= 0 || numPieces == 0) {
            error = "invalid torrent metadata";
            return;
        }

        struct File {
            std::string path;
            int64_t len = 0, off = 0;
        };
        std::vector<File> files;
        const BValue* list = info.get("files");
        if (list && list->type == BValue::List) {
            int64_t off = 0;
            for (auto& f : list->list) {
                File x;
                x.len = f.getInt("length");
                x.off = off;
                off += x.len;
                const BValue* path = f.get("path");
                if (path)
                    for (auto& part : path->list) {
                        if (!x.path.empty()) x.path += "/";
                        x.path += part.s;
                    }
                files.push_back(x);
            }
            totalLen = off;
        } else {
            File x;
            x.path = info.getStr("name");
            x.len = info.getInt("length");
            files.push_back(x);
            totalLen = x.len;
        }

        int idx = wantFile;
        if (idx < 0 || idx >= (int)files.size()) {
            idx = 0;
            for (int i = 1; i < (int)files.size(); i++)
                if (files[i].len > files[idx].len) idx = i;
        }
        fileName = files[idx].path;
        fileOff = files[idx].off;
        fileLen = files[idx].len;
        if (fileLen <= 0) {
            error = "selected file is empty";
            return;
        }
        firstPiece = (int)(fileOff / pieceLen);
        lastPiece = (int)((fileOff + fileLen - 1) / pieceLen);
        head = firstPiece;
        haveMeta = true;
        metaBuf.clear();
        metaBuf.shrink_to_fit();
        logMsg("torrent: metadata ok: %s, %lld bytes, %d pieces of %lld", fileName.c_str(), (long long)fileLen,
               numPieces, (long long)pieceLen);
        setupSave();
        cv.notify_all();
    }

    // ------------------------------------------------------------ SD card saving

    // The info dict, kept next to the download so it can be opened again without asking peers.
    std::string metaPath() const { return saveDir + "/" + hashHex + ".info"; }

    bool isSaved(int piece) const { return piece >= 0 && piece < (int)saved.size() && saved[piece]; }

    // Bytes of `piece` that belong to the selected file.
    void pieceOverlap(int piece, int64_t& a, int64_t& b) const {
        int64_t g0 = (int64_t)piece * pieceLen;
        a = std::max(g0, fileOff);
        b = std::min(g0 + pieceSize(piece), fileOff + fileLen);
    }

    static std::string sanitize(const std::string& name) {
        std::string base = name.substr(name.rfind('/') + 1), out;
        for (char c : base) out += (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == ' ') ? c : '_';
        if (out.size() > 90) out = out.substr(out.size() - 90);
        return out.empty() ? "video" : out;
    }

    std::string savedHex() const {
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (int i = firstPiece; i <= lastPiece; i += 4) {
            int v = 0;
            for (int k = 0; k < 4; k++)
                if (i + k <= lastPiece && isSaved(i + k)) v |= 8 >> k;
            out += hex[v];
        }
        return out;
    }

    void loadSavedHex(const std::string& h) {
        for (int i = firstPiece, c = 0; i <= lastPiece && c < (int)h.size(); i += 4, c++) {
            char ch = h[c];
            int v = ch >= 'a' ? ch - 'a' + 10 : ch - '0';
            for (int k = 0; k < 4 && i + k <= lastPiece; k++)
                if (v & (8 >> k)) saved[i + k] = 1;
        }
    }

    void writeSidecar() {
        bool ok = false;
        Json j = Json::parse(saveInfo, &ok);
        if (!ok || !j.isObject()) j = Json::object();
        j.set("infoHash", hashHex).set("fileIdx", wantFile).set("fileName", fileName).set("file", dataPath);
        j.set("size", (double)fileLen).set("pieceLen", (double)pieceLen).set("savedBytes", (double)savedBytes);
        j.set("complete", complete).set("saved", savedHex()).set("updated", (double)time(nullptr));
        std::string text = j.dump();
        writeFile(sidecarPath, text.data(), text.size());
        sidecarDirty = false;
        lastSidecar = nowMs();
    }

    void recountSaved() {
        savedBytes = 0;
        complete = true;
        for (int i = firstPiece; i <= lastPiece; i++) {
            if (!isSaved(i)) {
                complete = false;
                continue;
            }
            int64_t a, b;
            pieceOverlap(i, a, b);
            savedBytes += b - a;
        }
    }

    void setupSave() {
        if (saveDir.empty()) return;
        ensureDir(saveDir);
        saved.assign((size_t)numPieces, 0);

        bool resumed = false;
        std::string text;
        if (readFile(sidecarPath, text)) {
            bool ok = false;
            Json j = Json::parse(text, &ok);
            if (ok && (int64_t)j["size"].num() == fileLen && (int64_t)j["pieceLen"].num() == pieceLen &&
                fileExists(j["file"].str())) {
                dataPath = j["file"].str();
                loadSavedHex(j["saved"].str());
                resumed = true;
            }
        }
        if (!resumed) {
            std::fill(saved.begin(), saved.end(), 0);
            dataPath = saveDir + "/" + hashHex.substr(0, 8) + "_" + sanitize(fileName);
        }
        recountSaved();

        s64 freeSpace = 0;
        if (FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc")) fsFsGetFreeSpace(fs, "/", &freeSpace);
        int64_t need = resumed ? 0 : fileLen;
        if (freeSpace > 0 && freeSpace < need + (64LL << 20)) {
            saveError = "Not enough space on the SD card";
            saved.clear();
            logMsg("torrent: %s (%lld MB free)", saveError.c_str(), (long long)(freeSpace >> 20));
            return;
        }

        // Opening and sizing the file can take a while on big files: done by allocateFile()
        // outside the main lock. Pieces that arrive meanwhile wait in writeQueue.
        pendingAlloc = true;
        resumedSave = resumed;
    }

    void allocateFile() {
        std::string path;
        bool resumed;
        int64_t len;
        {
            std::lock_guard<std::mutex> lock(m);
            if (!pendingAlloc) return;
            pendingAlloc = false;
            path = dataPath;
            resumed = resumedSave;
            len = fileLen;
        }
        std::string err;
        // FAT32 caps files at 4 GB. A "concatenation file" is stored in parts by the system but read and
        // written as one file by everyone (mpv included), on FAT32 and exFAT alike.
        bool big = !resumed && len >= (4LL << 30);
        if (big) {
            remove(path.c_str());
            FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
            std::string fsPath = path.compare(0, 5, "sdmc:") == 0 ? path.substr(5) : path;
            if (!fs || R_FAILED(fsFsCreateFile(fs, fsPath.c_str(), len, FsCreateOption_BigFile)))
                err = "Can't create the file on the SD card";
        }
        FILE* f = err.empty() ? fopen(path.c_str(), resumed || big ? "r+b" : "w+b") : nullptr;
        if (!f) {
            if (err.empty()) err = "Can't create the file on the SD card";
        } else if (!resumed && !big) {
            uint64_t t0 = nowMs();
            if (ftruncate(fileno(f), (off_t)len) != 0) {
                err = "Can't allocate the file on the SD card";
                fclose(f);
                f = nullptr;
                remove(path.c_str());
            } else {
                logMsg("torrent: allocated %lld MB on the SD card in %llu ms", (long long)(len >> 20),
                       (unsigned long long)(nowMs() - t0));
            }
        }
        {
            std::lock_guard<std::mutex> fl(fileMutex);
            fp = f;
        }
        std::lock_guard<std::mutex> lock(m);
        if (!err.empty()) {
            saveError = err;
            saved.clear();
            writeQueue.clear();
            logMsg("torrent: %s", saveError.c_str());
            return;
        }
        saving = true;
        logMsg("torrent: saving to %s (%s, %lld/%lld bytes already there)", dataPath.c_str(),
               resumed ? "resumed" : "new", (long long)savedBytes, (long long)fileLen);
        writeSidecar();
    }

    // Writes queued pieces outside the main lock so the player never waits on the SD card.
    void flushWrites(bool all) {
        for (int i = 0; all || i < 4; i++) {
            std::pair<int, std::string> job;
            {
                std::lock_guard<std::mutex> lock(m);
                if (writeQueue.empty() || !fp) break;
                job = std::move(writeQueue.front());
                writeQueue.pop_front();
            }
            int64_t a, b;
            pieceOverlap(job.first, a, b);
            int64_t g0 = (int64_t)job.first * pieceLen;
            bool ok = true;
            if (a < b) {
                std::lock_guard<std::mutex> fl(fileMutex);
                ok = fseek(fp, (long)(a - fileOff), SEEK_SET) == 0 &&
                     fwrite(job.second.data() + (a - g0), 1, (size_t)(b - a), fp) == (size_t)(b - a);
            }
            std::lock_guard<std::mutex> lock(m);
            if (!ok) {
                saveError = "Writing to the SD card failed";
                saving = false;
                logMsg("torrent: %s", saveError.c_str());
                break;
            }
            if (!saved[job.first]) {
                saved[job.first] = 1;
                savedBytes += b - a;
                sidecarDirty = true;
            }
        }
        std::lock_guard<std::mutex> lock(m);
        if (saving && sidecarDirty) {
            bool wasComplete = complete;
            recountSaved();
            if (complete || all || nowMs() - lastSidecar > 5000) {
                {
                    std::lock_guard<std::mutex> fl(fileMutex);
                    if (fp) fflush(fp);
                }
                writeSidecar();
            }
            if (complete && !wasComplete) logMsg("torrent: download complete: %s", dataPath.c_str());
        }
    }

    void onExtended(Peer& p, uint8_t extId, const std::string& payload) {
        size_t pos = 0;
        BValue d;
        if (!bdecode(payload, pos, d) || d.type != BValue::Dict) return;

        if (extId == 0) {  // extended handshake
            if (const BValue* mm = d.get("m")) {
                p.utMetadata = (int)mm->getInt("ut_metadata", 0);
                p.utPex = (int)mm->getInt("ut_pex", 0);
            }
            int64_t ms = d.getInt("metadata_size", 0);
            if (!haveMeta && metaSize == 0 && ms > 0 && ms < (16 << 20)) {
                metaSize = ms;
                metaBuf.assign((size_t)ms, '\0');
                metaGot.assign((size_t)((ms + kBlock - 1) / kBlock), false);
            }
            if (!haveMeta && p.utMetadata && metaSize > 0) requestMetadata(p);
            return;
        }
        if (extId == 1) {  // ut_metadata (our local id)
            int type = (int)d.getInt("msg_type", -1);
            int piece = (int)d.getInt("piece", -1);
            if (type != 1 || haveMeta || metaSize == 0 || piece < 0 || piece >= (int)metaGot.size()) return;
            size_t off = (size_t)piece * kBlock;
            size_t want = (size_t)std::min<int64_t>(kBlock, metaSize - (int64_t)off);
            if (payload.size() - pos < want) return;
            memcpy(&metaBuf[off], payload.data() + pos, want);
            metaGot[piece] = true;
            if (std::all_of(metaGot.begin(), metaGot.end(), [](bool b) { return b; })) finishMetadata();
            return;
        }
        if (extId == 2) {  // ut_pex (our local id)
            const BValue* added = d.get("added");
            if (added && added->type == BValue::Str) addPeers(parseCompact(added->s));
        }
    }

    // ------------------------------------------------------------ pieces

    void evict(int64_t need) {
        while (cacheBytes + activeBytes + need > budget() && !cache.empty()) {
            int victim = -1;
            int64_t best = -1;
            for (auto& kv : cache) {
                int pc = kv.first;
                if (pc >= head && pc <= head + 1) continue;
                // Pieces already on the SD card can always be read back from there: drop them first.
                if (!isSaved(pc) && (pc >= lastPiece - 1 || pc == firstPiece)) continue;
                int64_t score = pc < head ? (int64_t)(head - pc) * 4 + 1000000 : pc - head;
                if (isSaved(pc)) score += 100000000;
                if (score > best) {
                    best = score;
                    victim = pc;
                }
            }
            if (victim < 0) break;
            cacheBytes -= (int64_t)cache[victim].size();
            cache.erase(victim);
        }
    }

    PieceBuf* getActive(int piece) {
        auto it = active.find(piece);
        if (it != active.end()) return &it->second;
        int64_t sz = pieceSize(piece);
        bool urgent = piece == head || piece == head + 1;
        if (cacheBytes + activeBytes + sz > budget()) {
            evict(sz);
            if (!urgent && cacheBytes + activeBytes + sz > budget()) return nullptr;
        }
        PieceBuf& pb = active[piece];
        pb.data.assign((size_t)sz, '\0');
        pb.blocks.assign((size_t)((sz + kBlock - 1) / kBlock), 0);
        pb.askedAt.assign(pb.blocks.size(), 0);
        activeBytes += sz;
        return &pb;
    }

    void buildOrder() {
        order.clear();
        if (!haveMeta) return;
        int win = std::max<int>(3, (int)(kReadAhead / pieceLen));
        int end = std::min(lastPiece, head + win - 1);
        // What the player needs right now, then the file's tail (MKV cues / MP4 moov live there),
        // then the rest of the read-ahead window.
        for (int i = head; i <= std::min(end, head + 1); i++) order.push_back(i);
        for (int t = lastPiece; t >= std::max(firstPiece, lastPiece - 1); t--)
            if (t > head + 1) order.push_back(t);
        if (prefetchAt >= 0) {  // resuming: where the player will seek to, before it asks
            int64_t at = fileOff + std::max<int64_t>(0, (int64_t)(prefetchAt * fileLen) - kPrefetch / 4);
            int p0 = (int)(at / pieceLen), p1 = std::min(lastPiece, (int)((at + kPrefetch) / pieceLen));
            if (head >= p0)
                prefetchAt = -1;  // the player got there
            else
                for (int i = p0; i <= p1; i++) order.push_back(i);
        }
        bool windowDone = true;  // the next ~64 MB of playback are all here
        for (int i = head + 2; i <= end; i++) {
            if (i >= lastPiece - 1) continue;
            order.push_back(i);
            if (!cache.count(i) && !isSaved(i)) windowDone = false;
        }
        if (firstPiece < head) order.push_back(firstPiece);  // container headers
        // Beyond the window: while someone watches, new pieces there start only once the window is
        // complete, so they never take bandwidth (or RAM) from what plays next. Pieces already started
        // are finished, since they hold RAM. A download nobody watches takes everything.
        auto beyond = [&](int i) { return windowDone || !watched || active.count(i); };
        if (saving || pendingAlloc) {  // downloading the whole file: everything after the window, then what's before it
            for (int i = end + 1; i < lastPiece - 1; i++)
                if (!isSaved(i) && beyond(i)) order.push_back(i);
            for (int i = firstPiece; i < head; i++)
                if (!isSaved(i) && beyond(i)) order.push_back(i);
            return;
        }
        int64_t budgetPieces = budget() / pieceLen;
        for (int i = end + 1; i < lastPiece - 1 && (i - head) < budgetPieces * 3 / 4; i++)
            if (beyond(i)) order.push_back(i);
    }

    void sendRequest(Peer& p, int piece, int block, uint64_t now) {
        int64_t begin = (int64_t)block * kBlock;
        int64_t len = std::min<int64_t>(kBlock, pieceSize(piece) - begin);
        std::string pl;
        put32(pl, (uint32_t)piece);
        put32(pl, (uint32_t)begin);
        put32(pl, (uint32_t)len);
        sendMsg(p, 6, pl);
        p.reqs.push_back({piece, block, now});
    }

    void fillRequests(Peer& p, uint64_t now) {
        // First what playback is blocked on (the next ~8 MB and the file tail): blocks another
        // peer has sat on for a while are asked from this one too. This must come before the pipeline
        // fills with the rest, or one slow peer holding the first blocks delays the start while the
        // others download hundreds of MB further ahead.
        int dup = 0, urgentEnd = head + std::max<int>(1, (int)(kUrgent / pieceLen));
        for (size_t k = 0; k < order.size() && k < 64 && dup < 8; k++) {
            bool urgent = (order[k] >= head && order[k] <= urgentEnd) || order[k] >= lastPiece - 1;
            if (!urgent) continue;
            auto it = active.find(order[k]);
            if (it == active.end() || !p.has(order[k])) continue;
            PieceBuf& pb = it->second;
            for (int b = 0; b < (int)pb.blocks.size() && dup < 8 && p.reqs.size() < kPipeline; b++) {
                if (pb.blocks[b] == 1 && now - pb.askedAt[b] > kStallMs && !p.requested(order[k], b)) {
                    sendRequest(p, order[k], b, now);
                    pb.askedAt[b] = now;
                    dup++;
                }
            }
        }
        for (int piece : order) {
            if (p.reqs.size() >= kPipeline) return;
            if (cache.count(piece) || isSaved(piece) || !p.has(piece)) continue;
            PieceBuf* pb = getActive(piece);
            if (!pb) break;  // RAM budget full: the later (less urgent) pieces would not fit either
            // The file's last piece is fetched back to front: its final bytes matter first.
            bool backwards = piece == lastPiece && piece != head;
            int nb = (int)pb->blocks.size();
            for (int k = 0; k < nb && p.reqs.size() < kPipeline; k++) {
                int b = backwards ? nb - 1 - k : k;
                if (pb->blocks[b] != 0) continue;
                pb->blocks[b] = 1;
                pb->askedAt[b] = now;
                sendRequest(p, piece, b, now);
            }
        }
    }

    void releaseBlock(int piece, int block) {
        auto it = active.find(piece);
        if (it != active.end() && block < (int)it->second.blocks.size() && it->second.blocks[block] == 1)
            it->second.blocks[block] = 0;
    }

    void releaseRequests(Peer& p) {
        for (auto& r : p.reqs) releaseBlock(r.piece, r.block);
        p.reqs.clear();
    }

    void expireRequests(Peer& p, uint64_t now) {
        auto it = std::remove_if(p.reqs.begin(), p.reqs.end(), [&](const Peer::Req& r) {
            if (now - r.at < kBlockTimeoutMs) return false;
            releaseBlock(r.piece, r.block);
            return true;
        });
        p.reqs.erase(it, p.reqs.end());
    }

    void onBlock(Peer& p, int piece, int64_t begin, const char* data, size_t len) {
        int b = (int)(begin / kBlock);
        auto rit = std::remove_if(p.reqs.begin(), p.reqs.end(),
                                  [&](const Peer::Req& r) { return r.piece == piece && r.block == b; });
        p.reqs.erase(rit, p.reqs.end());
        downloaded += (int64_t)len;
        p.recvWindow += (int64_t)len;

        auto it = active.find(piece);
        if (it == active.end()) return;
        PieceBuf& pb = it->second;
        if (b < 0 || b >= (int)pb.blocks.size() || pb.blocks[b] == 2) return;
        if (begin + (int64_t)len > (int64_t)pb.data.size()) return;
        memcpy(&pb.data[(size_t)begin], data, len);
        pb.blocks[b] = 2;
        pb.received++;
        cv.notify_all();  // the player can read blocks before the whole piece is verified
        if (pb.received < (int)pb.blocks.size()) return;

        uint8_t h[20];
        sha1CalculateHash(h, pb.data.data(), pb.data.size());
        if (memcmp(h, hashes.data() + (size_t)piece * 20, 20) == 0) {
            int64_t sz = (int64_t)pb.data.size();
            activeBytes -= sz;
            cacheBytes += sz;
            cache[piece] = std::move(pb.data);
            active.erase(it);
            if ((saving || pendingAlloc) && !isSaved(piece)) writeQueue.emplace_back(piece, cache[piece]);
            broadcastHave(piece);
            evict(0);
            cv.notify_all();
        } else {
            logMsg("torrent: piece %d failed the hash check", piece);
            std::fill(pb.blocks.begin(), pb.blocks.end(), 0);
            pb.received = 0;
        }
    }

    void handleMessage(Peer& p, uint8_t id, const char* data, size_t n) {
        switch (id) {
            case 0:  // choke: the peer drops our pending requests
                if (!p.choked) p.chokedSince = nowMs();
                p.choked = true;
                releaseRequests(p);
                break;
            case 1: p.choked = false; break;
            case 2: p.peerInterested = true; break;
            case 3: p.peerInterested = false; break;
            case 6:
                if (n >= 12) serveRequest(p, get32(data), get32(data + 4), get32(data + 8));
                break;
            case 4:
                if (n >= 4) {
                    uint32_t idx = get32(data);
                    if (idx < (haveMeta ? (uint32_t)numPieces : (1u << 20))) p.setHave((int)idx);
                }
                break;
            case 5: p.bits.assign((const uint8_t*)data, (const uint8_t*)data + n); break;
            case 7:
                if (n >= 8 && haveMeta) {
                    uint32_t idx = get32(data);
                    if (idx < (uint32_t)numPieces) onBlock(p, (int)idx, get32(data + 4), data + 8, n - 8);
                }
                break;
            case 20:
                if (n >= 1) onExtended(p, (uint8_t)data[0], std::string(data + 1, n - 1));
                break;
            default: break;  // cancel and others: nothing to do
        }
    }

    // ------------------------------------------------------------ uploading (tit-for-tat)
    // Peers mostly send to whoever sends back. Without uploading we'd only get their spare
    // "optimistic" slot and downloads stall after the first seconds.

    void sendHave(Peer& p, int piece) {
        std::string pl;
        put32(pl, (uint32_t)piece);
        sendMsg(p, 4, pl);
    }

    // Tells a newly ready peer what we already have.
    void sendHaves(Peer& p) {
        if (!haveMeta) return;
        for (auto& kv : cache) sendHave(p, kv.first);
        for (int i = 0; i < (int)saved.size(); i++)
            if (saved[i] && !cache.count(i)) sendHave(p, i);
    }

    void broadcastHave(int piece) {
        for (auto& p : peers)
            if (p.state == 2 && !p.dead) sendHave(p, piece);
    }

    void serveRequest(Peer& p, uint32_t piece, uint32_t begin, uint32_t len) {
        if (p.amChoking || !haveMeta || piece >= (uint32_t)numPieces || len == 0 || len > 32768) return;
        if (uploadMode == 0) return;
        if (uploadMode == 1) {  // limited: about 100 KB/s
            uint64_t now = nowMs();
            if (now - upWindowStart >= 1000) {
                upWindowStart = now;
                upWindowBytes = 0;
            }
            if (upWindowBytes + (int64_t)len > kLimitedUploadPerSec) return;  // they will ask again
            upWindowBytes += len;
        }
        if (p.out.size() > (1u << 20)) return;  // they'll ask again
        std::string pl;
        put32(pl, piece);
        put32(pl, begin);
        auto c = cache.find((int)piece);
        if (c != cache.end()) {
            if ((size_t)begin + len > c->second.size()) return;
            pl.append(c->second, begin, len);
        } else if (isSaved((int)piece)) {
            int64_t g = (int64_t)piece * pieceLen + begin;
            if (g < fileOff || g + len > fileOff + fileLen) return;  // only our file's bytes are on disk
            std::lock_guard<std::mutex> fl(fileMutex);
            if (!fp || fseek(fp, (long)(g - fileOff), SEEK_SET) != 0) return;
            size_t at = pl.size();
            pl.resize(at + len);
            if (fread(&pl[at], 1, len, fp) != len) return;
        } else {
            return;
        }
        sendMsg(p, 7, pl);
        uploaded += len;
    }

    // Every 10 s: unchoke the interested peers that send us the most, plus one rotating optimistic.
    void rechoke(uint64_t now) {
        std::vector<Peer*> interested;
        for (auto& p : peers)
            if (p.state == 2 && !p.dead && p.peerInterested) interested.push_back(&p);
        std::sort(interested.begin(), interested.end(),
                  [](const Peer* a, const Peer* b) { return a->recvWindow > b->recvWindow; });
        if (now - lastOptimistic > 30000 && !interested.empty()) {
            lastOptimistic = now;
            uint32_t r = 0;
            randomGet(&r, sizeof(r));
            optimistic = interested[r % interested.size()]->ep;
        }
        for (size_t i = 0; i < interested.size(); i++) {
            Peer& p = *interested[i];
            int slots = uploadMode == 0 ? 0 : uploadMode == 1 ? 2 : kUploadSlots;
            bool unchoke = uploadMode > 0 &&
                           ((int)i < slots || (p.ep.ip == optimistic.ip && p.ep.port == optimistic.port));
            if (unchoke && p.amChoking) sendMsg(p, 1, "");
            if (!unchoke && !p.amChoking) sendMsg(p, 0, "");
            p.amChoking = !unchoke;
        }
        for (auto& p : peers) p.recvWindow = 0;
    }

    void processInput(Peer& p) {
        if (p.state == 1) {
            if (p.in.size() < 68) return;
            if (p.in[0] != 19 || memcmp(p.in.data() + 28, infoHash, 20) != 0) {
                p.dead = true;
                return;
            }
            bool ext = (p.in[25] & 0x10) != 0;
            p.in.erase(0, 68);
            p.state = 2;
            p.chokedSince = nowMs();
            if (ext) sendExtHandshake(p);
            sendHaves(p);
        }
        size_t off = 0;
        while (!p.dead && p.in.size() - off >= 4) {
            uint32_t len = get32(p.in.data() + off);
            if (len > (1u << 20) + 64) {
                p.dead = true;
                break;
            }
            if (p.in.size() - off < 4 + (size_t)len) break;
            if (len > 0) handleMessage(p, (uint8_t)p.in[off + 4], p.in.data() + off + 5, len - 1);
            off += 4 + len;
        }
        if (off) p.in.erase(0, off);
    }

    // ------------------------------------------------------------ reading

    // Bytes readable at torrent offset g: from a verified piece, or from the received blocks of a
    // piece still downloading (so playback can start before whole multi-MB pieces arrive).
    int64_t availableAt(int64_t g, const char** ptr) {
        int piece = (int)(g / pieceLen);
        int64_t in = g - (int64_t)piece * pieceLen;
        auto c = cache.find(piece);
        if (c != cache.end()) {
            *ptr = c->second.data() + in;
            return (int64_t)c->second.size() - in;
        }
        auto a = active.find(piece);
        if (a == active.end()) return 0;
        const PieceBuf& pb = a->second;
        int64_t len = 0;
        for (int b = (int)(in / kBlock); b < (int)pb.blocks.size() && pb.blocks[b] == 2; b++)
            len = std::min<int64_t>((int64_t)(b + 1) * kBlock, (int64_t)pb.data.size()) - in;
        if (len > 0) *ptr = pb.data.data() + in;
        return len;
    }

    int64_t contiguous(int64_t g, int64_t maxLen) {
        int64_t got = 0;
        const char* p = nullptr;
        while (got < maxLen) {
            int64_t a = availableAt(g + got, &p);
            if (a <= 0) {
                int piece = (int)((g + got) / pieceLen);
                if (!isSaved(piece)) break;
                a = std::min((int64_t)(piece + 1) * pieceLen, fileOff + fileLen) - (g + got);  // on the SD card
            }
            got += a;
        }
        return std::min(got, maxLen);
    }

    // ------------------------------------------------------------ UDP helpers

    static int udpSocket() {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return -1;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = 0;  // any port
        a.sin_addr.s_addr = INADDR_ANY;
        bind(fd, (sockaddr*)&a, sizeof(a));
        return fd;
    }

    static void sendUdp(int fd, const Endpoint& ep, const std::string& data) {
        if (fd < 0) return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(ep.port);
        a.sin_addr.s_addr = ep.ip;
        sendto(fd, data.data(), data.size(), 0, (sockaddr*)&a, sizeof(a));
    }

    static bool readable(int fd) {
        pollfd p{fd, POLLIN, 0};
        return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
    }

    static void put16(std::string& s, uint16_t v) {
        s += (char)(v >> 8);
        s += (char)v;
    }

    static uint16_t get16(const char* p) {
        const uint8_t* u = (const uint8_t*)p;
        return (uint16_t)((u[0] << 8) | u[1]);
    }

    // ------------------------------------------------------------ uTP (BEP 29)
    // A small, conservative implementation: fixed window, in-order delivery, retransmit on timeout.

    enum { UtpData = 0, UtpFin = 1, UtpState = 2, UtpReset = 3, UtpSyn = 4 };

    static uint32_t micros() { return (uint32_t)(armTicksToNs(armGetSystemTick()) / 1000); }

    std::string utpHeader(UtpConn& u, int type, uint16_t connId, uint16_t seq) {
        std::string h;
        h += (char)((type << 4) | 1);
        h += (char)0;  // no extensions
        put16(h, connId);
        put32(h, micros());
        put32(h, u.replyDelay);
        put32(h, 1u << 20);  // our receive window
        put16(h, seq);
        put16(h, u.ack);
        return h;
    }

    void utpConnect(const Endpoint& e, uint64_t now) {
        if (utpFd < 0) utpFd = udpSocket();
        if (utpFd < 0) return;
        Peer p;
        p.fd = -1;
        p.ep = e;
        p.since = p.lastRecv = now;
        p.utp = std::make_shared<UtpConn>();
        UtpConn& u = *p.utp;
        randomGet(&u.recvId, sizeof(u.recvId));
        u.sendId = (uint16_t)(u.recvId + 1);
        std::string syn = utpHeader(u, UtpSyn, u.recvId, u.seq);
        sendUdp(utpFd, e, syn);
        u.unacked.push_back({u.seq, syn, now, 1});
        u.seq++;
        peers.push_back(std::move(p));
    }

    void utpSendState(Peer& p) {
        UtpConn& u = *p.utp;
        sendUdp(utpFd, p.ep, utpHeader(u, UtpState, u.sendId, u.seq));
    }

    void utpFlush(Peer& p, uint64_t now) {
        UtpConn& u = *p.utp;
        if (!u.connected) return;
        size_t limit = std::min<size_t>(u.peerWnd, 96 * 1024);
        while (!p.out.empty() && u.inFlight() < limit) {
            size_t k = std::min<size_t>(p.out.size(), 1200);
            std::string pkt = utpHeader(u, UtpData, u.sendId, u.seq) + p.out.substr(0, k);
            sendUdp(utpFd, p.ep, pkt);
            u.unacked.push_back({u.seq, pkt, now, 1});
            u.seq++;
            p.out.erase(0, k);
            p.lastSend = now;
        }
    }

    // Retransmits and timeouts for every uTP peer.
    void utpTimers(uint64_t now) {
        for (auto& p : peers) {
            if (!p.utp || p.dead) continue;
            UtpConn& u = *p.utp;
            if (!u.unacked.empty()) {
                auto& o = u.unacked.front();
                if (now - o.sentAt > 1000u * (uint64_t)o.tries) {
                    if (o.tries >= 6) {
                        p.dead = true;
                        continue;
                    }
                    sendUdp(utpFd, p.ep, o.pkt);
                    o.sentAt = now;
                    o.tries++;
                }
            }
            if (!u.connected && now - p.since > kConnectTimeoutMs) p.dead = true;
        }
    }

    void utpReceive(std::vector<char>& buf, uint64_t now) {
        // Only read what poll says is there: a blocking read here would stall the whole engine
        // (and the screen, which waits on its lock).
        for (int k = 0; k < 256 && (k == 0 || readable(utpFd)); k++) {
            sockaddr_in from{};
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(utpFd, buf.data(), buf.size(), MSG_DONTWAIT, (sockaddr*)&from, &flen);
            if (n < 20) {
                if (n < 0) return;
                continue;
            }
            const char* b = buf.data();
            int type = (uint8_t)b[0] >> 4;
            if (((uint8_t)b[0] & 15) != 1) continue;
            uint16_t connId = get16(b + 2), seq = get16(b + 16), ack = get16(b + 18);
            uint32_t ts = get32(b + 4), wnd = get32(b + 12);
            Peer* pp = nullptr;
            for (auto& p : peers)
                if (p.utp && p.utp->recvId == connId && p.ep.ip == from.sin_addr.s_addr) pp = &p;
            if (!pp) continue;
            Peer& p = *pp;
            UtpConn& u = *p.utp;
            u.replyDelay = micros() - ts;
            u.peerWnd = std::max<uint32_t>(wnd, 1500);
            p.lastRecv = now;
            while (!u.unacked.empty() && (int16_t)(ack - u.unacked.front().seq) >= 0) u.unacked.pop_front();

            size_t pos = 20;  // skip extension headers
            int ext = (uint8_t)b[1];
            while (ext && pos + 2 <= (size_t)n) {
                int next = (uint8_t)b[pos], len = (uint8_t)b[pos + 1];
                pos += 2 + len;
                ext = next;
            }
            if (pos > (size_t)n) continue;

            if (type == UtpFin || type == UtpReset) {
                p.dead = true;
            } else if (type == UtpState && !u.connected) {
                u.connected = true;
                u.ack = (uint16_t)(seq - 1);
                p.state = 1;
                p.since = now;
                sendHandshake(p);
            } else if (type == UtpData && u.connected) {
                std::string payload(b + pos, (size_t)n - pos);
                if (seq == (uint16_t)(u.ack + 1)) {
                    p.in += payload;
                    u.ack = seq;
                    for (auto it = u.ooo.find((uint16_t)(u.ack + 1)); it != u.ooo.end();
                         it = u.ooo.find((uint16_t)(u.ack + 1))) {
                        p.in += it->second;
                        u.ack = it->first;
                        u.ooo.erase(it);
                    }
                } else if ((int16_t)(seq - u.ack) > 1 && u.ooo.size() < 512) {
                    u.ooo[seq] = payload;
                }
                utpSendState(p);
                if (p.state >= 1 && !p.in.empty()) processInput(p);
            }
        }
    }

    // ------------------------------------------------------------ DHT (BEP 5)
    // Only the "get_peers" lookup for our info hash, plus polite answers to pings.

    void dhtStart() {
        dhtFd = udpSocket();
        if (dhtFd < 0) return;
        randomGet(dhtId, sizeof(dhtId));
        auto self = shared_from_this();
        startThread(
            [self]() {
                static const char* const hosts[][2] = {{"router.bittorrent.com", "6881"},
                                                       {"dht.transmissionbt.com", "6881"},
                                                       {"router.utorrent.com", "6881"},
                                                       {"dht.libtorrent.org", "25401"}};
                for (auto& h : hosts) {
                    addrinfo hints{};
                    hints.ai_family = AF_INET;
                    hints.ai_socktype = SOCK_DGRAM;
                    addrinfo* res = nullptr;
                    if (getaddrinfo(h[0], h[1], &hints, &res) != 0 || !res) continue;
                    Endpoint e;
                    e.ip = ((sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
                    e.port = (uint16_t)atoi(h[1]);
                    freeaddrinfo(res);
                    std::lock_guard<std::mutex> lock(self->m);
                    uint8_t zero[20] = {};
                    self->dhtAddNode(zero, e);
                }
            },
            256 * 1024);
    }

    void dhtAddNode(const uint8_t* id, const Endpoint& e) {
        if (!e.ip || !e.port || dhtNodes.size() >= 500) return;
        if (!dhtKnown.insert(e).second) return;
        DhtNode n;
        memcpy(n.id, id, 20);
        n.ep = e;
        dhtNodes.push_back(n);
    }

    // XOR distance to our info hash: is a closer than b?
    bool closer(const uint8_t* a, const uint8_t* b) const {
        for (int i = 0; i < 20; i++) {
            uint8_t da = a[i] ^ infoHash[i], db = b[i] ^ infoHash[i];
            if (da != db) return da < db;
        }
        return false;
    }

    void dhtQuery(DhtNode& n, uint64_t now) {
        std::string tx;
        put16(tx, dhtTx++);
        BValue msg = BValue::dictionary(), a = BValue::dictionary();
        a.dict["id"] = BValue::string(std::string((const char*)dhtId, 20));
        a.dict["info_hash"] = BValue::string(std::string((const char*)infoHash, 20));
        msg.dict["a"] = a;
        msg.dict["q"] = BValue::string("get_peers");
        msg.dict["t"] = BValue::string(tx);
        msg.dict["y"] = BValue::string("q");
        sendUdp(dhtFd, n.ep, bencode(msg));
        dhtPending[tx] = now;
        n.queried = true;
    }

    void dhtStep(uint64_t now) {
        if (dhtFd < 0) return;
        for (auto it = dhtPending.begin(); it != dhtPending.end();)
            it = now - it->second > 4000 ? dhtPending.erase(it) : std::next(it);
        // New lookup round: every minute while peers are short, every 5 minutes otherwise.
        uint64_t every = (countState(2) < 15 || (haveMeta && speed < 500 * 1024)) ? 60000 : 300000;
        if (now - lastDhtLookup > every) {
            lastDhtLookup = now;
            for (auto& n : dhtNodes) n.queried = false;
        }
        while (dhtPending.size() < 6) {
            DhtNode* best = nullptr;
            for (auto& n : dhtNodes)
                if (!n.queried && (!best || closer(n.id, best->id))) best = &n;
            if (!best) break;
            dhtQuery(*best, now);
        }
    }

    void dhtReceive(std::vector<char>& buf) {
        for (int k = 0; k < 256 && (k == 0 || readable(dhtFd)); k++) {
            sockaddr_in from{};
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(dhtFd, buf.data(), buf.size(), MSG_DONTWAIT, (sockaddr*)&from, &flen);
            if (n <= 0) return;
            std::string data(buf.data(), (size_t)n);
            size_t pos = 0;
            BValue msg;
            if (!bdecode(data, pos, msg) || msg.type != BValue::Dict) continue;
            std::string y = msg.getStr("y"), t = msg.getStr("t");
            Endpoint src;
            src.ip = from.sin_addr.s_addr;
            src.port = ntohs(from.sin_port);
            if (y == "r") {
                dhtPending.erase(t);
                const BValue* r = msg.get("r");
                if (!r) continue;
                if (const BValue* values = r->get("values")) {
                    for (auto& v : values->list) {
                        auto eps = parseCompact(v.s);
                        dhtPeersFound += (int)eps.size();
                        addPeers(eps);
                    }
                }
                std::string nodes = r->getStr("nodes");
                for (size_t i = 0; i + 26 <= nodes.size(); i += 26) {
                    Endpoint e;
                    memcpy(&e.ip, nodes.data() + i + 20, 4);
                    e.port = (uint16_t)(((uint8_t)nodes[i + 24] << 8) | (uint8_t)nodes[i + 25]);
                    dhtAddNode((const uint8_t*)nodes.data() + i, e);
                }
                std::string id = r->getStr("id");
                if (id.size() == 20) dhtAddNode((const uint8_t*)id.data(), src);
            } else if (y == "q") {  // be a polite node: answer with our id
                BValue reply = BValue::dictionary(), r = BValue::dictionary();
                r.dict["id"] = BValue::string(std::string((const char*)dhtId, 20));
                std::string q = msg.getStr("q");
                if (q == "get_peers") {
                    r.dict["token"] = BValue::string("nx");
                    r.dict["nodes"] = BValue::string("");
                } else if (q == "find_node") {
                    r.dict["nodes"] = BValue::string("");
                }
                reply.dict["r"] = r;
                reply.dict["t"] = BValue::string(t);
                reply.dict["y"] = BValue::string("r");
                sendUdp(dhtFd, src, bencode(reply));
            }
        }
    }

    // ------------------------------------------------------------ engine loop

    void run() {
        {  // Saved before: start from the SD card right away, peers only fill in what is missing.
            std::lock_guard<std::mutex> lock(m);
            if (!saveDir.empty() && readFile(metaPath(), metaBuf)) {
                logMsg("torrent: metadata loaded from the SD card");
                finishMetadata();
            }
        }
        announceAll();
        dhtStart();
        std::vector<char> buf(64 * 1024);
        std::vector<pollfd> fds;
        std::vector<Peer*> refs;
        int utpIdx = -1, dhtIdx = -1;

        while (!stopping) {
            watchBeat(WatchEngine, "engine: announce/connect");
            uint64_t now = nowMs();
            {
                std::lock_guard<std::mutex> lock(m);
                int ready = countState(2);
                // Ask the trackers again often while the download is slow or short on peers.
                uint32_t every = (ready < 10 || (haveMeta && speed < 300 * 1024)) ? 20000u : 120000u;
                if (now - lastAnnounce > every) announceAll();
                connectMore(now);
                dhtStep(now);
                utpTimers(now);
                fds.clear();
                refs.clear();
                for (auto& p : peers) {
                    if (p.utp) continue;  // uTP peers share the UDP socket below
                    pollfd f{p.fd, POLLIN, 0};
                    if (p.state == 0 || !p.out.empty()) f.events |= POLLOUT;
                    fds.push_back(f);
                    refs.push_back(&p);
                }
                utpIdx = dhtIdx = -1;
                if (utpFd >= 0) {
                    utpIdx = (int)fds.size();
                    fds.push_back(pollfd{utpFd, POLLIN, 0});
                }
                if (dhtFd >= 0) {
                    dhtIdx = (int)fds.size();
                    fds.push_back(pollfd{dhtFd, POLLIN, 0});
                }
            }

            if (fds.empty())
                svcSleepThread(100000000ULL);
            else
                poll(fds.data(), fds.size(), 100);
            watchBeat(WatchEngine, "engine: peers");

            {
            std::lock_guard<std::mutex> lock(m);
            now = nowMs();
            if (utpIdx >= 0 && (fds[utpIdx].revents & POLLIN)) utpReceive(buf, now);
            if (dhtIdx >= 0 && (fds[dhtIdx].revents & POLLIN)) dhtReceive(buf);
            for (auto& p : peers) {  // uTP peers: the same timeouts as TCP ones
                if (!p.utp || p.dead) continue;
                if (p.state == 1 && now - p.since > kHandshakeTimeoutMs) p.dead = true;
                if (p.state == 2 && haveMeta && p.choked && now - p.chokedSince > 60000 && !candidates.empty() &&
                    peers.size() >= kMaxPeers - 5)
                    p.dead = true;
                if (p.state == 2 && p.out.empty() && now - p.lastSend > 90000) put32(p.out, 0);
                if (now - p.lastRecv > 120000) p.dead = true;
            }
            for (size_t i = 0; i < refs.size(); i++) {
                Peer& p = *refs[i];
                short re = fds[i].revents;
                if (p.state == 0) {
                    if (re & (POLLOUT | POLLERR | POLLHUP)) {
                        int err = 0;
                        socklen_t elen = sizeof(err);
                        getsockopt(p.fd, SOL_SOCKET, SO_ERROR, &err, &elen);
                        if (err || !(re & POLLOUT)) {
                            p.dead = true;
                            continue;
                        }
                        p.state = 1;
                        p.since = p.lastRecv = now;
                        sendHandshake(p);
                    } else if (now - p.since > kConnectTimeoutMs) {
                        p.dead = true;
                    }
                    continue;
                }
                if (re & POLLIN) {
                    for (int k = 0; k < 16 && (k == 0 || readable(p.fd)); k++) {
                        ssize_t n = recv(p.fd, buf.data(), buf.size(), MSG_DONTWAIT);
                        if (n > 0) {
                            p.in.append(buf.data(), (size_t)n);
                            p.lastRecv = now;
                            if ((size_t)n < buf.size()) break;
                        } else {
                            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) p.dead = true;
                            break;
                        }
                    }
                    if (!p.in.empty()) processInput(p);
                } else if (re & (POLLERR | POLLHUP | POLLNVAL)) {
                    p.dead = true;
                }
                if (p.state == 1 && now - p.since > kHandshakeTimeoutMs) p.dead = true;
                // Make room for new peers: drop ones that kept us choked for a minute.
                if (p.state == 2 && haveMeta && p.choked && now - p.chokedSince > 60000 && !candidates.empty() &&
                    peers.size() >= kMaxPeers - 5)
                    p.dead = true;
                if (p.state == 2 && !p.dead && p.out.empty() && now - p.lastSend > 90000) put32(p.out, 0);  // keep-alive
                if (now - p.lastRecv > 120000) p.dead = true;
            }

            if (!haveMeta && metaSize > 0 && now - lastMetaReq > 4000) {
                lastMetaReq = now;
                int asked = 0;
                for (auto& p : peers)
                    if (p.state == 2 && !p.dead && p.utMetadata && asked++ < 3) requestMetadata(p);
            }

            if (now - lastRechoke > 10000) {
                lastRechoke = now;
                rechoke(now);
            }
            if (now - lastDiag > 10000) {
                lastDiag = now;
                int ready = 0, unchoked = 0, inflight = 0;
                for (auto& p : peers) {
                    if (p.state != 2 || p.dead) continue;
                    ready++;
                    if (!p.choked) unchoked++;
                    inflight += (int)p.reqs.size();
                }
                int utpPeers = 0;
                for (auto& p : peers)
                    if (p.utp && p.state == 2 && !p.dead) utpPeers++;
                logMsg("torrent: %d peers (%d sending to us, %d over uTP), %d requests, %.2f MB/s, up %.1f MB, "
                       "known %zu, queue %zu, DHT %zu nodes / %d peers, active %lld MB, cache %lld MB, head %d",
                       ready, unchoked, utpPeers, inflight, speed / (1024 * 1024), uploaded / (1024.0 * 1024),
                       known.size(), candidates.size(), dhtNodes.size(), dhtPeersFound,
                       (long long)(activeBytes >> 20), (long long)(cacheBytes >> 20), head);
            }
            if (!watched) evict(0);  // just became a background download: give the RAM back now
            buildOrder();
            if (yielding && !wasYielding)  // give the connection back now: cancel what is in flight
                for (auto& p : peers) {
                    if (p.dead || p.state != 2) continue;
                    for (auto& r : p.reqs) {
                        int64_t begin = (int64_t)r.block * kBlock;
                        std::string pl;
                        put32(pl, (uint32_t)r.piece);
                        put32(pl, (uint32_t)begin);
                        put32(pl, (uint32_t)std::min<int64_t>(kBlock, pieceSize(r.piece) - begin));
                        sendMsg(p, 8, pl);
                    }
                    releaseRequests(p);
                }
            wasYielding = yielding;
            for (auto& p : peers) {
                if (p.dead || p.state != 2 || !haveMeta) continue;
                if (!p.interestedSent) {
                    sendMsg(p, 2, "");
                    p.interestedSent = true;
                }
                expireRequests(p, now);
                if (!p.choked && !yielding) fillRequests(p, now);
            }

            for (auto it = peers.begin(); it != peers.end();) {
                if (!it->dead && !it->out.empty()) flush(*it);
                if (it->dead) {
                    releaseRequests(*it);
                    if (it->fd >= 0) close(it->fd);
                    if (it->state == 0 && !it->utp && !utpTried.count(it->ep)) {
                        tryUtp.insert(it->ep);  // TCP did not connect: give uTP a chance
                        candidates.push_back(it->ep);
                    } else if (attempts[it->ep] < 2) {
                        candidates.push_back(it->ep);
                    }
                    it = peers.erase(it);
                } else {
                    ++it;
                }
            }

            if (now - speedAt >= 1000) {
                speed = speedAt ? (double)(downloaded - speedBase) * 1000.0 / (double)(now - speedAt) : 0;
                speedBase = downloaded;
                speedAt = now;
            }
            }
            watchBeat(WatchEngine, "engine: SD card allocate");
            allocateFile();
            watchBeat(WatchEngine, "engine: SD card write");
            flushWrites(false);
        }

        flushWrites(true);
        {
            std::lock_guard<std::mutex> fl(fileMutex);
            if (fp) fclose(fp);
            fp = nullptr;
        }
        std::lock_guard<std::mutex> lock(m);
        for (auto& p : peers)
            if (p.fd >= 0) close(p.fd);
        if (utpFd >= 0) close(utpFd);
        if (dhtFd >= 0) close(dhtFd);
        utpFd = dhtFd = -1;
        peers.clear();
        cache.clear();
        active.clear();
        writeQueue.clear();
        engineDone = true;
        cv.notify_all();
        logMsg("torrent: engine stopped");
        watchIdle(WatchEngine);
    }
};

// ---------------------------------------------------------------- public API

Torrent::Torrent(const std::string& infoHashHex, const std::vector<std::string>& trackers, int fileIdx)
    : d(std::make_shared<Impl>()) {
    d->hashHex = infoHashHex;
    d->wantFile = fileIdx;
    if (!hexToBytes(infoHashHex, d->infoHash, 20)) d->error = "unsupported info hash";

    std::set<std::string> seen;
    for (auto& t : trackers)
        if (seen.insert(t).second) d->trackers.push_back(t);
    for (const char* t : kDefaultTrackers)
        if (seen.insert(t).second) d->trackers.push_back(t);

    memcpy(d->peerId, "-NX0010-", 8);
    static const char* alnum = "0123456789abcdefghijklmnopqrstuvwxyz";
    uint8_t rnd[12];
    randomGet(rnd, sizeof(rnd));
    for (int i = 0; i < 12; i++) d->peerId[8 + i] = (uint8_t)alnum[rnd[i] % 36];
}

Torrent::~Torrent() {
    stop();
}

void Torrent::start() {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->started || !d->error.empty()) return;
    d->started = true;
    logMsg("torrent: starting %s with %zu trackers", d->hashHex.c_str(), d->trackers.size());
    auto self = d;
    startThread([self]() { self->run(); }, 512 * 1024);
}

bool Torrent::stopped() const {
    return d->stopping;
}

void Torrent::stop() {
    d->stopping = true;
    d->cv.notify_all();
}

void Torrent::saveTo(const std::string& dir, const std::string& infoJson) {
    std::lock_guard<std::mutex> lock(d->m);
    d->saveDir = dir;
    d->saveInfo = infoJson;
    d->sidecarPath = dir + "/" + d->hashHex + "_" + std::to_string(d->wantFile) + ".json";
}

bool Torrent::waitStopped(int timeoutMs) {
    std::unique_lock<std::mutex> lock(d->m);
    if (!d->started) return true;
    return d->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return d->engineDone; });
}

const std::string& Torrent::infoHash() const {
    return d->hashHex;
}

int Torrent::fileIndex() const {
    return d->wantFile;
}

bool Torrent::keepsDownloading() {
    std::lock_guard<std::mutex> lock(d->m);
    if (d->saveDir.empty() || !d->saveError.empty() || d->complete || !d->error.empty()) return false;
    return !d->haveMeta || d->saving || d->pendingAlloc;
}

TorrentStatus Torrent::status() {
    std::lock_guard<std::mutex> lock(d->m);
    TorrentStatus s;
    s.hasMetadata = d->haveMeta;
    s.peersConnected = d->countState(2);
    for (auto& p : d->peers)
        if (p.state == 2 && !p.dead && !p.choked) s.peersSending++;
    s.peersKnown = (int)d->known.size();
    s.downloaded = d->downloaded;
    s.speed = d->speed;
    s.fileName = d->fileName;
    s.fileSize = d->fileLen;
    s.error = d->error;
    s.saving = d->saving;
    s.savedBytes = d->savedBytes;
    s.complete = d->complete;
    s.saveError = d->saveError;
    s.savePath = d->dataPath;
    return s;
}

bool Torrent::hasMetadata() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->haveMeta;
}

int64_t Torrent::fileSize() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->fileLen;
}

std::string Torrent::fileName() {
    std::lock_guard<std::mutex> lock(d->m);
    return d->fileName;
}

bool Torrent::initialBufferReady(int* pct, bool* startReady) {
    std::lock_guard<std::mutex> lock(d->m);
    *pct = 0;
    if (!d->haveMeta) return false;
    int64_t startNeed = std::min(d->fileLen, kInitialBuffer);
    int64_t tailNeed = std::min(d->fileLen - startNeed, kTailBuffer);
    int64_t start = d->contiguous(d->fileOff, startNeed);
    int64_t tail = tailNeed > 0 ? d->contiguous(d->fileOff + d->fileLen - tailNeed, tailNeed) : 0;
    *pct = (int)((start + tail) * 100 / std::max<int64_t>(1, startNeed + tailNeed));
    if (startReady) *startReady = start >= startNeed;
    return start >= startNeed && tail >= tailNeed;
}

int64_t Torrent::read(int64_t off, char* buf, int64_t n, const std::atomic<bool>& cancel) {
    std::unique_lock<std::mutex> lock(d->m);
    if (!d->haveMeta) return -1;
    if (off >= d->fileLen) return 0;
    n = std::min(n, d->fileLen - off);
    int64_t g = d->fileOff + off;
    for (;;) {
        int piece = (int)(g / d->pieceLen);
        d->head = piece;
        const char* p = nullptr;
        int64_t a = d->availableAt(g, &p);
        if (a > 0) {
            int64_t k = std::min(n, a);
            memcpy(buf, p, (size_t)k);
            return k;
        }
        if (d->isSaved(piece)) {  // already on the SD card
            int64_t end = std::min((int64_t)(piece + 1) * d->pieceLen, d->fileOff + d->fileLen);
            int64_t k = std::min(n, end - g);
            lock.unlock();
            std::lock_guard<std::mutex> fl(d->fileMutex);
            if (!d->fp || fseek(d->fp, (long)off, SEEK_SET) != 0) return -1;
            size_t got = fread(buf, 1, (size_t)k, d->fp);
            return got > 0 ? (int64_t)got : -1;
        }
        if (d->stopping || cancel) return -1;
        d->cv.wait_for(lock, std::chrono::milliseconds(250));
    }
}

void Torrent::setWatched(bool on) {
    d->watched = on;
}

void Torrent::yieldNetwork(bool on) {
    d->yielding = on;
}

void Torrent::prefetch(double fraction) {
    std::lock_guard<std::mutex> lock(d->m);
    d->prefetchAt = fraction;
}

void Torrent::wakeUp() {
    std::lock_guard<std::mutex> lock(d->m);
    d->attempts.clear();
    d->candidates.clear();
    for (auto& e : d->known) d->candidates.push_back(e);
    d->lastAnnounce = 0;
    d->lastDhtLookup = 0;
    for (auto& p : d->peers) p.lastRecv = 0;  // stale sockets get dropped and replaced
}

void Torrent::setUploadMode(int mode) {
    d->uploadMode = std::max(0, std::min(2, mode));
}
