# ZeroPlay

A lightweight H.264 video player for Linux SBCs, built as a modern replacement for the discontinued omxplayer. Uses the V4L2 M2M hardware decoder, DRM/KMS display, and ALSA audio — zero CPU video decode, zero X11 dependency.

have a nice day ;)

---

## Supported Hardware

ZeroPlay runs on any Linux device with a V4L2 M2M hardware decoder and DRM/KMS display. Tested on:

| Device | OS |
|---|---|
| Pi Zero W | Raspberry Pi OS Lite 32-bit (Trixie) |
| Pi Zero 2 W | Raspberry Pi OS Lite 32/64-bit (Trixie) |
| Pi Zero 2 W | balenaOS (Bookworm) |
| Pi 3 / 3+ | Raspberry Pi OS Lite 32/64-bit (Trixie) |
| Pi 4 | Raspberry Pi OS Lite 32/64-bit (Trixie) |

Both 32-bit and 64-bit builds are supported. The install script builds from source automatically for the correct architecture.

---

## Supported Formats

| Codec | Container |
|---|---|
| H.264 (up to High@L4.1) | MP4, MKV, MOV, HLS (.m3u8) |
| H.263 | MP4, MKV |
| MPEG-4 | MP4, MKV |

H.264 is hardware decoded via the bcm2835 VPU on Pi Zero W, Pi Zero 2W, and Pi 3, and the V4L2 stateful decoder on Pi 4.

### Audio

| Codec | Notes |
|---|---|
| AAC / HE-AAC | SBR rate mismatch auto-detected and corrected |
| MP3 | |
| AC3 | |
| FLAC | |
| Opus | |

---

## Installation

```bash
curl -fsSL https://raw.githubusercontent.com/HorseyofCoursey/zeroplay/main/install.sh | sudo bash
```

This installs dependencies, builds from source, and places the binary at `/usr/local/bin/zeroplay`.

### Manual build

```bash
sudo apt install git gcc make pkgconf \
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev \
  libdrm-dev libasound2-dev libcjson-dev libfreetype-dev

git clone https://github.com/HorseyofCoursey/zeroplay.git
cd zeroplay
make
sudo make install
```

`libfreetype-dev` is optional but recommended — without it subtitles use a built-in bitmap font fallback.

### YouTube support (optional)

To play YouTube URLs directly, install yt-dlp:

```bash
sudo curl -L https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp \
  -o /usr/local/bin/yt-dlp && sudo chmod +x /usr/local/bin/yt-dlp
```

Keep yt-dlp up to date — YouTube changes frequently and old versions stop working.

### WebSocket remote control (optional)

```bash
sudo apt install libwebsockets-dev
make WS=1
sudo make install
```

### Cross-compile from x86 Linux

```bash
./cross-build.sh
```

Uses Docker with buildx to produce a native ARM binary. Copy the result to your Pi with `scp`.

---

## Usage

```
zeroplay [options] <path> [path2 ...]
```

Each path can be a video file, image, `.txt`/`.m3u` playlist, directory, URL, or YouTube URL. Up to 4 paths may be given — each is assigned to a connected display in DRM enumeration order.

### Options

| Flag | Description |
|---|---|
| `--loop` | Loop playback indefinitely |
| `--shuffle` | Randomise playlist order |
| `--no-audio` | Disable audio |
| `--vol n` | Initial volume, 0–200 (default: 100) |
| `--pos n` | Start position in seconds |
| `--audio-device dev` | ALSA device override |
| `--sub path` | External subtitle file (.srt) |
| `--hls-bitrate bps` | Cap HLS variant bitrate in bps (or `HLS_MAX_BANDWIDTH` env) |
| `--yt-quality n` | YouTube stream height: 360, 480, 720, 1080 (default: 480) |
| `--image-duration n` | Seconds per image (default: 10, 0 = hold forever) |
| `--verbose` | Print decoder and driver info |
| `--help` | Show usage |

### Examples

```bash
# Play a local file
zeroplay movie.mp4

# Play a YouTube video (requires yt-dlp)
zeroplay "https://www.youtube.com/watch?v=..."

# Play YouTube at 720p (Pi 4/5)
zeroplay --yt-quality 720 "https://www.youtube.com/watch?v=..."

# Play YouTube at 360p (Pi Zero W)
zeroplay --yt-quality 360 "https://www.youtube.com/watch?v=..."

# Play an HLS stream
zeroplay https://example.com/stream.m3u8

# Play separate video and audio streams (advanced)
zeroplay "(https://example.com/video.m3u8)(https://example.com/audio.m3u8)"

# Play with subtitles (auto-detected if .srt has the same name as the video)
zeroplay movie.mp4

# Play with an explicit subtitle file
zeroplay --sub subtitles.srt movie.mp4

# Play all media in a directory
zeroplay /home/pi/media/

# Play a playlist file, loop and shuffle
zeroplay --loop --shuffle playlist.txt

# Mix images and videos in a directory, 15 seconds per image
zeroplay --loop --image-duration 15 /home/pi/media/

# Display a static image indefinitely
zeroplay --image-duration 0 photo.jpg

# Loop a directory of media
zeroplay --loop /home/pi/media/

# Shuffle a playlist
zeroplay --loop --shuffle playlist.txt

# Dual display on Pi 4
zeroplay file1.mp4 file2.mp4

# Start at 1h 30min
zeroplay --pos 5400 movie.mp4
```

---

## Playlist files

A plain `.txt` or `.m3u` file with one path or URL per line. Lines starting with `#` are ignored.

```
# My playlist
/home/pi/media/intro.mp4
/home/pi/media/photo.jpg
https://example.com/stream.m3u8
```

---

## Controls

| Key | Action |
|---|---|
| `p` / Space | Pause / resume |
| ← / → | Seek −/+ 1 minute |
| ↑ / ↓ | Seek −/+ 5 minutes |
| `+` / `=` | Volume up 10% |
| `-` | Volume down 10% |
| `m` | Mute / unmute |
| `n` | Next playlist item |
| `b` | Previous playlist item |
| `i` / `o` | Previous / next chapter |
| `q` / Esc | Quit |

---

## Subtitles

ZeroPlay renders subtitles via a DRM overlay plane — composited by the display hardware with no CPU overhead.

- **Auto-detection** — a `.srt` file with the same name as the video is loaded automatically
- **Explicit file** — use `--sub subtitles.srt`
- **Embedded** — MKV files with embedded SRT/subrip subtitle tracks work automatically
- **Font** — DejaVu Sans Bold 36px when `libfreetype-dev` is installed; built-in bitmap font otherwise

---

## YouTube

ZeroPlay detects YouTube URLs automatically and resolves them via yt-dlp:

```bash
zeroplay "https://www.youtube.com/watch?v=..."
zeroplay "https://youtu.be/..."
zeroplay "https://www.youtube.com/shorts/..."
```

Use `--yt-quality` to set the maximum stream height. The default is 480p which works well on Pi Zero 2W. Pi 4/5 users can use 720p or 1080p.

| Device | Recommended quality |
|---|---|
| Pi Zero W | 360p |
| Pi Zero 2 W | 480p |
| Pi 3 | 480p |
| Pi 4 / 5 | 720p or 1080p |

> **Note:** Rapid seeking and pausing during YouTube playback may cause instability due to the dual-stream sync mechanism. Normal playback and occasional seeking works reliably.

---

## Audio

ZeroPlay auto-detects the HDMI audio device and routes through the `hdmi:` ALSA device, which uses the IEC958 plugin chain built into vc4-hdmi. Using `plughw:` directly bypasses this chain and produces noise.

The hardware's native sample rate is probed at startup to avoid pitch distortion from resampling.

```bash
# Override the audio device
zeroplay --audio-device plughw:CARD=Headphones,DEV=0 movie.mp4

# List available devices
aplay -L
```

---

## WebSocket Remote Control

> Requires `make WS=1`. No libwebsockets dependency in the base build.

Runs ZeroPlay as a remotely controlled player, receiving commands from a backend and reporting state every 5 seconds.

```bash
zeroplay --ws-url ws://backend.local:8080/ws --device-token <token>
```

### Options

| Flag | Env var | Description |
|---|---|---|
| `--ws-url URL` | `BACKEND_WS_URL` | Backend WebSocket URL (`ws://` or `wss://`) |
| `--device-token TOKEN` | `DEVICE_TOKEN` | Device auth token (required) |
| `--health-port PORT` | `HEALTH_PORT` | HTTP health endpoint port (default: 3000) |

### Commands

| Command | Fields | Description |
|---|---|---|
| `load` | `url` | Load and play a media URL |
| `play` | — | Resume |
| `pause` | — | Pause |
| `stop` | — | Stop and unload |
| `seek` | `positionMs` | Seek to position in milliseconds |

Reconnects automatically with exponential backoff (1s → 30s).

---

## Running as a service

```bash
sudo nano /etc/systemd/system/zeroplay.service
```

```ini
[Unit]
Description=ZeroPlay video player
After=multi-user.target

[Service]
User=pi
Group=video
Environment=HOME=/home/pi
ExecStart=/usr/local/bin/zeroplay --loop /home/pi/media/
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now zeroplay
sudo usermod -aG video pi
```

---

## How It Works

- **Demux** — libavformat reads the container and routes packets (local files, HLS, network streams, YouTube via yt-dlp)
- **Video decode** — V4L2 M2M hardware decoder
- **Display** — DRM/KMS atomic modesetting with DMABUF zero-copy from decoder to scanout
- **Subtitles** — rendered into an ARGB8888 DRM overlay plane, composited by the display hardware
- **Audio** — libavcodec → libswresample (hardware rate probing, HE-AAC SBR correction) → ALSA
- **Sync** — wall-clock pacing against video PTS; A/V sync throttling for separate stream playback

No X11, no Wayland, no GPU compositing. Runs from a TTY or SSH session.

---

## Differences from omxplayer

| Feature | omxplayer | ZeroPlay |
|---|---|---|
| Hardware decode | OpenMAX (deprecated) | V4L2 M2M |
| Display | dispmanx (deprecated) | DRM/KMS |
| Dual display | No | Yes (Pi 4) |
| Playlist / directory | No | Yes |
| Image display | No | Yes |
| HLS streaming | No | Yes |
| YouTube | No | Yes (via yt-dlp) |
| Subtitles (SRT) | Yes | Yes |
| Subtitles (embedded MKV) | No | Yes |
| WebSocket remote control | No | Yes (opt-in) |
| Chapter navigation | No | Yes |
| Seeking | Yes | Yes |
| Volume control | Yes | Yes |
| Loop | Yes | Yes |
| Runs on modern OS | No | Yes |
