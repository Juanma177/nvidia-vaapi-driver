# NVENC Encoding Support for nvidia-vaapi-driver

## The Problem

The `nvidia-vaapi-driver` (by elFarto) implements VA-API for NVIDIA GPUs but only supports **decoding** (NVDEC). It exposes `VAEntrypointVLD` for H.264, HEVC, AV1, VP8, VP9, etc.

On Linux, applications that use VA-API for hardware encoding (Steam Remote Play, GStreamer, ffmpeg `h264_vaapi`/`hevc_vaapi`) cannot use NVIDIA GPUs because the driver doesn't expose `VAEntrypointEncSlice`.

### Impact on Steam Remote Play

Steam Remote Play on Linux uses VA-API for hardware video encoding:

- **AMD GPUs**: Mesa drivers expose `VAEntrypointEncSlice` → works
- **Intel GPUs**: iHD driver exposes `VAEntrypointEncSlice` → works
- **NVIDIA GPUs**: `nvidia-vaapi-driver` only exposes `VAEntrypointVLD` → Steam falls back to `libx264` software encoding → 20fps, unusable

This has been reported for 10+ years (issue #116 on the project, issue #12639 on steam-for-linux).

### The 32-bit CUDA Problem

Steam's encoding pipeline runs in a **32-bit process** (`steamui.so` inside the 32-bit `steam` binary). On modern NVIDIA drivers (580+) with Blackwell GPUs (RTX 50xx), 32-bit `cuInit()` returns error 100 ("no CUDA-capable device detected"). This breaks:

- Steam's direct NVENC path (`NVENC - No CUDA support` in logs)
- Any 32-bit VA-API driver that depends on CUDA

This is a fundamental NVIDIA driver limitation — 32-bit CUDA doesn't support Blackwell.

## The Solution

### Two encode paths

The driver implements two encode paths, selected automatically based on CUDA availability:

#### 1. Direct NVENC (when CUDA works)

Used by: 64-bit processes on any GPU, 32-bit processes on pre-Blackwell GPUs.

```
Application → VA-API → nvidia_drv_video.so
  → CUDA context → NVENC session → hardware encode
  ← encoded bitstream via VA-API coded buffer
```

No helper process needed. The driver talks to NVENC directly via CUDA.

#### 2. Shared Memory Bridge (when CUDA is unavailable)

Used by: 32-bit processes on Blackwell GPUs (cuInit fails).

```
Application (32-bit) → VA-API → nvidia_drv_video.so (32-bit)
  │
  │  vaDeriveImage: maps surface to host-memory buffer
  │  Application writes NV12 frame data into the buffer
  │
  │  vaEndPicture: triggers encode via shared memory bridge
  │    1. memcpy frame to shared memory region (memfd)
  │    2. send CMD_ENCODE_SHM (16 bytes) via Unix socket
  │
  └──── Unix socket ────→ nvenc-helper (64-bit daemon)
                            │
                            │  Reads frame from shared memory
                            │  memcpy to NVENC input buffer
                            │  nvEncEncodePicture (hardware)
                            │
                            │  Encoded bitstream (~5-30KB)
                            ├──── Unix socket ────→ back to driver
                            │
  ← VA-API coded buffer filled with bitstream
```

### How the path is selected

```
init() constructor:
  cu->cuInit(0)
    ├─ SUCCESS → cudaAvailable = true  → Direct NVENC path
    └─ FAIL    → cudaAvailable = false → Shared memory bridge
```

The decision is based **only** on whether `cuInit()` succeeds, not on the process architecture. A 32-bit process on a Turing/Ampere/Ada GPU where CUDA works will use the direct path — no bridge needed.

## Architecture

### Files

| File | Role |
|------|------|
| `src/nvenc.c` | Core NVENC wrapper: session, encoder init, buffer management |
| `src/nvenc.h` | NVENC context structures, API declarations |
| `src/h264_encode.c` | H.264 VA-API parameter handlers (seq, pic, slice, misc) |
| `src/hevc_encode.c` | HEVC VA-API parameter handlers |
| `src/encode_handlers.h` | Header declaring all encode handler functions |
| `src/nvenc-helper.c` | 64-bit encode helper daemon (standalone binary) |
| `src/nvenc-ipc-client.c` | Bridge client: shared memory + Unix socket |
| `src/nvenc-ipc.h` | Bridge protocol definitions |
| `src/vabackend.c` | Modified: encode paths in VA-API callbacks |
| `src/vabackend.h` | Modified: encode fields in driver structures |
| `src/direct/direct-export-buf.c` | Modified: CUDA-optional surface allocation |
| `cross-i386.txt` | Meson cross-compilation file for 32-bit build |
| `install.sh` | Build + install script (both architectures + systemd) |
| `nvenc-helper.service` | Systemd user service for the helper daemon |

### Data flow detail

#### Frame data transfer (shared memory)

The `nvenc-helper` creates a shared memory region via `memfd_create()` during `CMD_INIT`. The memfd file descriptor is sent to the driver via `SCM_RIGHTS` ancillary data on the Unix socket. Both processes `mmap()` the same memory.

```
Driver (32-bit)                     Helper (64-bit)
─────────────────                   ──────────────────
                  CMD_INIT
       ──────────────────────→
                                    memfd_create("nvenc-frame")
                                    mmap(shm_fd)
       ←── shm_fd via SCM_RIGHTS ──
mmap(shm_fd)

Per frame:
memcpy(shm, pixels, 3MB)           (shared memory — no transfer)
       ── CMD_ENCODE_SHM (16B) ──→
                                    read from shm (same physical pages)
                                    memcpy to NVENC input buffer
                                    nvEncEncodePicture
       ←── bitstream (5-30KB) ────
```

Frame data never crosses the socket. Only the small command header (16 bytes) and the encoded bitstream (~5-30KB) go through the socket. The 3MB NV12 frame stays in shared memory.

If `memfd_create` fails, the driver falls back to sending frame data through the socket (CMD_ENCODE with full 3MB payload).

#### Control flow (Unix socket)

| Command | Direction | Payload | Description |
|---------|-----------|---------|-------------|
| `CMD_INIT` | driver → helper | Init params (40B) | Initialize encoder, create shm |
| `CMD_ENCODE_SHM` | driver → helper | Encode params (16B) | Encode frame from shm |
| `CMD_ENCODE` | driver → helper | Params + frame data (3MB) | Fallback: encode from socket |
| `CMD_CLOSE` | driver → helper | (none) | Close encoder session |
| Response | helper → driver | Status + bitstream | Encoded HEVC/H.264 data |

#### Surface management in bridge mode

When CUDA is unavailable, surfaces need special handling:

1. **GPU memory allocation**: The DRM direct backend (`nv-driver.c`) allocates GPU memory via kernel DRM ioctls — no CUDA needed. Surfaces get real GPU backing for OpenGL interop.

2. **CUDA import skipped**: `direct_allocateBackingImage()` skips `import_to_cuda()` when `cudaAvailable=false`. The NVIDIA opaque fds (`nvFd`) are preserved for potential use by the helper.

3. **Pixel data via vaDeriveImage**: Steam writes captured frames through `vaDeriveImage()` → `vaMapBuffer()` → host memory write. The driver allocates `hostPixelData` on the surface and returns a `VAImage` backed by this buffer.

4. **Encode reads from host memory**: `nvEndPictureEncodeIPC()` copies `hostPixelData` to shared memory, then signals the helper.

## Edge Cases

### Steam reinitializes the encoder frequently

Steam's ffmpeg creates and destroys the VA-API encoder multiple times during a streaming session (probing, resolution changes, bitrate adaptation). Each reinit:

1. Destroys context → IPC close → helper closes NVENC session
2. Creates new surfaces + context → new IPC connection → helper creates new session + shm

The helper handles this via the accept loop — each client connection is a separate encode session.

### Encoder height vs surface height

HEVC/H.264 encoders require macroblock-aligned dimensions (multiples of 16/64). A 1920x1080 surface becomes a 1920x1088 encoder. The driver sends the **surface dimensions** (1080) to the helper, which copies only 1080 lines and zero-pads the 8-line remainder.

### IDR keyframe recovery

Steam sets `intra_period=3600` (60 seconds between keyframes). A single lost network packet causes the client to lose sync and request a new keyframe. Without periodic IDR frames, the client freezes for up to 60 seconds.

Fix: the helper forces an IDR every 60 frames (~1 second at 60fps) regardless of `intra_period`. When the VA-API `idr_pic_flag` is set in picture params, an IDR is also forced immediately.

### Frame tearing prevention

Steam reuses the same surface for every frame. Without protection, the helper could read a partially-written frame (Steam writes frame N+1 while the helper encodes frame N from the same buffer).

Fix: the driver copies the frame to shared memory atomically before signaling the helper. The shared memory acts as a snapshot buffer.

### Dead client detection

If the Steam process exits without sending `CMD_CLOSE`, the helper's `recv()` blocks forever on the dead socket. The helper sets `SO_RCVTIMEO = 5 seconds` on client sockets. After 5 seconds of silence, it closes the session and returns to accepting new connections.

### Object ID growth

Each `vaDeriveImage()` call creates new `NVImage` and `NVBuffer` objects with incrementing IDs. Steam calls this 60 times per second. The objects are destroyed by `vaDestroyImage()`, but the ID counter grows monotonically. This is normal — the IDs are `uint32_t` and won't wrap in any practical session.

The derived image buffer is marked with a sentinel (`offset = (size_t)-1`) so `vaDestroyImage` doesn't free the surface's `hostPixelData` (the surface owns that memory).

### No B-frames

B-frames are disabled (`frameIntervalP=1`) because NVENC returns `NV_ENC_ERR_NEED_MORE_INPUT` for non-reference frames, producing empty coded buffers. ffmpeg's `vaapi_encode` (through version 6.x) asserts on empty coded buffers.

This is optimal for streaming (low latency). For offline transcoding with better compression, use ffmpeg's native NVENC encoders:
```bash
ffmpeg -i input.mp4 -c:v h264_nvenc -preset p7 -bf 2 output.mp4
```

### DMA-BUF path (unused by Steam)

The driver implements a DMA-BUF encode path (`CMD_ENCODE_DMABUF`) that sends NVIDIA opaque fds to the helper for CUDA import. This path exists for future use but is not triggered by Steam (Steam uses `vaDeriveImage` + host memory, not DMA-BUF surface import).

## Supported encode profiles

| VA-API Profile | NVENC Codec | NVENC Profile | Pixel Format |
|----------------|-------------|---------------|--------------|
| VAProfileH264ConstrainedBaseline | H.264 | Baseline | NV12 |
| VAProfileH264Main | H.264 | Main | NV12 |
| VAProfileH264High | H.264 | High | NV12 |
| VAProfileHEVCMain | HEVC | Main | NV12 |
| VAProfileHEVCMain10 | HEVC | Main10 | P010 |

## Installation

```bash
git clone https://github.com/efortin/nvidia-vaapi-driver.git
cd nvidia-vaapi-driver
git checkout feat/nvenc-support
./install.sh
```

The install script:
1. Builds the 64-bit driver + `nvenc-helper` binary
2. Cross-compiles the 32-bit driver (if i386 architecture is enabled)
3. Installs drivers to `/usr/lib/{x86_64,i386}-linux-gnu/dri/`
4. Installs helper to `/usr/libexec/nvenc-helper`
5. Creates and enables a systemd user service for the helper
6. Verifies the installation

No environment variables are needed. libva auto-detects the NVIDIA driver from the DRM device, and `NVD_BACKEND` defaults to `direct`.

## Debugging

Enable driver logging:
```bash
export NVD_LOG=1          # log to stdout
export NVD_LOG=/tmp/nvd.log  # log to file
```

Check helper status:
```bash
systemctl --user status nvenc-helper
journalctl --user -u nvenc-helper -f
```

Check Steam streaming:
```bash
cat ~/.steam/debian-installation/logs/streaming_log.txt | grep -iE 'vaapi|encoder|failed|codec'
```

Key indicators in Steam log:
- `VAAPI H264` or `VAAPI HEVC` = our encoder is active
- `libx264` = fallback to software (our driver not loaded)
- `NVENC - No CUDA support` = Steam's direct NVENC failed (expected on 32-bit Blackwell)
