# PR: Add NVENC Encoding Support via VA-API

> **Disclaimer:** This implementation was totally vibe coded in a single session — from zero to working Steam Remote Play on NVIDIA Linux in one sitting. I had a Windows + WSL long-running Ubuntu setup but was sad to reintroduce this at home when I switched to native Linux. Instead of going back to Windows, I decided to fix my Steam Remote Play setup with AI. It works, it's tested, but it carries the energy of 3AM debugging and "just one more fix". Review accordingly.

## TL;DR

This PR adds `VAEntrypointEncSlice` (hardware encoding) to nvidia-vaapi-driver by wrapping NVIDIA's NVENC API. Any application using VA-API for encoding — Steam Remote Play, ffmpeg, GStreamer, OBS — can now use NVIDIA hardware encoding on Linux.

The killer feature: a **shared memory bridge** that makes encoding work even when 32-bit CUDA is broken (Blackwell GPUs + driver 580+), which is the exact scenario that breaks Steam Remote Play for every NVIDIA user on Linux.

## What was broken

```
Steam Remote Play encoding pipeline on NVIDIA Linux:
1. Try NVENC direct → "NVENC - No CUDA support" (32-bit CUDA broken)
2. Try VA-API encode → fails (nvidia-vaapi-driver doesn't support it)
3. Fallback to libx264 software → 20fps, unusable
```

This has been open for 10+ years. Issue #116 (45+ thumbs up). Affects every NVIDIA GPU user on Linux who wants Steam Remote Play.

## What this PR does

### 1. VA-API encode support (H.264 + HEVC)

Adds `VAEntrypointEncSlice` for:
- H.264: Constrained Baseline, Main, High
- HEVC: Main, Main10 (10-bit)

After this, `vainfo` shows encode entrypoints alongside the existing decode entrypoints. ffmpeg `h264_vaapi` and `hevc_vaapi` work out of the box.

### 2. Shared memory bridge for 32-bit Steam

On Blackwell GPUs, 32-bit `cuInit()` fails with error 100. The entire nvidia-vaapi-driver depends on CUDA, so nothing works in 32-bit. Steam's encoding runs in a 32-bit process (`steamui.so`).

Solution: a 64-bit helper daemon (`nvenc-helper`) that does the CUDA/NVENC work. The 32-bit driver communicates via shared memory (for frame pixels) and a Unix socket (for control commands and encoded bitstream).

```
Steam 32-bit → vaDeriveImage → write NV12 pixels to host buffer
  → memcpy to shared memory (memfd, 3MB) 
  → signal via Unix socket (16 bytes)
    → nvenc-helper 64-bit: read from shm → NVENC encode
    ← HEVC/H.264 bitstream via socket (~10-30KB)
  ← VA-API coded buffer filled
← Steam streams to client
```

The bridge activates **only** when `cuInit()` fails. On systems where 32-bit CUDA works (Turing, Ampere, Ada), the driver uses NVENC directly — no helper, no overhead.

### 3. Everything else that was needed

Getting from "vainfo shows EncSlice" to "Steam Remote Play actually works" required fixing a cascade of issues:

| Fix | Why |
|-----|-----|
| `vaDeriveImage` implementation | Steam writes captured frames through derived images, not `vaPutImage` |
| DRM surface allocation without CUDA | GPU-backed surfaces via kernel DRM ioctls, no CUDA needed |
| NV12 pitch/height alignment | Encoder uses 1088 (MB-aligned), surface has 1080 — copy only 1080 lines |
| Frame snapshot before IPC send | Prevent tearing from Steam writing next frame while sending current |
| Periodic IDR keyframes (every 60 frames) | Steam sets `intra_period=3600` — client can't recover from packet loss |
| IDR on `idr_pic_flag` from picture params | Forward client keyframe requests to NVENC |
| Dead client timeout on helper socket | Helper was blocking forever on dead connections |
| NVIDIA opaque fds vs DMA-BUF fds | `cuImportExternalMemory` needs `nvFd`, not `drmFd` |

## Test results

| Test | Status |
|------|--------|
| vainfo encode entrypoints | PASS — 5 EncSlice profiles |
| H.264 1080p30 (ffmpeg) | PASS — High profile, valid output |
| HEVC 1080p30 (ffmpeg) | PASS — Main profile, valid output |
| HEVC Main10 10-bit | PASS — yuv420p10le |
| 1440p60 stress (60s) | PASS — 3600 frames, no crash |
| Bitrate control (CBR 5Mbps) | PASS — within 20% of target |
| NVDEC decode regression | PASS — unchanged |
| GPU encode (nvidia-smi) | PASS — 12% encoder util, 159fps |
| Sequential encodes (leak check) | PASS — 10 runs, 0 errors |
| 32-bit driver init | PASS — 5 encode, 0 decode entrypoints |
| Steam Remote Play (Mac Steam Link) | PASS — VAAPI H264, 60fps, 0% loss |
| Steam Remote Play (Legion Go) | PASS — VAAPI HEVC, 60fps |
| nvenc-helper systemd service | PASS — auto-start, auto-restart |

## Known limitations

### No B-frames

`frameIntervalP=1` always. NVENC with B-frames returns `NV_ENC_ERR_NEED_MORE_INPUT` for reordered frames. ffmpeg 6.x `vaapi_encode` asserts on the resulting empty coded buffer. Verified by testing — enabling B-frames crashes ffmpeg.

Not a problem: B-frames add latency, which is the opposite of what streaming needs. For offline transcoding, use `h264_nvenc`/`hevc_nvenc` directly.

### Packed headers

NVENC generates its own SPS/PPS/VPS headers. Application-provided packed headers are accepted but not injected. Works fine for ffmpeg and Steam.

### 32-bit encode-only

When the shared memory bridge is active (Blackwell 32-bit), only encoding works — no hardware decode. Steam only needs encode on the server side, so this is fine.

## Files changed

### New files (8)
| File | Lines | Role |
|------|-------|------|
| `src/nvenc.c` | ~450 | NVENC wrapper: session, encoder, buffers |
| `src/nvenc.h` | ~130 | NVENC context structures |
| `src/h264_encode.c` | ~115 | H.264 VA-API parameter handlers |
| `src/hevc_encode.c` | ~100 | HEVC VA-API parameter handlers |
| `src/encode_handlers.h` | ~20 | Encode handler declarations |
| `src/nvenc-helper.c` | ~870 | 64-bit encode daemon |
| `src/nvenc-ipc-client.c` | ~360 | Shared memory bridge client |
| `src/nvenc-ipc.h` | ~120 | Bridge protocol definitions |

### Modified files (4)
| File | Role |
|------|------|
| `src/vabackend.c` | Encode paths in all VA-API callbacks |
| `src/vabackend.h` | Encode fields in driver structures |
| `src/direct/direct-export-buf.c` | CUDA-optional surface allocation |
| `meson.build` | New sources + helper binary |

### Supporting files (4)
| File | Role |
|------|------|
| `cross-i386.txt` | Meson cross-compilation for 32-bit |
| `install.sh` | Build + install both archs + systemd |
| `nvenc-helper.service` | Systemd user service |
| `docs/nvenc-encoding.md` | Full architecture documentation |
| `tests/encoding-tests.md` | 12 test cases |

## Comparison with PR #425

PR #425 by alper-han also adds NVENC encoding. Key differences:

| | PR #425 | This PR |
|-|---------|---------|
| Codecs | H.264 only | H.264 + HEVC + Main10 |
| 32-bit Steam | Not addressed | Full shared memory bridge |
| B-frames | Supported | Disabled (ffmpeg compat) |
| Packed headers | Full support | NVENC-generated only |
| File count | 27 files changed | 12 new + 4 modified |
| Steam tested | Not mentioned | Verified on Mac + Legion Go |

The approaches are complementary. PR #425 has a cleaner encode abstraction layer and packed header support. This PR has the 32-bit bridge and HEVC. Both solve the core problem of making `VAEntrypointEncSlice` available on NVIDIA.

## How to test

```bash
# Install
./install.sh

# Verify
vainfo --display drm --device /dev/dri/renderD128

# Encode
ffmpeg -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=5:size=1920x1080:rate=30 \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -qp 20 test.mp4

# Steam Remote Play: just launch Steam, no env vars needed
steam
```

## Hardware tested

- GPU: NVIDIA GeForce RTX 5070 Ti (Blackwell, 16GB GDDR7)
- Driver: 580.126.09 (open kernel modules)
- OS: Ubuntu 24.04 LTS
- CUDA: 13.0
- Steam client: 32-bit (steamui.so)
- Clients: macOS Steam Link, SteamOS Legion Go
