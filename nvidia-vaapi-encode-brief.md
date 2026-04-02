# nvidia-vaapi-driver: Adding NVENC Encoding Support via VA-API

## Context

The `nvidia-vaapi-driver` project (by elFarto) is a VA-API implementation for NVIDIA GPUs that currently only supports **decoding** (NVDEC). It exposes `VAEntrypointVLD` for various codecs (H.264, HEVC, AV1, VP8, VP9, MPEG2, VC1).

**The goal**: Add **encoding support** (`VAEntrypointEncSlice`) by wrapping NVIDIA's NVENC API behind the VA-API encoding interface. This would allow any application that uses VA-API for encoding (Steam Remote Play, GStreamer, ffmpeg via `h264_vaapi`/`hevc_vaapi`) to use NVIDIA hardware encoding on Linux.

## Why This Matters

On Linux, Steam Remote Play uses VA-API for hardware video encoding:
- **AMD GPUs**: Mesa drivers expose `VAEntrypointEncSlice` natively → Steam encodes via VA-API → works perfectly
- **Intel GPUs**: iHD driver exposes `VAEntrypointEncSlice` natively → Steam encodes via VA-API → works perfectly  
- **NVIDIA GPUs**: `nvidia-vaapi-driver` only exposes `VAEntrypointVLD` (decode) → Steam tries NVENC direct (broken 32-bit libs on modern drivers 570+/Blackwell) → falls back to libx264 software encoding → 20fps, unusable

Steam's encoding pipeline on Linux:
1. Try NVENC direct → fails (`NVENC - No CUDA support`, can't load 32-bit CUDA libs)
2. Try VA-API encode (`VAEntrypointEncSlice`) → fails (nvidia-vaapi-driver doesn't support it)
3. Fallback to libx264 software encoding → slow, high latency

If we add `VAEntrypointEncSlice` to this driver, **step 2 succeeds** and Steam encodes via VA-API → NVENC automatically. No changes needed to Steam (closed source). This fixes the problem for ALL applications using VA-API encode on NVIDIA.

This is a 10+ year old bug affecting every NVIDIA GPU user on Linux who wants Steam Remote Play. Issue #116 on the project has 45+ thumbs up. Issue #12639 on steam-for-linux confirms the problem persists with the latest Blackwell GPUs and driver 590+.

## Current Architecture

### Project Structure
```
nvidia-vaapi-driver/
├── src/
│   ├── vabackend.c          ← Main entry point, implements VA-API vtable
│   ├── h264.c               ← H.264 decode via NVDEC
│   ├── hevc.c               ← HEVC decode via NVDEC
│   ├── av1.c                ← AV1 decode via NVDEC
│   ├── vp8.c, vp9.c         ← VP8/VP9 decode
│   ├── mpeg2.c, mpeg4.c     ← MPEG decode
│   ├── vc1.c, jpeg.c        ← VC1/JPEG decode
│   ├── export-buf.c         ← DMA-BUF export for surface sharing
│   ├── list.c               ← Utility functions
│   └── direct/
│       ├── nv-driver.c      ← Direct backend: talks to NVIDIA DRM driver
│       └── direct-export-buf.c ← Direct backend buffer export
├── nvidia-include/           ← Headers from NVIDIA open-gpu-kernel-modules
├── meson.build               ← Build system
└── README.md
```

### How the driver works
1. **Entry point**: `__vaDriverInit_1_0` in `vabackend.c` — called by libva when loading the driver
2. **Backend selection**: EGL (broken on driver 525+) or **Direct** (current, uses `/dev/dri/renderD128`)
3. **Profile/Entrypoint registration**: Currently registers only `VAEntrypointVLD` for each codec
4. **Codec callbacks**: Each codec file (h264.c, hevc.c...) provides `beginPicture`, `renderPicture`, `endPicture` callbacks for decoding
5. **Dependencies**: `libva`, `ffnvcodec` (nv-codec-headers — includes BOTH NVDEC and NVENC headers), `gstreamer-codecparsers`, `EGL/DRM`

### Key insight
The project already depends on `ffnvcodec` (nv-codec-headers) which contains the NVENC API headers (`nvEncodeAPI.h`). The NVENC structs and function declarations are already available — no new dependency needed.

## What Needs To Be Done

### Phase 1: Register encoding entrypoints
In `vabackend.c`, where profiles are registered with `VAEntrypointVLD`, add `VAEntrypointEncSlice` for:
- H.264 (Main, High, ConstrainedBaseline)
- HEVC (Main, Main10)

After this phase, `vainfo` should show `VAEntrypointEncSlice` lines alongside the existing `VAEntrypointVLD` lines.

### Phase 2: Implement encoding callbacks
Create new files (e.g., `h264_encode.c`, `hevc_encode.c`) that implement the VA-API encoding callbacks:
- `vaCreateConfig` for encode configs
- `vaCreateContext` — open NVENC session (`NvEncOpenEncodeSessionEx`)
- `vaCreateBuffer` — handle encode buffer types (`VAEncSequenceParameterBufferH264`, `VAEncPictureParameterBufferH264`, `VAEncSliceParameterBufferH264`, `VAEncCodedBufferType`)
- `vaBeginPicture` / `vaRenderPicture` / `vaEndPicture` — translate VA-API encode params to NVENC params and call `NvEncEncodePicture`
- `vaMapBuffer` for coded buffer — retrieve encoded bitstream via `NvEncLockBitstream`
- `vaSyncSurface` — wait for encode completion

### Phase 3: Surface/buffer management
- Handle input surfaces (NV12 frames to encode) — register with NVENC via `NvEncRegisterResource`
- Handle output buffers (encoded bitstream) — allocate NVENC output bitstream buffers
- Map between VA-API surface IDs and NVENC resource handles

## Key References

### 1. FFmpeg `libavcodec/nvenc.c` (https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/nvenc.c)
Complete NVENC implementation in C. Shows:
- How to dynamically load `libnvidia-encode.so` and resolve NVENC functions
- How to open encode sessions and initialize the encoder
- How to map presets, profiles, rate control modes
- How to manage input/output buffers
- How to handle the encode pipeline (register → map → encode → lock bitstream → unmap)

### 2. Intel VA-API driver (`intel-vaapi-driver`, https://github.com/intel/intel-vaapi-driver)
Reference VA-API encode implementation. Shows:
- Which VA-API callbacks need to be implemented for encoding
- How `VAEncSequenceParameterBuffer*`, `VAEncPictureParameterBuffer*`, `VAEncSliceParameterBuffer*` structures are processed
- How coded buffers (`VAEncCodedBufferType`) are managed
- How to report encoding capabilities via `vaGetConfigAttributes`

### 3. NVIDIA Video SDK Samples (https://github.com/NVIDIA/video-sdk-samples)
Encoding examples showing the NVENC workflow:
- `NvEncoder.h/cpp` — encoder wrapper class with full lifecycle
- `nvEncodeAPI.h` — complete NVENC API reference
- Shows buffer format handling, preset configuration, bitstream output

### 4. nv-codec-headers (already a dependency)
The `ffnvcodec` headers in the project already include:
- `dynlink_nvcuvid.h` (decode — currently used)
- `nvEncodeAPI.h` (encode — NOT yet used, but available)
- `dynlink_loader.h` (dynamic loading helpers)

## Hardware Available for Testing

- **GPU**: NVIDIA GeForce RTX 5070 Ti (Blackwell, 16GB GDDR7)
- **Driver**: 580.126.09 (open kernel modules)
- **OS**: Ubuntu 24.04 LTS
- **CUDA**: 13.0
- **Current vainfo output**: All profiles show `VAEntrypointVLD` only (decode)
- **Target**: See `VAEntrypointEncSlice` for H.264 and HEVC profiles

## Success Criteria

1. `vainfo` shows `VAEntrypointEncSlice` for H.264 Main/High and HEVC Main/Main10
2. `ffmpeg -vaapi_device /dev/dri/renderD128 -f lavfi -i testsrc=duration=5 -vf 'format=nv12,hwupload' -c:v h264_vaapi test.mp4` produces a valid H.264 file
3. Steam Remote Play uses VA-API encode instead of falling back to libx264

## Approach

Start with analysis only. Read the source code, understand the architecture, identify exactly where changes are needed, then propose a detailed implementation plan before writing any code.
