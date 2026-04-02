# Phase 5: HEVC Encode Pipeline

## Goal
HEVC encoding via VA-API: `ffmpeg -c:v hevc_vaapi` produces valid HEVC output.

## Changes

### `src/hevc_encode.c` — Buffer handlers
Same pattern as H.264, with HEVC-specific VA-API buffer types:

1. **`hevc_enc_handle_sequence_params`** (`VAEncSequenceParameterBufferHEVC`)
   - Extracts `pic_width_in_luma_samples`, `pic_height_in_luma_samples` (direct pixel dimensions, unlike H.264's MB units).
   - Extracts VUI timing info: `vui_time_scale` / `vui_num_units_in_tick`.
   - Stores intra_period, ip_period, bitrate.

2. **`hevc_enc_handle_picture_params`** (`VAEncPictureParameterBufferHEVC`)
   - Captures `coded_buf` ID.

3. **`hevc_enc_handle_slice_params`** (`VAEncSliceParameterBufferHEVC`)
   - No-op (NVENC handles slicing).

4. **`hevc_enc_handle_misc_params`** — Same as H.264.

### Codec dispatch in `nvRenderPictureEncode()`
Checks whether profile is H.264 or HEVC and routes to the appropriate handlers. The `nvEndPictureEncode()` function is codec-agnostic — it uses the NVENC GUIDs from the profile to configure the correct codec.

### NVENC initialization differences
- Codec GUID: `NV_ENC_CODEC_HEVC_GUID` (vs `NV_ENC_CODEC_H264_GUID`).
- Profile GUID: `NV_ENC_HEVC_PROFILE_MAIN_GUID` or `NV_ENC_HEVC_PROFILE_MAIN10_GUID`.
- 10-bit support: `NV_ENC_BUFFER_FORMAT_YUV420_10BIT` for `VAProfileHEVCMain10`.

## Verification
```bash
ffmpeg -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=1:size=1920x1080:rate=60 \
  -vf 'format=nv12,hwupload' -c:v hevc_vaapi -b:v 5M test_hevc.mp4

# Output: 60 frames, HEVC Main profile, valid playable file
ffprobe test_hevc.mp4
# codec_name=hevc, profile=Main, 1920x1080, 60fps
```

## Supported encode profiles

| VA-API Profile                  | NVENC Codec | NVENC Profile       | Pixel Format |
|---------------------------------|-------------|---------------------|--------------|
| VAProfileH264ConstrainedBaseline| H.264       | Baseline            | NV12         |
| VAProfileH264Main               | H.264       | Main                | NV12         |
| VAProfileH264High               | H.264       | High                | NV12         |
| VAProfileHEVCMain               | HEVC        | Main                | NV12         |
| VAProfileHEVCMain10             | HEVC        | Main10              | P010         |
