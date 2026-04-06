#!/bin/bash
set -e

IMAGE="zeroplay-cross"

# Register QEMU binfmt handlers if arm64 isn't already available
if ! docker buildx inspect --bootstrap 2>/dev/null | grep -q linux/arm64; then
    echo "registering QEMU binfmt handlers..."
    docker run --rm --privileged multiarch/qemu-user-static --reset -p yes
fi

echo "building zeroplay for aarch64..."
docker build --platform linux/arm64 -f Dockerfile.cross -t "$IMAGE" .

echo "extracting binary..."
CONTAINER=$(docker create "$IMAGE")
docker cp "$CONTAINER":/build/zeroplay ./zeroplay
docker rm "$CONTAINER" >/dev/null

echo ""
file zeroplay
echo ""
echo "done! copy to your Pi with: scp zeroplay pi@<ip>:~/"
