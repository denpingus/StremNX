# StremNX

**Cliente Stremio não oficial para Nintendo Switch (homebrew).**

> [!IMPORTANT]
> O StremNX é um projeto independente da comunidade. Ele **não tem ligação, aprovação nem vínculo com o Stremio**
> nem com a Nintendo. "Stremio" é marca registrada do seu dono e aparece aqui só para indicar compatibilidade.
> O StremNX não hospeda, fornece nem indica nenhum conteúdo: tudo o que aparece vem dos addons que **você** instala.

🇺🇸 [Read in English](README.md)

## Sobre torrents e envio de dados

O BitTorrent funciona por troca: enquanto você baixa um torrent, outras pessoas baixam pedaços dele de você.
O StremNX envia dados de forma **limitada** por padrão. Em **Settings → Torrent uploading** dá para **desligar**
(os torrents podem ficar lentos ou travar) ou deixar **normal**.

Em muitos países, baixar ou compartilhar material protegido por direitos autorais sem permissão é ilegal e pode gerar
notificações ou multas do provedor de internet. **Você é responsável pelos addons que instala e pelo que assiste.**
O StremNX não vem com nenhum addon de torrent ou pirataria.

## Privacidade

- A senha da conta Stremio só é enviada para `api.strem.io`, por HTTPS, no login, e nunca é salva.
- A chave de sessão (`authKey`) fica em texto puro em `sdmc:/switch/stremnx/config.json`. **Não compartilhe esse arquivo.**
- Logado, o progresso e a biblioteca são sincronizados com a sua conta Stremio, como nos apps oficiais.

## Licença

Software livre sob a **GNU General Public License v3.0** ([LICENSE](LICENSE)). Bibliotecas de terceiros em
[THIRD_PARTY.md](THIRD_PARTY.md).

## Como é feito

Escrito em C++ com libnx, SDL2, libcurl e libmpv.

Ele não é uma recompilação do app oficial: reimplementa o protocolo de addons do Stremio e usa o mpv como player.

## O que já faz (v0.3)

- **Início no estilo Netflix/Prime:** destaque rotativo no topo (fundo, logo, sinopse, botões Play e More Info) e fileiras horizontais
- **Continue Watching:** retoma de onde você parou, com barra de progresso; sincroniza com o progresso da sua conta Stremio
- Abas **Movies** e **Series** com fileiras por catálogo e por gênero, **Search** com resultados em fileiras e **Account**
- Detalhes com logo do título, nota do IMDb, elenco, botões Resume/Start Over, temporadas e episódios com miniatura e progresso
- Lista de streams com selos de qualidade (4K, 1080p, HDR/Dolby Vision), tamanho, seeders e tipo (Torrent, Debrid, Direct)
- Motor de torrent próprio: torrents tocam direto no Switch
- Player com interface própria: título, barra de progresso com buffer, tempo restante, aviso de +10s, tela de carregamento e menu de áudio e legendas
- Legendas dos addons (OpenSubtitles etc.) no idioma escolhido, com tamanho, posição, estilo, cor e atraso ajustáveis em Settings
- **Progresso sincronizado com a conta Stremio** (o que você assiste no Switch aparece no celular/PC) e **My List** (biblioteca da conta)
- **Continue Watching volta direto ao mesmo stream** escolhido antes (X escolhe outro); se o link expirou, abre a lista de streams
- **Próximo episódio automático** com contagem regressiva ("Up next"), escolhendo sozinho o melhor stream
- **Qualidade preferida** (aprende com suas escolhas ou fixa em Settings) e **filtros/ordenação** na lista de streams (Y qualidade, X origem, ZL/ZR ordem)
- Aba **Discover** com filtros de tipo, catálogo e gênero
- Torrents com **DHT** (acha pares sem depender de trackers) e **uTP** (reserva quando o TCP falha)
- Player: **velocidade**, **proporção/zoom** e **volume até 200%** no menu X; **retoma sozinho depois de suspender** o console
- **Tela de toque**: tocar seleciona/abre, arrastar navega; no player, tocar nas laterais pula 10 s, na barra vai para o ponto
- Login na conta Stremio, addons instalados na conta e addons extras por URL

## Compilar

### 1. Instalar o devkitPro

Windows: baixe o instalador em https://github.com/devkitPro/installer/releases e marque **Switch Development**.
Depois abra o terminal **MSYS2 / devkitPro** (não o PowerShell).

Linux/WSL: siga https://devkitpro.org/wiki/devkitPro_pacman

### 2. Instalar as bibliotecas

```bash
dkp-pacman -Syu
dkp-pacman -S switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-curl switch-libmpv
```

(No Linux/WSL, use `sudo dkp-pacman ...`.)

### 3. Compilar

```bash
cd stremio-nx
make
```

O resultado é o arquivo `stremnx.nro`.

## Instalar no Switch

Copie `stremnx.nro` para `sdmc:/switch/stremnx/` e abra pelo Homebrew Menu.

**Importante:** abra o Homebrew Menu segurando **R** ao abrir um jogo (modo título, não pelo Álbum). No modo applet, o app tem pouca memória e o vídeo não roda.

Para testar mais rápido, sem tirar o cartão SD: no Homebrew Menu aperte **Y** (nxlink) e, no PC, rode:

```bash
nxlink -s stremnx.nro
```

O `-s` mostra os logs no terminal do PC. Os logs também ficam em `sdmc:/switch/stremnx/log.txt`.

## Controles

| Tela | Botão | Ação |
|---|---|---|
| Geral | L / R | Trocar de aba |
| Geral | + | Sair |
| Início/Movies/Series | ↑ ↓ ← → | Navegar entre fileiras e títulos |
| Início/Movies/Series | A | Abrir (no Continue Watching: continuar no mesmo stream) |
| Continue Watching | X | Escolher outro stream |
| Início/Movies/Series | Y | Detalhes do título |
| Busca | Y | Nova busca |
| Detalhes | ↑ ↓ | Botões → temporadas → episódios |
| Detalhes | L / R | Trocar de temporada |
| Streams | A | Reproduzir |
| Streams | L / R | Pular página |
| Streams | Y / X / ZL·ZR | Filtrar qualidade / origem, mudar ordenação |
| Discover | ← → e A | Escolher filtro e trocar a opção |
| Player (fim do episódio) | A / B | Up next: assistir agora / cancelar |
| Player | A | Pausar/continuar |
| Player | ← → | Voltar/avançar 10 s |
| Player | ZL / ZR | Voltar/avançar 60 s |
| Player | ↑ ↓ | Mostrar os controles |
| Player | X | Menu de áudio, legendas e reprodução (velocidade, proporção, zoom, volume) |
| Player | Y | Ligar/desligar legendas |
| Player | L / R | Atrasar/adiantar a legenda (passo ajustável em Settings) |
| Menu de áudio e legendas | ZL / ZR | Tamanho da legenda |
| Player | − | Estatísticas |
| Player | B / + | Sair do vídeo |

## Torrents

O app tem um motor de torrent próprio (`source/torrent.cpp`), então torrents tocam direto no Switch, sem PC:

- Pares vêm dos trackers do stream (campo `sources`), de uma lista de trackers públicos, de PEX e do DHT.
- Os metadados do torrent são baixados dos próprios pares (BEP 9), então só o info hash basta.
- Os pedaços são baixados em ordem a partir do ponto que o vídeo está lendo, mais o fim do arquivo (índice do MKV/MP4).
- Tudo fica num cache de 256 MB na RAM; nada é gravado no cartão SD. Voltar muito no vídeo baixa de novo.
- Pares também vêm do DHT; o uTP é usado quando a conexão TCP com um par falha. Ainda não há criptografia (MSE).

Opcional: para usar o servidor do Stremio de um PC em vez do motor interno, vá em
**Account → Streaming server** e digite `http://IP-DO-PC:11470`.

Links HTTP e de debrid (Real-Debrid, AllDebrid, etc.) tocam direto, como antes.

## Downloads no cartão SD

Em **Settings → Save torrents to SD card**, ligue a opção para salvar no cartão os torrents que você assistir:

- O arquivo vai para `sdmc:/switch/stremnx/downloads/`, junto com um `.json` que guarda o progresso.
- Com a opção ligada, o motor baixa o arquivo inteiro. Se você sair do vídeo antes de terminar, o download continua em segundo plano enquanto o app estiver aberto.
- Na aba **Downloads**:
  - **A** assiste offline (download completo) ou pausa/retoma (download incompleto);
  - **Y** apaga o arquivo do cartão, com confirmação.
- Quando o episódio ou filme já está baixado, ele aparece no topo da lista de streams como **On SD card**.
- Arquivos acima de 4 GB precisam de cartão em exFAT (FAT32 não aceita).

## Configuração

Arquivo `sdmc:/switch/stremnx/config.json`:

```json
{
  "authKey": "...",
  "email": "voce@exemplo.com",
  "server": "http://192.168.0.10:11470",
  "addons": ["https://exemplo.com/manifest.json"]
}
```

Sem login, o app usa só o Cinemeta (catálogo) e os addons listados em `addons`.

## Estrutura

| Arquivo | O que faz |
|---|---|
| `source/main.cpp` | Inicializa serviços do libnx |
| `source/app.*` | Loop principal, pilha de telas, config, carregamento de addons |
| `source/home.cpp` | Início, Discover, Movies, Series, Search, Downloads, Settings |
| `source/detail.cpp` | Tela de detalhes |
| `source/screens.cpp` | Streams (pontuação, filtros), torrent, "Up next" |
| `source/torrent.cpp` | Motor de torrent: trackers, DHT, PEX, uTP, download para o SD |
| `source/history.*`, `library.*` | Histórico, stream salvo e My List |
| `source/player.cpp` | mpv via render API OpenGL |
| `source/stremio.*` | Protocolo de addons e API da conta (api.strem.io) |
| `source/ui.*` | Desenho com SDL2, cache de texto, carregamento de imagens em threads |
| `source/http.*`, `json.*` | HTTP (curl) e parser JSON próprio |

## Pontos ainda não testados / próximos passos

- **Troca de contexto GL:** a UI usa o SDL_Renderer, e o player destrói o renderer e cria um contexto OpenGL para o mpv. Se der tela preta ou crash ao abrir um vídeo, é a primeira coisa a investigar (veja o `log.txt`).
- **Decodificação por hardware:** depende de como o `switch-libmpv` foi compilado. Sem hwdec, 1080p H.264 pode engasgar e HEVC/4K não roda. A solução é usar o FFmpeg com backend nvtegra (fork do averne, usado pelo Switchfin).
- Falta "Continuar assistindo" / sincronização de progresso com a conta (`datastoreGet`/`datastorePut` da API).
- Ícone: coloque um `icon.jpg` 256x256 na raiz do projeto.
