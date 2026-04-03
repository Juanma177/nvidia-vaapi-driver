# NVENC Encoding Test Suite

## Prerequisites

- NVIDIA GPU with NVENC support (Turing, Ampere, Ada Lovelace, Blackwell)
- Driver 525+ with `libnvidia-encode.so` installed
- `ffmpeg` with VA-API support (`h264_vaapi`, `hevc_vaapi`)
- For 32-bit tests: `libnvidia-compute:i386`, `libnvidia-encode:i386`, `libva-dev:i386`

## Test 1 — vainfo: Encode entrypoints visible

```bash
vainfo --display drm --device /dev/dri/renderD128
```

**Expected:** `VAEntrypointEncSlice` lines for:
- `VAProfileH264Main`, `VAProfileH264High`, `VAProfileH264ConstrainedBaseline`
- `VAProfileHEVCMain`, `VAProfileHEVCMain10`

All existing `VAEntrypointVLD` (decode) entries must still be present.

## Test 2 — H.264 encode (1080p30)

```bash
ffmpeg -y -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=5:size=1920x1080:rate=30 \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -qp 20 /tmp/test_h264.mp4
ffprobe /tmp/test_h264.mp4
```

**Expected:** Valid MP4, H.264 High profile, 1920x1080, 150 frames.

## Test 3 — HEVC encode (1080p30)

```bash
ffmpeg -y -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=5:size=1920x1080:rate=30 \
  -vf 'format=nv12,hwupload' -c:v hevc_vaapi -qp 20 /tmp/test_hevc.mp4
ffprobe /tmp/test_hevc.mp4
```

**Expected:** Valid MP4, HEVC Main profile, 1920x1080, 150 frames.

## Test 4 — HEVC Main10 (10-bit)

```bash
ffmpeg -y -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=2:size=1920x1080:rate=30 \
  -vf 'format=p010le,hwupload' -c:v hevc_vaapi -profile:v main10 -qp 20 /tmp/test_10bit.mp4
ffprobe -show_entries stream=codec_name,profile,pix_fmt -of csv=p=0 /tmp/test_10bit.mp4
```

**Expected:** `hevc,Main 10,yuv420p10le`

## Test 5 — GPU hardware encode verification

```bash
# Terminal 1: monitor GPU
watch -n 0.5 nvidia-smi --query-gpu=utilization.encoder,encoder.stats.sessionCount --format=csv

# Terminal 2: encode
ffmpeg -y -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=30:size=1920x1080:rate=60 \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -qp 20 /tmp/test_long.mp4
```

**Expected:** `nvidia-smi` shows `utilization.encoder > 0%` and `sessionCount = 1`.

## Test 6 — Stress test (1440p60, 60 seconds)

```bash
ffmpeg -y -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=60:size=2560x1440:rate=60 \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -qp 18 /tmp/test_stress.mp4
```

**Expected:** No crash, no corruption, valid output, all 3600 frames encoded.

## Test 7 — Bitrate control (CBR)

```bash
ffmpeg -y -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i testsrc=duration=5:size=1920x1080:rate=30 \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v 5M /tmp/test_cbr.mp4
ffprobe -show_entries format=bit_rate -of csv=p=0 /tmp/test_cbr.mp4
```

**Expected:** Bitrate approximately 5 Mbps (within ~20%).

## Test 8 — Decode regression

```bash
ffmpeg -y -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i /tmp/test_h264.mp4 -f null -
```

**Expected:** Successful decode using NVDEC, no errors.

## Test 9 — Sequential encodes (leak check)

```bash
for i in $(seq 1 10); do
  ffmpeg -y -vaapi_device /dev/dri/renderD128 \
    -f lavfi -i testsrc=duration=1:size=640x480:rate=30 \
    -vf 'format=nv12,hwupload' -c:v h264_vaapi /tmp/test_seq_$i.mp4 2>&1 \
    | grep -c 'Error'
done
```

**Expected:** All 10 runs output `0` (no errors). No memory growth in the process.

## Test 10 — 32-bit driver init (Steam Remote Play)

Requires 32-bit build: `meson setup build32 --cross-file cross-i386.txt && meson compile -C build32`

```c
// Compile: gcc -m32 test32.c -o test32 -lva -lva-drm -L/usr/lib/i386-linux-gnu
#include <stdio.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
int main(void) {
    int fd = open("/dev/dri/renderD128", O_RDWR);
    VADisplay dpy = vaGetDisplayDRM(fd);
    int major, minor;
    if (vaInitialize(dpy, &major, &minor) != 0) { printf("FAIL\n"); return 1; }
    printf("OK: %s\n", vaQueryVendorString(dpy));
    // Count encode entrypoints
    int np = vaMaxNumProfiles(dpy), ne = vaMaxNumEntrypoints(dpy);
    VAProfile *p = malloc(np * sizeof(VAProfile));
    VAEntrypoint *e = malloc(ne * sizeof(VAEntrypoint));
    vaQueryConfigProfiles(dpy, p, &np);
    int enc = 0;
    for (int i = 0; i < np; i++) {
        int n = 0; vaQueryConfigEntrypoints(dpy, p[i], e, &n);
        for (int j = 0; j < n; j++) if (e[j] == VAEntrypointEncSlice) enc++;
    }
    printf("Encode entrypoints: %d\n", enc);
    free(e); free(p); vaTerminate(dpy); close(fd);
    return enc > 0 ? 0 : 1;
}
```

**Expected:**
```
OK: VA-API NVENC driver [IPC encode-only]
Encode entrypoints: 5
```

No decode entrypoints (CUDA unavailable in 32-bit on Blackwell).

## Test 11 — Steam Remote Play

1. Ensure `nvenc-helper` is running: `systemctl --user status nvenc-helper`
2. Launch Steam (no special env vars needed)
3. Start Remote Play stream from another device
4. Check Steam overlay or `~/.steam/debian-installation/logs/streaming_log.txt`

**Expected:** Encoder shows `VAAPI H264` or `VAAPI HEVC` (not `libx264`).
Streaming performance: `encode < 10ms`, `perte d'images < 1%`.

## Test 12 — nvenc-helper systemd service

```bash
# Service is enabled and running after boot
systemctl --user status nvenc-helper

# Socket exists
ls -la /run/user/$(id -u)/nvenc-helper.sock

# Service restarts after crash
systemctl --user kill nvenc-helper
sleep 3
systemctl --user is-active nvenc-helper
```

**Expected:** Service is `active (running)`, socket exists, service restarts after kill.

---

## Known limitations

### No B-frames
B-frames are disabled (`frameIntervalP=1`). NVENC with B-frames returns
`NV_ENC_ERR_NEED_MORE_INPUT` for non-reference frames, producing empty coded
buffers. ffmpeg's `vaapi_encode` (through version 6.x) asserts on empty coded
buffers, causing a crash.

This is optimal for the primary use case (low-latency game streaming). For
offline transcoding where B-frames improve compression by 10-30%, use ffmpeg's
native NVENC encoders directly:
```bash
# Direct NVENC with B-frames (better compression, higher latency)
ffmpeg -i input.mp4 -c:v h264_nvenc -preset p7 -b:v 5M -bf 2 output.mp4
ffmpeg -i input.mp4 -c:v hevc_nvenc -preset p7 -b:v 5M -bf 2 output.mp4

# VA-API NVENC (no B-frames, low latency, streaming)
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -b:v 5M output.mp4
```

### 32-bit CUDA limitation
On NVIDIA driver 580+ with Blackwell GPUs, 32-bit `cuInit()` returns error 100
("no CUDA-capable device"). The 32-bit driver operates in IPC encode-only mode:
- No hardware decode (requires CUDA)
- Encoding via 64-bit `nvenc-helper` daemon over Unix socket
- Frame data transferred via shared memory (`memfd_create`)

### Packed headers
The driver advertises support for `VA_ENC_PACKED_HEADER_SEQUENCE` and
`VA_ENC_PACKED_HEADER_PICTURE` but does not inject application-provided packed
headers into the bitstream. NVENC generates its own SPS/PPS/VPS headers.
Applications that require custom packed header insertion should use ffmpeg's
native NVENC encoders.

---

## Edge cases and failure modes

### Potential failures documented

| Scenario | Behavior | Mitigation |
|----------|----------|------------|
| `cuInit()` fails in 64-bit | Driver falls back to IPC mode (same as 32-bit) | Helper handles encoding |
| `nvenc-helper` not running | Driver tries to auto-start from `/usr/libexec/nvenc-helper` | Logs error if not found |
| `nvenc-helper` crashes mid-encode | 5s `SO_RCVTIMEO` on socket, then reconnect on next frame | Steam restarts encoder |
| `memfd_create` fails (old kernel) | Falls back to socket-based frame transfer (slower) | Transparent fallback |
| Malicious/corrupt socket data | `frame_size` capped at 64MB, drain with fixed buffer | No malloc bomb |
| Resolution change mid-stream | Steam destroys+recreates context, new SHM allocated | Clean re-init |
| Surface height != encoder height | Copy only surface lines, zero-pad MB-aligned remainder | 1080→1088 padding |
| Client requests IDR after packet loss | `idr_pic_flag` forwarded to NVENC `FORCEIDR` | Recovery in 1 frame |
| No IDR request for 60 frames | Periodic IDR every 60 frames regardless | Recovery in ~1 second |
| `vaDeriveImage` on same surface reused | Returns same `hostPixelData`, sentinel prevents double-free | Safe aliasing |
| Multiple sequential encode sessions | Objects cleaned up per-session, IDs grow monotonically | No leak |
| B-frames requested (`ip_period > 1`) | Forced to `frameIntervalP=1` | ffmpeg 6.x compat |
| NVENC session limit reached (GPU max) | `nvEncOpenEncodeSessionEx` fails, error returned | Clean failure |
| Helper receives 0-byte frame | Encodes empty/black frame | Valid HEVC output |
| `vaExportSurfaceHandle` in IPC mode | CUDA push/pop guards skipped | DRM fds still exported |

### Known non-working scenarios

| Scenario | Status | Reason |
|----------|--------|--------|
| B-frame encoding via VA-API | Crashes ffmpeg 6.x | `vaapi_encode` asserts on empty coded buffer from `NEED_MORE_INPUT` |
| Custom packed header injection | Headers ignored | NVENC generates its own SPS/PPS/VPS |
| Hardware decode in 32-bit IPC mode | Not available | CUDA required for NVDEC, unavailable in IPC mode |
| AV1 encoding | Not implemented | NVENC supports AV1 but no VA-API handler written |
| HEVC 4:4:4 encoding | Not implemented | Could be added with `NV_ENC_HEVC_PROFILE_FREXT_GUID` |
| Multiple concurrent encode streams | Single-client helper | Helper handles one client at a time |
| DMA-BUF zero-copy from Steam | Not used by Steam | Steam uses `vaDeriveImage` host path instead |
