#!/bin/bash
set -e

REPO="https://github.com/HorseyofCoursey/zeroplay.git"
BUILD_DIR="/tmp/zeroplay-install"

echo "ZeroPlay installer"
echo "------------------"

# Check we're on a Pi
if ! grep -q "Raspberry Pi" /proc/cpuinfo 2>/dev/null; then
    echo "warning: this doesn't look like a Raspberry Pi — continuing anyway"
fi

# Check for required tools
for cmd in git gcc make; do
    if ! command -v $cmd &>/dev/null; then
        echo "installing build tools..."
        sudo apt-get install -y git gcc make
        break
    fi
done

# Install dependencies
echo "installing dependencies..."
sudo apt-get install -y \
    pkgconf \
    libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev \
    libdrm-dev libasound2-dev

# Clone and build
echo "cloning zeroplay..."
rm -rf "$BUILD_DIR"
git clone --depth=1 "$REPO" "$BUILD_DIR"

echo "building..."
make -C "$BUILD_DIR" clean
make -C "$BUILD_DIR"

# Install
echo "installing to /usr/local/bin..."
sudo make -C "$BUILD_DIR" install

# Cleanup
rm -rf "$BUILD_DIR"

echo ""
echo "done! run: zeroplay <file>"
