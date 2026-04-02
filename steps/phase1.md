# Phase 1: NVENC Loading & Entrypoint Registration

## Goal
Make `vainfo` show `VAEntrypointEncSlice` for H.264 and HEVC profiles.

## Changes

### `meson.build`
- Added `src/nvenc.c`, `src/h264_encode.c`, `src/hevc_encode.c` to sources list.

### `src/vabackend.h` (NVDriver struct)
- Added `NvencFunctions *nv` — NVENC dynamic loader handle (parallel to `cu`/`cv`).
- Added `bool nvencAvailable` — set to true when NVENC loads successfully.
- Added `bool isEncode` to `NVConfig` — distinguishes encode configs from decode.
- Added `bool isEncode` and `void *encodeData` to `NVContext` — holds `NVENCContext*` for encode contexts.

### `src/vabackend.c` — Library init/cleanup
- `init()`: Calls `nvenc_load(&nv)` after CUDA/NVDEC loading. If NVENC is unavailable, decode still works (graceful fallback).
- `cleanup()`: Calls `nvenc_unload(&nv)`.
- `__vaDriverInit_1_0()`: Sets `drv->nv = nv`, `drv->nvencAvailable = (nv != NULL)`. Sets `max_entrypoints = 2`. Updates vendor string to "NVDEC/NVENC" when available.

### `src/vabackend.c` — Profile/Entrypoint queries
- `nvQueryConfigEntrypoints()`: Returns both `VAEntrypointVLD` and `VAEntrypointEncSlice` for H.264/HEVC profiles when NVENC is available.
- `nvGetConfigAttributes()`: Handles `VAEntrypointEncSlice` with encode-specific attributes (RTFormat, RateControl, PackedHeaders, MaxRefFrames, MaxPictureWidth/Height).
- `nvQueryConfigAttributes()` (by config ID): Early return for encode configs.

### `src/vabackend.c` — Config creation
- `nvCreateConfig()`: For `VAEntrypointEncSlice`, creates an `NVConfig` with `isEncode=true`. Does not need a CUDA codec ID since NVENC uses GUIDs.

### `src/nvenc.c` / `src/nvenc.h` — NVENC infrastructure
- `nvenc_load()`: Loads `libnvidia-encode.so` via ffnvcodec's `nvenc_load_functions()`. Checks API version compatibility using the `(major << 4) | minor` format.
- `nvenc_unload()`: Frees NVENC functions.
- `nvenc_is_encode_profile()`: Returns true for H.264 CB/Main/High and HEVC Main/Main10.
- Profile/GUID mapping functions for converting VA-API profiles to NVENC codec GUIDs.

## Verification
```
$ vainfo
VAProfileH264Main               : VAEntrypointVLD
VAProfileH264Main               : VAEntrypointEncSlice
VAProfileH264High               : VAEntrypointVLD
VAProfileH264High               : VAEntrypointEncSlice
VAProfileH264ConstrainedBaseline : VAEntrypointVLD
VAProfileH264ConstrainedBaseline : VAEntrypointEncSlice
VAProfileHEVCMain               : VAEntrypointVLD
VAProfileHEVCMain               : VAEntrypointEncSlice
VAProfileHEVCMain10             : VAEntrypointVLD
VAProfileHEVCMain10             : VAEntrypointEncSlice
```
