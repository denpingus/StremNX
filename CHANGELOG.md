# Changelog

## 0.3.2 (2026-11-16)

- Fix: debrid streams (Torrentio + Real-Debrid and other `https://` links) failed with "Playback error: loading
  failed". The player's FFmpeg has no HTTPS support, so remote streams are now read through libcurl, with seeking
  and automatic reconnection when the link drops

## 0.3.1 (2026-10-19)

- Fix: subtitles from addons often did not show up. OpenSubtitles v3 (the official Stremio subtitles addon) is now
  always available, even when it is not on your account or you are not logged in
- Fix: subtitle files and downloaded videos are passed to the player with a path it always understands
- The player says so when there are no subtitles in your language for a video
- Fix: the app could freeze while a stream was stalled (torrent without peers, slow link). The player no longer
  waits on mpv for its state.
- Fix: the torrent engine could stall waiting for network packets, freezing the screen with it
- Fix: pausing or deleting a download no longer holds the screen
- A watchdog writes to `log.txt` where the app got stuck if it ever freezes for more than 4 seconds

## 0.3.0 beta (2026-10-18)

Tested on hardware; some bugs may remain.

- Watch progress and **My List** synced with the Stremio account
- **Continue Watching** reuses the stream picked last time; X picks another one; falls back to the list if it fails
- **Up next**: automatic next episode with a countdown, choosing the best stream
- Preferred quality (learned or fixed), preferred source, stream filters and sorting
- **Discover** tab (type, catalog, genre)
- Torrent engine: **DHT**, **uTP** fallback, uploading with **Off / Limited / Normal** setting (default: Limited)
- Player: speed, aspect ratio, zoom, volume boost; pauses and reconnects after sleep
- **Touch screen** support
- Subtitles from addons with size, position, style, color and delay settings
- Downloads to the SD card with a Downloads tab (watch offline, pause/resume, delete)

## 0.2.0

- Streaming-service style interface: hero banner, rows, details page, custom player overlay
- Watch history, Continue Watching, recommendations
- Built-in BitTorrent engine (no PC needed)
- New name and icon: StremNX

## 0.1.0

- First version: addon catalogs, search, details, streams, mpv player, Stremio login
