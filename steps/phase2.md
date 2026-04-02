# Phase 2: Encode Context & Session Management

## Goal
NVENC sessions open and close cleanly when applications create/destroy encode contexts.

## Changes

### `src/nvenc.c` — Session lifecycle
- `nvenc_open_session()`: Creates `NV_ENCODE_API_FUNCTION_LIST`, fills it via `NvEncodeAPICreateInstance()`, then opens a session with `nvEncOpenEncodeSessionEx()` using the CUDA context.
- `nvenc_close_session()`: Frees output buffer, sends EOS to flush the encoder, then calls `nvEncDestroyEncoder()`.
- `nvenc_init_encoder()`: Called lazily on first frame. Gets preset config via `nvEncGetEncodePresetConfigEx()`, applies rate control/GOP overrides from VA-API parameters, then calls `nvEncInitializeEncoder()`.
  - Uses P4 preset with LOW_LATENCY tuning (optimal for streaming).
  - Forces `frameIntervalP=1` (no B-frames) to ensure synchronous encode — every `EndPicture` produces output.

### `src/vabackend.c` — Context creation/destruction
- `nvCreateContext()`: When `cfg->isEncode`, allocates `NVENCContext`, opens NVENC session, stores it in `nvCtx->encodeData`. Does **not** create an NVDEC decoder or resolve thread.
- `destroyContext()`: When `nvCtx->isEncode`, calls `nvenc_close_session()` and frees the `NVENCContext`.

### Memory management
- `NVENCContext` is heap-allocated in `nvCreateContext()` and freed in `destroyContext()`.
- NVENC session is opened in `nvCreateContext()` and destroyed in `destroyContext()`.
- Output bitstream buffer is allocated lazily on first encode and freed during session close.
- `deleteAllObjects()` in `nvTerminate()` handles encode contexts via `destroyContext()`.

## Verification
Creating and destroying encode contexts produces clean NVENC session logs with no leaks.
