# Phase 3: Buffer Management & Surface Input

## Goal
Handle encode buffer types (coded buffers, parameter buffers) and implement surface pixel upload (`vaPutImage`).

## Changes

### `src/nvenc.h` — NVCodedBuffer
New struct wrapping `VACodedBufferSegment` with NVENC bitstream data:
```c
typedef struct {
    VACodedBufferSegment    segment;
    void                   *bitstreamData;   // heap-allocated bitstream storage
    uint32_t                bitstreamSize;
    uint32_t                bitstreamAlloc;
    bool                    hasData;
} NVCodedBuffer;
```

### `src/vabackend.c` — Buffer operations

#### `nvCreateBuffer()`
- `VAEncCodedBufferType`: Allocates `NVCodedBuffer` with pre-allocated bitstream storage (size from application request). The `NVBuffer->ptr` points to the `NVCodedBuffer`.
- All other encode buffer types (`VAEncSequenceParameterBufferType`, etc.) use the standard path — just malloc and memcpy the data.

#### `nvMapBuffer()`
- `VAEncCodedBufferType`: Returns pointer to `VACodedBufferSegment` (the standard VA-API coded buffer format). Sets `segment.buf` to the bitstream data, `segment.size` to the encoded size. If no data yet, returns an empty segment.

#### `nvDestroyBuffer()`
- `VAEncCodedBufferType`: Frees `bitstreamData` before freeing the `NVCodedBuffer` itself. Prevents memory leak.

### `src/vabackend.c` — `nvPutImage()` implementation
Previously a no-op. Now uploads image data from host memory to the surface's GPU-side `CUarray`:
1. Calls `realiseSurface()` to ensure the surface has a backing image with allocated GPU memory.
2. For each plane (Y, UV for NV12):
   - Uses `cuMemcpy2D` from `CU_MEMORYTYPE_HOST` to `CU_MEMORYTYPE_ARRAY`.
   - Respects format info (bppc, channel count, subsampling) from `formatsInfo[]`.

This is essential for encoding: applications use `vaPutImage` (or `hwupload` in ffmpeg) to write NV12 pixel data into VA-API surfaces before encoding.

### `src/vabackend.c` — `nvQuerySurfaceAttributes()`
Added early return for encode configs: returns `VASurfaceAttribPixelFormat` of NV12 (or P010 for 10-bit).

## Memory lifecycle
- `NVCodedBuffer.bitstreamData`: Allocated in `nvCreateBuffer`, may be grown via `realloc` in `nvEndPictureEncode` if encoded output exceeds initial allocation, freed in `nvDestroyBuffer`.
- Linear CUDA buffer for NVENC input: Allocated per-frame in `nvEndPictureEncode`, freed immediately after encode completes. No persistent allocations.
- Backing images: Managed by the existing backend (`direct-export-buf.c`), allocated on first use.
