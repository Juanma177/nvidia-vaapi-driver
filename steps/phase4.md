# Phase 4: H.264 Encode Pipeline

## Goal
Full H.264 encoding via VA-API: `ffmpeg -c:v h264_vaapi` produces valid H.264 output.

## Encode pipeline flow

### `nvBeginPicture()` (encode path)
Records the render target surface. No NVDEC decode setup needed.

### `nvRenderPicture()` (encode path)
Routes each buffer to codec-specific handlers via `nvRenderPictureEncode()`:

#### `src/h264_encode.c` — Buffer handlers

1. **`h264enc_handle_sequence_params`** (`VAEncSequenceParameterBufferH264`)
   - Extracts width/height (in MBs), intra_period, ip_period, framerate (from time_scale/num_units_in_tick), bitrate.
   - Stores in `NVENCContext` for use during encoder initialization.

2. **`h264enc_handle_picture_params`** (`VAEncPictureParameterBufferH264`)
   - Captures `coded_buf` ID so `EndPicture` knows where to write output.
   - Picture type decisions delegated to NVENC (`enablePTD=1`).

3. **`h264enc_handle_slice_params`** (`VAEncSliceParameterBufferH264`)
   - NVENC handles slicing internally. No action needed.

4. **`h264enc_handle_misc_params`** (`VAEncMiscParameterBuffer`)
   - `VAEncMiscParameterTypeRateControl`: Updates bitrate, max bitrate (from `bits_per_second * target_percentage / 100`).
   - `VAEncMiscParameterTypeFrameRate`: Updates framerate (packed as `num | (den << 16)`).
   - `VAEncMiscParameterTypeHRD`: Logged but not applied (NVENC handles HRD internally).

### `nvEndPicture()` → `nvEndPictureEncode()`
The core encode operation:

1. **Lazy encoder initialization**: On first frame, calls `nvenc_init_encoder()` with accumulated parameters from sequence/picture/misc buffers.

2. **Surface → Linear buffer**: The backing image uses `CUarray` (2D texture memory), but NVENC needs a linear `CUdeviceptr`.
   - Allocates a linear CUDA buffer with 256-byte aligned pitch.
   - Zeros the buffer (handles height padding for MB alignment, e.g., 1080→1088).
   - Copies luma plane from `CUarray[0]` to linear buffer.
   - Copies chroma plane from `CUarray[1]` to linear buffer + luma offset.

3. **NVENC encode**:
   - `nvEncRegisterResource()` — registers the linear CUDA buffer.
   - `nvEncMapInputResource()` — maps it for NVENC access.
   - `nvEncEncodePicture()` — encodes the frame.
   - `nvEncUnmapInputResource()` / `nvEncUnregisterResource()` — cleanup.
   - `cuMemFree()` — free the linear buffer.

4. **Bitstream retrieval**:
   - `nvEncLockBitstream()` — get encoded data pointer and size.
   - Copy into the application's coded buffer (`NVCodedBuffer`).
   - `nvEncUnlockBitstream()`.

5. **`NV_ENC_ERR_NEED_MORE_INPUT` handling**: When B-frames would cause this (not with our `frameIntervalP=1`), marks the coded buffer as empty and returns `VA_STATUS_SUCCESS`.

### `nvSyncSurface()` (encode path)
Returns immediately — encode is synchronous (blocks in `nvEndPicture`).

## Verification
```bash
ffmpeg -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=5:size=1280x720:rate=30 \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v 2M test.mp4

# Output: 150 frames, H.264 High profile, valid playable file
ffprobe test.mp4
# codec_name=h264, profile=High, 1280x720, 30fps
```

## Per-frame CUDA allocations
Each frame allocates and frees a linear CUDA buffer. This is intentional:
- Registration/mapping/unmap/unregister is the NVENC pattern for external resources.
- A persistent buffer pool would be an optimization for later.
- Current approach has zero leaks — every `cuMemAlloc` has a matching `cuMemFree`.
