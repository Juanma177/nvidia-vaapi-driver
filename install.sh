#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-/usr}"

echo "=== nvidia-vaapi-driver installer ==="
echo "Source: $SCRIPT_DIR"
echo "Prefix: $PREFIX"
echo ""

# Check dependencies
echo "[1/7] Checking dependencies..."
for cmd in meson ninja gcc pkg-config; do
    command -v $cmd >/dev/null || { echo "ERROR: $cmd not found"; exit 1; }
done
pkg-config --exists libva ffnvcodec libdrm egl || { echo "ERROR: missing dev packages"; exit 1; }

# Build 64-bit
echo "[2/7] Building 64-bit driver + helper..."
meson setup "$SCRIPT_DIR/build64" "$SCRIPT_DIR" --wipe --prefix="$PREFIX" 2>&1 | tail -3
meson compile -C "$SCRIPT_DIR/build64" 2>&1 | tail -1

# Build 32-bit (optional)
echo "[3/7] Building 32-bit driver (cross-compile)..."
if [ -f "$SCRIPT_DIR/cross-i386.txt" ] && dpkg --print-foreign-architectures 2>/dev/null | grep -q i386; then
    if pkg-config --exists libva libdrm egl 2>/dev/null; then
        meson setup "$SCRIPT_DIR/build32" "$SCRIPT_DIR" --wipe --cross-file "$SCRIPT_DIR/cross-i386.txt" 2>&1 | tail -3
        meson compile -C "$SCRIPT_DIR/build32" 2>&1 | tail -1
        HAS_32BIT=1
    else
        echo "  Skipped: missing i386 dev packages"
        HAS_32BIT=0
    fi
else
    echo "  Skipped: i386 architecture not enabled"
    HAS_32BIT=0
fi

# Install
echo "[4/7] Installing 64-bit driver + helper..."
sudo meson install -C "$SCRIPT_DIR/build64" 2>&1 | tail -2

if [ "$HAS_32BIT" = "1" ]; then
    echo "[5/7] Installing 32-bit driver..."
    sudo mkdir -p /usr/lib/i386-linux-gnu/dri
    sudo cp "$SCRIPT_DIR/build32/nvidia_drv_video.so" /usr/lib/i386-linux-gnu/dri/nvidia_drv_video.so
    echo "  Installed to /usr/lib/i386-linux-gnu/dri/"
else
    echo "[5/7] Skipping 32-bit install"
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

echo "[7/7] Verifying..."
sleep 1

# Verify helper
if systemctl --user is-active nvenc-helper.service >/dev/null 2>&1; then
    echo "  nvenc-helper: running"
else
    echo "  nvenc-helper: FAILED (check: systemctl --user status nvenc-helper)"
fi

# Verify 64-bit driver
if vainfo --display drm --device /dev/dri/renderD128 2>&1 | grep -q 'VAEntrypointEncSlice'; then
    echo "  64-bit encode: OK"
else
    echo "  64-bit encode: FAILED"
fi

# Verify 32-bit driver
if [ "$HAS_32BIT" = "1" ]; then
    echo "  32-bit driver: installed at /usr/lib/i386-linux-gnu/dri/nvidia_drv_video.so"
fi

echo ""
echo "=== Done ==="
echo "Files installed:"
echo "  /usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so  (64-bit VA-API driver)"
[ "$HAS_32BIT" = "1" ] && echo "  /usr/lib/i386-linux-gnu/dri/nvidia_drv_video.so   (32-bit VA-API driver)"
echo "  /usr/libexec/nvenc-helper                           (64-bit encode daemon)"
echo "  ~/.config/systemd/user/nvenc-helper.service         (systemd user service)"
echo ""
echo "No environment variables needed. Steam Remote Play should work automatically."
