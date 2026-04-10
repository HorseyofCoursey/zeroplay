# ZeroPlay

A lightweight H.264 video player for the Raspberry Pi, built as a modern replacement for the discontinued omxplayer. Uses the V4L2 M2M hardware decoder, DRM/KMS display, and ALSA audio — zero CPU video decode, zero X11 dependency.

have a nice day ;)

---

## Supported Hardware

| Device | OS |
|---|---|
| Pi Zero W (original) | Raspberry Pi OS Lite 32-bit (Trixie) |
| Pi Zero 2 W | Raspberry Pi OS Lite 64-bit (Trixie) |
| Pi Zero 2 W | Raspberry Pi OS Lite 32-bit (Trixie) |
| Pi Zero 2 W | balenaOS (Bookworm) |
| Pi 3 / 3+ | Raspberry Pi OS Lite 64-bit (Trixie) |
| Pi 3 / 3+ | Raspberry Pi OS Lite 32-bit (Trixie) |
| Pi 4 | Raspberry Pi OS Lite 64-bit (Trixie) |
| Pi 4 | Raspberry Pi OS Lite 32-bit (Trixie) |

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

### Streaming

ZeroPlay supports HLS streams (`.m3u8`) directly — pass a URL as the path argument. Use `--hls-bitrate` to cap the variant bitrate on bandwidth-limited connections.

---

## Installation

```bash
curl -fsSL https://raw.githubusercontent.com/HorseyofCoursey/zeroplay/main/install.sh | sudo bash
```

This will install dependencies, build from source, and place the binary at `/usr/local/bin/zeroplay`.

### Manual build

```bash
sudo apt install git gcc make pkgconf \
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev \
  libdrm-dev libasound2-dev

git clone https://github.com/HorseyofCoursey/zeroplay.git
cd zeroplay
make
sudo make install
```

### Cross-compile from x86 Linux

If you'd rather build on your desktop/laptop instead of the Pi:

```bash
./cross-build.sh
```

This uses Docker with QEMU emulation to build a native aarch64 binary. Requires Docker with buildx. The resulting `zeroplay` binary can be copied to the Pi with `scp`.

### WebSocket remote control (optional)

To build with WebSocket remote control support:

```bash
sudo apt install libwebsockets-dev libcjson-dev
make WS=1
sudo make install
```

The base build has no dependency on libwebsockets. See [WebSocket mode](#websocket-remote-control) below.

---

## Usage

```
zeroplay [options] <path> [path2 ...]
```

Each path can be a video file, an image, a `.txt`/`.m3u` playlist, a directory, or an HLS URL. Up to 4 paths may be given — each is assigned to a connected display in DRM enumeration order. On Pi 4 with two HDMI outputs connected, `zeroplay file1.mp4 file2.mp4` plays each file on a separate display simultaneously.

### Options

| Flag | Description |
|---|---|
| `--loop` | Loop playback indefinitely |
| `--shuffle` | Randomise playlist order |
| `--no-audio` | Disable audio |
| `--vol n` | Initial volume, 0–200 (default: 100) |
| `--pos n` | Start position in seconds |
| `--audio-device dev` | ALSA device override |
| `--hls-bitrate bps` | Cap HLS variant bitrate in bps (or `HLS_MAX_BANDWIDTH` env) |
| `--image-duration n` | Seconds to display each image (default: 10, 0 = hold forever) |
| `--verbose` | Print decoder and driver info on startup |
| `--help` | Show usage |

### Examples

```bash
# Play a file
zeroplay movie.mp4

# Play an HLS stream
zeroplay https://example.com/stream.m3u8

# Play an HLS stream, cap at 2 Mbps (useful on Pi Zero)
zeroplay --hls-bitrate 2000000 https://example.com/stream.m3u8

# Play video combined from separate video (no audio) and audio HLS streams (like YouTube does)
zeroplay (https://example.com/stream_video.m3u8)(https://example.com/stream_audio.m3u8)

# Play all media in a directory
zeroplay /home/pi/media/

# Play a playlist file, loop and shuffle
zeroplay --loop --shuffle playlist.txt

# Mix images and videos in a directory, 15 seconds per image
zeroplay --loop --image-duration 15 /home/pi/media/

# Display a static image indefinitely
zeroplay --image-duration 0 photo.jpg

# Dual display on Pi 4
zeroplay file1.mp4 file2.mp4

# Dual display with playlists, each display independent
zeroplay /media/screen1/ /media/screen2/

# Start at 1h 30min
zeroplay --pos 5400 movie.mp4

# Start at 80% volume
zeroplay --vol 80 movie.mp4

# No audio
zeroplay --no-audio movie.mp4

# Override ALSA output device
zeroplay --audio-device plughw:CARD=Headphones,DEV=0 movie.mp4

# Show decoder and driver details on startup
zeroplay --verbose movie.mp4
```

---

## Playlist files

A playlist is a plain `.txt` or `.m3u` file with one path per line. Lines starting with `#` are ignored.

```
# My playlist
/home/pi/media/intro.mp4
/home/pi/media/photo.jpg
/home/pi/media/main.mp4
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
| `n` | Skip to next playlist item |
| `b` | Go to previous playlist item |
| `i` | Previous chapter |
| `o` | Next chapter |
| `q` / Esc | Quit |

---

## Audio

ZeroPlay auto-detects the HDMI audio device and routes through the `hdmi:` ALSA device, which uses the IEC958 plugin chain built into vc4-hdmi. This ensures correct HDMI audio on all supported Pi models — using `plughw:` directly bypasses this chain and produces noise.

The hardware's native sample rate is probed at startup so that libswresample performs any necessary conversion correctly, avoiding pitch distortion.

To override the audio device:

```bash
zeroplay --audio-device plughw:CARD=Headphones,DEV=0 movie.mp4
```

To list available devices:

```bash
aplay -L
```

---

## WebSocket Remote Control

> Requires building with `make WS=1`. The base binary has no dependency on libwebsockets.

When built with WebSocket support, ZeroPlay can run as a remotely controlled player, receiving commands from a backend and reporting state every 5 seconds.

```bash
zeroplay --ws-url ws://backend.local:8080/ws --device-token <token>
```

In WebSocket mode, no file path is required on the command line — media is loaded via `load` commands from the backend.

### WebSocket options

| Flag | Env var | Description |
|---|---|---|
| `--ws-url URL` | `BACKEND_WS_URL` | Backend WebSocket URL (`ws://` or `wss://`) |
| `--device-token TOKEN` | `DEVICE_TOKEN` | Device authentication token (required) |
| `--health-port PORT` | `HEALTH_PORT` | HTTP health endpoint port (default: 3000) |

### Commands (backend → device)

| Command | Fields | Description |
|---|---|---|
| `load` | `url` | Load and play a media URL |
| `play` | — | Resume playback |
| `pause` | — | Pause playback |
| `stop` | — | Stop and unload |
| `seek` | `positionMs` | Seek to position in milliseconds |

### State reports (device → backend)

Every 5 seconds the device sends a `state` message with current position, pause state, loaded URL, and idle flag. The `/health` endpoint returns a JSON response indicating WebSocket connection and player readiness.

### Reconnection

ZeroPlay reconnects automatically on disconnect with exponential backoff (1s → 30s).

---

## Running as a service

### Standalone mode

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

### WebSocket mode

```ini
[Unit]
Description=ZeroPlay video player (WebSocket mode)
After=network-online.target
Wants=network-online.target

[Service]
User=pi
Group=video
Environment=HOME=/home/pi
Environment=BACKEND_WS_URL=ws://backend.local:8080/ws
Environment=DEVICE_TOKEN=your-token-here
ExecStart=/usr/local/bin/zeroplay --ws-url ${BACKEND_WS_URL} --device-token ${DEVICE_TOKEN}
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now zeroplay
```

Make sure your user is in the video group:

```bash
sudo usermod -aG video pi
```

---

## How It Works

- **Demux** — libavformat reads the container and routes packets (supports local files, HLS, and other network streams)
- **Video decode** — V4L2 M2M hardware decoder via bcm2835-codec
- **Display** — DRM/KMS atomic modesetting with DMABUF zero-copy from decoder to scanout
- **Audio** — libavcodec software decode → libswresample (with hardware rate probing and HE-AAC SBR correction) → ALSA
- **Sync** — Wall-clock pacing against video PTS, audio runs independently

No X11, no Wayland, no GPU compositing. Runs directly on the framebuffer from a TTY or SSH session.

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
| WebSocket remote control | No | Yes (opt-in) |
| Subtitles | Yes | Yes |
| Chapter skip | No | Yes |
| Seeking | Yes | Yes |
| Volume control | Yes | Yes |
| Loop | Yes | Yes |
| Runs on modern OS | No | Yes |
