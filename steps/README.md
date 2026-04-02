# NVENC Encoding Support — Implementation Notes

## Overview
Adds `VAEntrypointEncSlice` support to nvidia-vaapi-driver by wrapping NVIDIA's NVENC API behind the VA-API encoding interface. This enables any VA-API encoding application (Steam Remote Play, GStreamer, ffmpeg) to use NVIDIA hardware encoding on Linux.

## Implementation phases
1. [Phase 1](phase1.md) — NVENC loading & entrypoint registration
2. [Phase 2](phase2.md) — Encode context & session management
3. [Phase 3](phase3.md) — Buffer management & surface input (vaPutImage)
4. [Phase 4](phase4.md) — H.264 encode pipeline
5. [Phase 5](phase5.md) — HEVC encode pipeline

## New files
- `src/nvenc.h` — NVENC context structures and API declarations
- `src/nvenc.c` — Core NVENC infrastructure (session, encoder, buffers, resource management)
- `src/h264_encode.c` — H.264 VA-API encode parameter handlers
- `src/hevc_encode.c` — HEVC VA-API encode parameter handlers
- `src/encode_handlers.h` — Header for encode buffer handlers

## Modified files
- `src/vabackend.h` — Added encode fields to NVDriver, NVConfig, NVContext
- `src/vabackend.c` — NVENC init/cleanup, encode paths in all VA-API callbacks
- `meson.build` — Added new source files

## Key design decisions

### No B-frames (`frameIntervalP=1`)
VA-API's encode model expects every `vaEndPicture` to produce output. NVENC with B-frames returns `NV_ENC_ERR_NEED_MORE_INPUT` for non-reference frames, breaking this assumption. Disabling B-frames ensures synchronous encode and is optimal for the primary use case (low-latency game streaming).

### Per-frame linear buffer allocation
NVENC requires linear `CUdeviceptr` input, but the driver's surfaces use `CUarray` (2D texture memory). Each frame copies from CUarray to a temporary linear buffer, registers it with NVENC, encodes, then frees it. A buffer pool could be added as an optimization.

### Lazy encoder initialization
The NVENC encoder is initialized on the first `vaEndPicture` call rather than in `vaCreateContext`. This is because VA-API sequence/picture parameters (needed to configure NVENC properly) aren't available until `vaRenderPicture` is called.

### Low-latency preset
Uses `NV_ENC_PRESET_P4_GUID` with `NV_ENC_TUNING_INFO_LOW_LATENCY`. This balances quality and speed for the target use case (game streaming). Applications can influence encoding via VA-API rate control parameters.

## Memory safety
- Every `cuMemAlloc` has a matching `cuMemFree` in the same function scope.
- Every `nvEncRegisterResource` has a matching `nvEncUnregisterResource`.
- Every `nvEncMapInputResource` has a matching `nvEncUnmapInputResource`.
- Every `nvEncLockBitstream` has a matching `nvEncUnlockBitstream`.
- Coded buffer bitstream data is freed in `nvDestroyBuffer`.
- NVENC session is destroyed in `destroyContext` / `nvTerminate`.
- NVENC library is unloaded in the destructor.

## Test results
```
# H.264 320x240
ffmpeg ... -c:v h264_vaapi test.mp4     # OK, 60 frames

# H.264 1080p60
ffmpeg ... -c:v h264_vaapi test.mp4     # OK, 60 frames

# HEVC 320x240
ffmpeg ... -c:v hevc_vaapi test.mp4     # OK, 60 frames

# HEVC 1080p60
ffmpeg ... -c:v hevc_vaapi test.mp4     # OK, 60 frames

# H.264 720p, 5 seconds
ffmpeg ... -c:v h264_vaapi test.mp4     # OK, 150 frames
```
