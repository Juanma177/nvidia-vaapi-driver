#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/usr}"

echo "=== nvidia-vaapi-driver installer ==="
echo ""

# Detect NVIDIA driver version
NV_VER=$(dpkg -l 2>/dev/null | grep 'libnvidia-compute-.*amd64' | awk '{print $2}' | sed 's/libnvidia-compute-//' | sed 's/:amd64//' | head -1)
if [ -z "$NV_VER" ]; then
    echo "ERROR: NVIDIA driver not detected. Install the NVIDIA driver first."
    exit 1
fi
echo "NVIDIA driver: $NV_VER"

# Install build dependencies
echo ""
echo "[1/7] Installing build dependencies..."
sudo apt-get install -y --no-install-recommends \
    meson ninja-build gcc pkg-config \
    libva-dev libdrm-dev libegl-dev libffmpeg-nvenc-dev \
    2>&1 | tail -1

# 32-bit dependencies (for Steam Remote Play)
echo "[2/7] Installing 32-bit dependencies (for Steam)..."
if ! dpkg --print-foreign-architectures 2>/dev/null | grep -q i386; then
    sudo dpkg --add-architecture i386
    sudo apt-get update -qq 2>&1 | tail -1
fi
sudo apt-get install -y --no-install-recommends \
    gcc-multilib \
    libva-dev:i386 libdrm-dev:i386 libegl-dev:i386 \
    libnvidia-compute-${NV_VER}:i386 \
    libnvidia-encode-${NV_VER}:i386 \
    2>&1 | tail -1

# Build 64-bit
echo "[3/7] Building 64-bit driver + helper..."
meson setup "$SCRIPT_DIR/build64" "$SCRIPT_DIR" --wipe --prefix="$PREFIX" 2>&1 | tail -3
meson compile -C "$SCRIPT_DIR/build64" 2>&1 | tail -1

# Build 32-bit
echo "[4/7] Building 32-bit driver (cross-compile)..."
HAS_32BIT=0
if [ -f "$SCRIPT_DIR/cross-i386.txt" ]; then
    meson setup "$SCRIPT_DIR/build32" "$SCRIPT_DIR" --wipe --cross-file "$SCRIPT_DIR/cross-i386.txt" 2>&1 | tail -3
    meson compile -C "$SCRIPT_DIR/build32" 2>&1 | tail -1
    HAS_32BIT=1
fi

# Install
echo "[5/7] Installing drivers + helper..."
sudo meson install -C "$SCRIPT_DIR/build64" 2>&1 | tail -2
if [ "$HAS_32BIT" = "1" ]; then
    sudo mkdir -p /usr/lib/i386-linux-gnu/dri
    sudo cp "$SCRIPT_DIR/build32/nvidia_drv_video.so" /usr/lib/i386-linux-gnu/dri/nvidia_drv_video.so
    echo "  32-bit driver installed"
fi

# Systemd user service
echo "[6/7] Installing systemd user service..."
mkdir -p ~/.config/systemd/user
cat > ~/.config/systemd/user/nvenc-helper.service << 'EOF'
[Unit]
Description=NVENC encode helper for nvidia-vaapi-driver
Documentation=https://github.com/efortin/nvidia-vaapi-driver
After=graphical-session.target

[Service]
Type=simple
ExecStart=/usr/libexec/nvenc-helper
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
EOF

systemctl --user daemon-reload
systemctl --user enable nvenc-helper.service
systemctl --user restart nvenc-helper.service

# Verify
echo "[7/7] Verifying..."
sleep 1

systemctl --user is-active nvenc-helper.service >/dev/null 2>&1 \
    && echo "  nvenc-helper: running" \
    || echo "  nvenc-helper: FAILED"

vainfo --display drm --device /dev/dri/renderD128 2>&1 | grep -q 'VAEntrypointEncSlice' \
    && echo "  64-bit encode: OK" \
    || echo "  64-bit encode: FAILED"

[ "$HAS_32BIT" = "1" ] && echo "  32-bit driver: OK"

echo ""
echo "=== Done ==="
echo "No environment variables needed. Just launch Steam."
