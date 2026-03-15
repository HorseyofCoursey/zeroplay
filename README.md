# ZeroPlay
A lightweight H.264 video player for the Raspberry Pi, built as a modern replacement for the discontinued __omxplayer__. Uses the V4L2 M2M hardware decoder, DRM/KMS display, and ALSA audio — zero CPU video decode, zero X11 dependency.

```
have a nice day ;)
```

## Supported Hardware
| Device | OS |
|---|---|
| Pi Zero W (original) | Raspberry Pi OS Lite 32-bit (Trixie) |
| Pi Zero 2 W | Raspberry Pi OS Lite 64-bit (Trixie) |
| Pi Zero 2 W | Raspberry Pi OS Lite 32-bit (Trixie) |
| Pi 3 / 3+ | Raspberry Pi OS Lite 64-bit (Trixie) |
| Pi 3 / 3+ | Raspberry Pi OS Lite 32-bit (Trixie) |
| Pi 4 | Raspberry Pi OS Lite 64-bit (Trixie) |
| Pi 4 | Raspberry Pi OS Lite 32-bit (Trixie) |

Both 32-bit and 64-bit builds are supported. The install script builds from source automatically for the correct architecture.

## Supported Formats
| Codec | Container |
|---|---|
| H.264 (up to High@L4.1) | MP4, MKV, MOV |

H.264 is hardware decoded via the bcm2835 VPU on Pi Zero W, Pi Zero 2W, and Pi 3, and the V4L2 stateful decoder on Pi 4.

## Installation

```
curl -fsSL https://raw.githubusercontent.com/HorseyofCoursey/zeroplay/main/install.sh | sudo bash
```

This will install dependencies, build from source, and place the binary at `/usr/local/bin/zeroplay`.

### Manual build

If you'd prefer to build yourself:

```
sudo apt install git gcc make pkgconf \
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev \
  libdrm-dev libasound2-dev

git clone https://github.com/HorseyofCoursey/zeroplay.git
cd zeroplay
make
sudo make install
```

## Usage

```
zeroplay [options] <path> [path2 ...]
```

Each path can be a video file, an image, a `.txt`/`.m3u` playlist, or a directory. Up to 4 paths may be given — each is assigned to a connected display in DRM enumeration order. On Pi 4 with two HDMI outputs connected, `zeroplay file1.mp4 file2.mp4` plays each file on a separate display simultaneously.

### Options
| Flag | Description |
|---|---|
| `--loop` | Loop playback indefinitely |
| `--shuffle` | Randomise playlist order |
| `--no-audio` | Disable audio |
| `--vol n` | Initial volume, 0–200 (default: 100) |
| `--pos n` | Start position in seconds |
| `--audio-device dev` | ALSA device override |
| `--image-duration n` | Seconds to display each image (default: 10, 0 = hold forever) |
| `--verbose` | Print decoder and driver info on startup |
| `--sync-master` | Broadcast PTS for multi-Pi sync |
| `--sync-slave ip` | Receive PTS from master at `<ip>` |
| `--help` | Show usage |

### Examples

```
# Play a file
zeroplay movie.mp4

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

# Loop
zeroplay --loop movie.mp4

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

### Playlist files

A playlist is a plain `.txt` or `.m3u` file with one path per line. Lines starting with `#` are ignored.

```
# My playlist
/home/pi/media/intro.mp4
/home/pi/media/photo.jpg
/home/pi/media/main.mp4
```

### Controls
| Key | Action |
|---|---|
| `p` / `Space` | Pause / resume |
| `←` / `→` | Seek −/+ 1 minute |
| `↑` / `↓` | Seek −/+ 5 minutes |
| `+` / `=` | Volume up 10% |
| `-` | Volume down 10% |
| `m` | Mute / unmute |
| `n` | Skip to next playlist item |
| `b` | Go to previous playlist item |
| `i` | Previous chapter |
| `o` | Next chapter |
| `q` / `Esc` | Quit |

## Audio Device
ZeroPlay auto-detects the HDMI audio device. To override:

```
zeroplay --audio-device plughw:CARD=vc4hdmi0,DEV=0 movie.mp4
```

To list available devices:

```
aplay -L
```

## Running as a service

To start ZeroPlay automatically on boot, create a systemd service:

```
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

```
sudo systemctl daemon-reload
sudo systemctl enable --now zeroplay
```

Make sure your user is in the `video` group:

```
sudo usermod -aG video pi
```

## How It Works
* **Demux** — libavformat reads the container and routes packets
* **Video decode** — V4L2 M2M hardware decoder via bcm2835-codec
* **Display** — DRM/KMS atomic modesetting with DMABUF zero-copy from decoder to scanout
* **Audio** — libavcodec software decode → libswresample → ALSA
* **Sync** — Wall-clock pacing against video PTS, audio runs independently

No X11, no Wayland, no GPU compositing. Runs directly on the framebuffer from a TTY or SSH session.

## Differences from omxplayer
| Feature | omxplayer | ZeroPlay |
|---|---|---|
| Hardware decode | OpenMAX (deprecated) | V4L2 M2M |
| Display | dispmanx (deprecated) | DRM/KMS |
| Dual display | No | Yes (Pi 4) |
| Playlist / directory | No | Yes |
| Image display | No | Yes |
| Multi-Pi sync | Yes (omxplayer-sync) | Yes |
| Subtitles | Yes | No |
| Chapter skip | No | Yes |
| Seeking | Yes | Yes |
| Volume control | Yes | Yes |
| Loop | Yes | Yes |
| Runs on modern OS | No | Yes |
