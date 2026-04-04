# PR: Add NVENC Encoding Support via VA-API

> **Disclaimer:** I had a Windows + WSL long-running Ubuntu setup but was sad to reintroduce this at home when I switched to native Linux. Instead of going back to Windows, I decided to fix my Steam Remote Play setup with AI. It works, it's tested, but it carries the energy of 3AM debugging and "just one more fix". Review accordingly.

## TL;DR

This PR adds `VAEntrypointEncSlice` (hardware encoding) to nvidia-vaapi-driver by wrapping NVIDIA's NVENC API. Any application using VA-API for encoding — Steam Remote Play, ffmpeg, GStreamer, OBS, Chromium — can now use NVIDIA hardware encoding on Linux.

For Blackwell GPUs (RTX 50xx) where NVIDIA dropped 32-bit CUDA support, a **shared memory bridge** delegates encoding to a 64-bit helper daemon. This is the exact scenario that breaks Steam Remote Play for every NVIDIA user on Linux.

## What was broken

```
Steam Remote Play encoding pipeline on NVIDIA Linux:
1. Try NVENC direct → "NVENC - No CUDA support" (32-bit CUDA broken)
2. Try VA-API encode → fails (nvidia-vaapi-driver doesn't support it)
3. Fallback to libx264 software → 20fps, unusable
```

This has been open for 2+ years. Issue #116 (45+ thumbs up). Affects every NVIDIA GPU user on Linux who wants Steam Remote Play.

## What this PR does

### 1. VA-API encode support (H.264 + HEVC)

Adds `VAEntrypointEncSlice` for:
- H.264: Constrained Baseline, Main, High
- HEVC: Main, Main10 (10-bit)

After this, `vainfo` shows encode entrypoints alongside the existing decode entrypoints. ffmpeg `h264_vaapi` and `hevc_vaapi` work out of the box.

### 2. Shared memory bridge (when CUDA is unavailable)

On Blackwell GPUs, 32-bit `cuInit()` fails with error 100. Steam's encoding runs in a 32-bit process (`steamui.so`).

Solution: a 64-bit helper daemon (`nvenc-helper`) that does the CUDA/NVENC work. The 32-bit driver communicates via shared memory (for frame pixels) and a Unix socket (for control and bitstream).

```
Steam 32-bit → vaDeriveImage → writes NV12 directly to shared memory
  → 16-byte signal via Unix socket
    → nvenc-helper 64-bit: cuMemcpy2D from SHM → persistent GPU buffer → NVENC
    ← HEVC/H.264 bitstream via socket (~10-30KB)
  ← VA-API coded buffer filled
← Steam streams to client
```

The bridge activates **only** when `cuInit()` fails. On systems where CUDA works (64-bit, or 32-bit pre-Blackwell), the driver uses NVENC directly — no helper, no overhead.

### 3. Everything else that was needed

Getting from "vainfo shows EncSlice" to "Steam Remote Play actually works" required fixing a cascade of issues:

| Fix | Why |
|-----|-----|
| `vaDeriveImage` implementation | Steam writes captured frames through derived images, not `vaPutImage` |
| DRM surface allocation without CUDA | GPU-backed surfaces via kernel DRM ioctls, no CUDA needed |
| NV12 pitch/height alignment | Encoder uses 1088 (MB-aligned), surface has 1080 — copy only 1080 lines |
| Periodic IDR keyframes (every 60 frames) | Steam sets `intra_period=3600` — client can't recover from packet loss |
| IDR on `idr_pic_flag` from picture params | Forward client keyframe requests to NVENC |
| Dead client detection via poll() timeout | Helper was blocking forever on dead connections |
| NVIDIA opaque fds vs DMA-BUF fds | `cuImportExternalMemory` needs `nvFd`, not `drmFd` |

## Test results

46 automated tests via `meson test`, plus manual Steam validation.

### Automated C test suite (`meson test`)

| Suite | Tests | Status |
|-------|-------|--------|
| `test_encode` — full encode cycles, leak checks | 11 | All PASS |
| `test_encode_config` — capabilities, error paths, surfaces | 35 | All PASS |

### Manual integration tests

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

## Performance optimizations

The shared memory bridge went through several optimization rounds:

| Optimization | Encode time | What changed |
|-------------|-------------|--------------|
| Baseline (socket transfer) | ~8ms | 3MB frame sent over Unix socket per frame |
| Shared memory (memfd) | ~6ms | Frame data in SHM, only 16-byte signal over socket |
| SHM zero-copy redirect | ~5ms | `vaDeriveImage` maps directly to SHM, skip memcpy |
| Eliminate redundant memset | ~4ms | Only zero 8 padding rows, not entire 3MB buffer |
| Persistent CUDA buffer + cuMemcpy2D | ~3ms | GPU DMA engine handles host→device + pitch in HW |
| CUDA context kept active per session | **~2.8ms** | Eliminate per-frame cuCtxPushCurrent/PopCurrent |

Final pipeline (1080p NV12):
```
Steam writes NV12 → SHM (zero-copy via vaDeriveImage)
  → 16-byte signal via socket
  → Helper: 2× cuMemcpy2D (host→device, DMA engine) → persistent CUDA buffer
  → NVENC encodes from VRAM (no PCIe upload at encode time)
  → Bitstream back via socket (~10-30KB)
```

## Code hardening

All code reviewed for production reliability:
- Zero warnings at `-Dwarning_level=3`, zero cppcheck issues
- All CUDA/NVENC return values checked (no silent failures)
- Socket frame_size capped at 64MB (prevents malloc bomb from corrupt data)
- File descriptors tracked and closed (no fd leaks, verified with /proc/pid/fd)
- Dead client detection via poll() with 5s timeout
- Derived image buffer ownership tracked (sentinel prevents double-free)
- DMA-BUF fds properly closed on partial import failure
- NVIDIA opaque fds closed in surface destroy
- Pre-allocated bitstream output buffer (no per-frame malloc)
- CUDA context kept pushed for entire client session (no per-frame sync)

## What Steam actually uses

From streaming logs, Steam's ffmpeg VA-API encode pipeline uses:

| VA-API feature | Used by Steam | Status |
|---|---|---|
| Sequence params (resolution, bitrate, framerate, GOP) | Yes | Fully mapped to NVENC |
| Picture params (coded_buf, idr_pic_flag) | Yes | Working, IDR forwarded |
| Rate control misc (bits_per_second, target_percentage) | Yes | Applied to NVENC RC |
| Framerate misc | Yes | Applied |
| HRD misc (buffer_size) | Yes | Applied to NVENC vbvBufferSize |
| Packed headers (SEQ+PIC+SLICE+MISC) | Yes | Accepted (NVENC generates its own, no warning) |
| Quality level | quality=0 (default) | VAConfigAttribEncQualityRange reported |
| vaDeriveImage + vaMapBuffer | Yes (every frame) | Implemented, zero-copy SHM redirect |
| vaExportSurfaceHandle | No | Implemented but Steam doesn't call it |
| vaPutImage | No | Implemented but Steam uses vaDeriveImage instead |

## Known limitations

### No B-frames

`frameIntervalP=1` always. NVENC with `enablePTD=1` and B-frames returns `NV_ENC_ERR_NEED_MORE_INPUT` for reordered frames, producing empty coded buffers. ffmpeg 6.x `vaapi_encode` asserts on empty coded buffers. With `enablePTD=0`, NVENC requires full DPB (Decoded Picture Buffer) reference frame management which Intel drivers handle in hardware but NVENC delegates to the caller.

Not a problem for streaming (B-frames add latency). For offline transcoding with B-frames, use `h264_nvenc`/`hevc_nvenc` directly.

### Packed headers

Driver advertises full packed header support (SEQ+PIC+SLICE+MISC). NVENC generates its own SPS/PPS/VPS headers internally. Application-provided packed headers are accepted and silently skipped.

### 32-bit encode-only

When the shared memory bridge is active (CUDA unavailable), only encoding works — no hardware decode. Steam only needs encode on the server side, so this is fine.

### HDR

VA-API encode specification does not include color metadata fields (colour_primaries, transfer_characteristics) in sequence parameter structs. Intel drivers have the same limitation — HDR metadata only passes through packed headers (which NVENC generates internally). HDR encode requires direct NVENC (`hevc_nvenc` with `-color_primaries bt2020`).

## Files changed

### New files (7)
| File | Role |
|------|------|
| `src/nvenc.c` | NVENC wrapper: session, encoder, buffers |
| `src/nvenc.h` | NVENC context structures + encode handler declarations |
| `src/h264_encode.c` | H.264 VA-API parameter handlers |
| `src/hevc_encode.c` | HEVC VA-API parameter handlers |
| `src/nvenc-helper.c` | 64-bit encode daemon |
| `src/nvenc-ipc-client.c` | Shared memory bridge client |
| `src/nvenc-ipc.h` | Bridge protocol definitions |

### Modified files (4)
| File | Role |
|------|------|
| `src/vabackend.c` | Encode paths in all VA-API callbacks |
| `src/vabackend.h` | Encode fields in driver structures |
| `src/direct/direct-export-buf.c` | CUDA-optional surface allocation |
| `meson.build` | New sources + helper binary + test targets |

### Test files (3)
| File | Role |
|------|------|
| `tests/test_encode.c` | 11 encode cycle integration tests |
| `tests/test_encode_config.c` | 35 config/capability/surface tests |
| `tests/test_common.h` | Shared test framework |

### Supporting files
| File | Role |
|------|------|
| `cross-i386.txt` | Meson cross-compilation for 32-bit |
| `install.sh` | Auto-detects driver version, installs all deps + builds + systemd |
| `nvenc-helper.service` | Systemd user service |
| `docs/nvenc-encoding.md` | Full architecture documentation |
| `docs/pr-summary.md` | This document |
| `tests/encoding-tests.md` | Manual test documentation + edge cases |

## Comparison with PR #425

PR #425 by alper-han also adds NVENC encoding. Key differences:

| | PR #425 | This PR |
|-|---------|---------|
| Codecs | H.264 only | H.264 + HEVC + Main10 |
| 32-bit Steam | Not addressed | Full shared memory bridge |
| B-frames | Attempted (requires DPB mgmt) | Disabled (ffmpeg 6.x compat) |
| Packed headers | Injection support | Accepted, NVENC-generated |
| File count | 27 files changed | 7 new + 4 modified |
| Steam tested | Not mentioned | Verified on Mac + Legion Go |

The approaches are complementary. PR #425 has a cleaner encode abstraction layer. This PR has the 32-bit bridge and HEVC. Both solve the core problem of making `VAEntrypointEncSlice` available on NVIDIA.

## How to test

```bash
git clone https://github.com/efortin/nvidia-vaapi-driver
cd nvidia-vaapi-driver && git checkout feat/nvenc-support
./install.sh
sudo reboot
# Then just launch Steam — no env vars needed
```

## Hardware tested

- GPU: NVIDIA GeForce RTX 5070 Ti (Blackwell, 16GB GDDR7)
- Driver: 580.126.09 (open kernel modules)
- OS: Ubuntu 24.04 LTS
- CUDA: 13.0
- Steam client: 32-bit (steamui.so)
- Clients: macOS Steam Link, SteamOS Legion Go
