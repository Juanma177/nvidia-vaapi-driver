/*
 * nvenc-helper: 64-bit NVENC encode helper daemon.
 *
 * This standalone process runs as 64-bit, where CUDA works on all GPUs.
 * It receives raw NV12/P010 frames from the 32-bit VA-API driver via
 * a Unix domain socket, encodes them with NVENC, and returns the
 * encoded bitstream.
 *
 * Usage: nvenc-helper [--foreground]
 * The socket is created at $XDG_RUNTIME_DIR/nvenc-helper.sock
 *
 * The helper exits automatically when the last client disconnects
 * and no new client connects within 30 seconds.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <sys/time.h>

#include <ffnvcodec/dynlink_loader.h>
#include <ffnvcodec/nvEncodeAPI.h>
#include "nvenc-ipc.h"

static CudaFunctions *cu;
static NvencFunctions *nv_dl;
static volatile sig_atomic_t running = 1;
static int log_enabled = 0;

/* Macro for CUDA error check in helper */
#define CHECK_CUDA_RESULT_HELPER(err) ({ \
    CUresult _r = (err); \
    if (_r != CUDA_SUCCESS) { \
        const char *_s = NULL; \
        cu->cuGetErrorString(_r, &_s); \
        HELPER_LOG("CUDA error: %s (%d)", _s ? _s : "?", _r); \
    } \
    _r != CUDA_SUCCESS; \
})

#define HELPER_LOG(fmt, ...) do { \
    if (log_enabled) { \
        struct timespec _ts; clock_gettime(CLOCK_MONOTONIC, &_ts); \
        fprintf(stderr, "[nvenc-helper %ld.%03ld] " fmt "\n", \
                (long)_ts.tv_sec, _ts.tv_nsec / 1000000, ##__VA_ARGS__); \
    } \
} while (0)

/* Per-client encoder state */
typedef struct {
    CUcontext                   cudaCtx;
    void                       *encoder;
    NV_ENCODE_API_FUNCTION_LIST funcs;
    bool                        initialized;
    NV_ENC_INPUT_PTR            inputBuffer;
    NV_ENC_OUTPUT_PTR           outputBuffer;
    uint32_t                    width;
    uint32_t                    height;
    uint32_t                    is10bit;
    uint64_t                    frameCount;
} HelperEncoder;

/* Reliable I/O */
static bool send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool recv_all(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool send_response(int fd, int32_t status, const void *data, uint32_t size)
{
    NVEncIPCRespHeader resp = { .status = status, .payload_size = size };
    if (!send_all(fd, &resp, sizeof(resp))) return false;
    if (size > 0 && data != NULL) {
        if (!send_all(fd, data, size)) return false;
    }
    return true;
}

/* Encoder lifecycle */
static bool encoder_init(HelperEncoder *enc, const NVEncIPCInitParams *params)
{
    HELPER_LOG("Init: %ux%u codec=%u profile=%u bitrate=%u",
               params->width, params->height, params->codec, params->profile,
               params->bitrate);

    /* Create CUDA context */
    if (CHECK_CUDA_RESULT_HELPER(cu->cuCtxCreate(&enc->cudaCtx, 0, 0))) {
        return false;
    }

    /* Get NVENC function list */
    enc->funcs.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    NVENCSTATUS st = nv_dl->NvEncodeAPICreateInstance(&enc->funcs);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("NvEncodeAPICreateInstance failed: %d", st);
        cu->cuCtxDestroy(enc->cudaCtx);
        return false;
    }

    /* Open NVENC session */
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessParams = {0};
    sessParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    sessParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    sessParams.device = enc->cudaCtx;
    sessParams.apiVersion = NVENCAPI_VERSION;

    st = enc->funcs.nvEncOpenEncodeSessionEx(&sessParams, &enc->encoder);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncOpenEncodeSessionEx failed: %d", st);
        cu->cuCtxDestroy(enc->cudaCtx);
        return false;
    }

    /* Select codec and profile GUIDs */
    GUID codecGuid = (params->codec == 0) ? NV_ENC_CODEC_H264_GUID : NV_ENC_CODEC_HEVC_GUID;
    GUID profileGuid;
    if (params->codec == 0) {
        /* H.264 */
        profileGuid = NV_ENC_H264_PROFILE_HIGH_GUID;
    } else {
        /* HEVC */
        profileGuid = params->is10bit ? NV_ENC_HEVC_PROFILE_MAIN10_GUID : NV_ENC_HEVC_PROFILE_MAIN_GUID;
    }

    /* Get preset config */
    NV_ENC_PRESET_CONFIG presetConfig = {0};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

    st = enc->funcs.nvEncGetEncodePresetConfigEx(
        enc->encoder, codecGuid, NV_ENC_PRESET_P4_GUID,
        NV_ENC_TUNING_INFO_LOW_LATENCY, &presetConfig);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncGetEncodePresetConfigEx failed: %d", st);
        goto fail;
    }

    NV_ENC_CONFIG encConfig;
    memcpy(&encConfig, &presetConfig.presetCfg, sizeof(encConfig));
    encConfig.version = NV_ENC_CONFIG_VER;
    encConfig.profileGUID = profileGuid;
    encConfig.frameIntervalP = 1; /* No B-frames for synchronous encode */

    if (params->bitrate > 0) {
        encConfig.rcParams.averageBitRate = params->bitrate;
    }
    if (params->maxBitrate > 0) {
        encConfig.rcParams.maxBitRate = params->maxBitrate;
    }
    if (params->gopLength > 0) {
        encConfig.gopLength = params->gopLength;
    }

    /* Initialize encoder */
    NV_ENC_INITIALIZE_PARAMS initParams = {0};
    initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    initParams.encodeGUID = codecGuid;
    initParams.presetGUID = NV_ENC_PRESET_P4_GUID;
    initParams.encodeWidth = params->width;
    initParams.encodeHeight = params->height;
    initParams.darWidth = params->width;
    initParams.darHeight = params->height;
    initParams.frameRateNum = params->frameRateNum > 0 ? params->frameRateNum : 30;
    initParams.frameRateDen = params->frameRateDen > 0 ? params->frameRateDen : 1;
    initParams.enablePTD = 1;
    initParams.encodeConfig = &encConfig;
    initParams.maxEncodeWidth = params->width;
    initParams.maxEncodeHeight = params->height;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;

    st = enc->funcs.nvEncInitializeEncoder(enc->encoder, &initParams);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncInitializeEncoder failed: %d", st);
        goto fail;
    }

    /* Create NVENC-managed input buffer */
    NV_ENC_CREATE_INPUT_BUFFER createIn = {0};
    createIn.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
    createIn.width = params->width;
    createIn.height = params->height;
    createIn.bufferFmt = params->is10bit ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;

    st = enc->funcs.nvEncCreateInputBuffer(enc->encoder, &createIn);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncCreateInputBuffer failed: %d", st);
        goto fail;
    }
    enc->inputBuffer = createIn.inputBuffer;

    /* Create output bitstream buffer */
    NV_ENC_CREATE_BITSTREAM_BUFFER createOut = {0};
    createOut.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

    st = enc->funcs.nvEncCreateBitstreamBuffer(enc->encoder, &createOut);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncCreateBitstreamBuffer failed: %d", st);
        enc->funcs.nvEncDestroyInputBuffer(enc->encoder, enc->inputBuffer);
        goto fail;
    }
    enc->outputBuffer = createOut.bitstreamBuffer;

    enc->width = params->width;
    enc->height = params->height;
    enc->is10bit = params->is10bit;
    enc->frameCount = 0;
    enc->initialized = true;

    HELPER_LOG("Encoder initialized: %ux%u %s %s",
               params->width, params->height,
               params->codec == 0 ? "H.264" : "HEVC",
               params->is10bit ? "10-bit" : "8-bit");
    return true;

fail:
    enc->funcs.nvEncDestroyEncoder(enc->encoder);
    enc->encoder = NULL;
    cu->cuCtxDestroy(enc->cudaCtx);
    enc->cudaCtx = NULL;
    return false;
}

static bool encoder_encode(HelperEncoder *enc, const void *frame_data,
                           uint32_t frame_width, uint32_t frame_height,
                           uint32_t frame_size, bool force_idr,
                           void **out_data, uint32_t *out_size)
{
    NVENCSTATUS st;

    /* Lock input buffer and copy frame data in */
    NV_ENC_LOCK_INPUT_BUFFER lockIn = {0};
    lockIn.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lockIn.inputBuffer = enc->inputBuffer;

    st = enc->funcs.nvEncLockInputBuffer(enc->encoder, &lockIn);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncLockInputBuffer failed: %d", st);
        return false;
    }

    /* Copy NV12/P010 data into NVENC's buffer, respecting pitch.
     * frame_height may be smaller than enc->height (e.g. 1080 vs 1088)
     * because the encoder uses MB-aligned height. Zero-fill padding rows. */
    uint32_t bpp = enc->is10bit ? 2 : 1;
    uint32_t srcPitch = frame_width * bpp;
    uint32_t dstPitch = lockIn.pitch;
    uint8_t *src = (uint8_t *)frame_data;
    uint8_t *dst = (uint8_t *)lockIn.bufferDataPtr;

    /* Zero the entire buffer to handle padding cleanly */
    memset(dst, 0, dstPitch * enc->height * 3 / 2);

    /* Copy luma — only frame_height lines from the source */
    for (uint32_t y = 0; y < frame_height; y++) {
        memcpy(dst + y * dstPitch, src + y * srcPitch, srcPitch);
    }

    /* Copy chroma (NV12: interleaved UV, half height).
     * Source chroma starts at srcPitch * frame_height.
     * Dest chroma starts at dstPitch * enc->height (encoder's full height). */
    uint32_t chromaOffset_src = srcPitch * frame_height;
    uint32_t chromaOffset_dst = dstPitch * enc->height;
    uint32_t chromaHeight = frame_height / 2;

    for (uint32_t y = 0; y < chromaHeight; y++) {
        memcpy(dst + chromaOffset_dst + y * dstPitch,
               src + chromaOffset_src + y * srcPitch,
               srcPitch);
    }

    st = enc->funcs.nvEncUnlockInputBuffer(enc->encoder, enc->inputBuffer);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncUnlockInputBuffer failed: %d", st);
        return false;
    }

    /* Encode */
    NV_ENC_PIC_PARAMS picParams = {0};
    picParams.version = NV_ENC_PIC_PARAMS_VER;
    picParams.inputBuffer = enc->inputBuffer;
    picParams.bufferFmt = enc->is10bit ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;
    picParams.inputWidth = enc->width;
    picParams.inputHeight = enc->height;
    picParams.inputPitch = dstPitch;
    picParams.outputBitstream = enc->outputBuffer;
    picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    picParams.pictureType = NV_ENC_PIC_TYPE_UNKNOWN;
    picParams.encodePicFlags = (enc->frameCount == 0 || force_idr)
        ? (NV_ENC_PIC_FLAG_OUTPUT_SPSPPS | NV_ENC_PIC_FLAG_FORCEIDR)
        : 0;
    picParams.frameIdx = (uint32_t)enc->frameCount;
    picParams.inputTimeStamp = enc->frameCount;

    st = enc->funcs.nvEncEncodePicture(enc->encoder, &picParams);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncEncodePicture failed: %d", st);
        return false;
    }

    enc->frameCount++;

    if (enc->frameCount % 300 == 0) {
        HELPER_LOG("Encoded %lu frames", (unsigned long)enc->frameCount);
    }

    /* Lock output bitstream */
    NV_ENC_LOCK_BITSTREAM lockOut = {0};
    lockOut.version = NV_ENC_LOCK_BITSTREAM_VER;
    lockOut.outputBitstream = enc->outputBuffer;

    st = enc->funcs.nvEncLockBitstream(enc->encoder, &lockOut);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncLockBitstream failed: %d", st);
        return false;
    }

    /* Copy bitstream data */
    *out_size = lockOut.bitstreamSizeInBytes;
    *out_data = malloc(lockOut.bitstreamSizeInBytes);
    if (*out_data == NULL) {
        enc->funcs.nvEncUnlockBitstream(enc->encoder, enc->outputBuffer);
        return false;
    }
    memcpy(*out_data, lockOut.bitstreamBufferPtr, lockOut.bitstreamSizeInBytes);

    enc->funcs.nvEncUnlockBitstream(enc->encoder, enc->outputBuffer);

    return true;
}

static void encoder_close(HelperEncoder *enc)
{
    if (enc->encoder == NULL) return;

    /* Flush */
    if (enc->initialized) {
        NV_ENC_PIC_PARAMS picParams = {0};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        enc->funcs.nvEncEncodePicture(enc->encoder, &picParams);
    }

    if (enc->outputBuffer) {
        enc->funcs.nvEncDestroyBitstreamBuffer(enc->encoder, enc->outputBuffer);
    }
    if (enc->inputBuffer) {
        enc->funcs.nvEncDestroyInputBuffer(enc->encoder, enc->inputBuffer);
    }

    enc->funcs.nvEncDestroyEncoder(enc->encoder);
    enc->encoder = NULL;

    if (enc->cudaCtx) {
        cu->cuCtxDestroy(enc->cudaCtx);
        enc->cudaCtx = NULL;
    }

    enc->initialized = false;
    HELPER_LOG("Encoder closed (encoded %lu frames)", (unsigned long)enc->frameCount);
}

/* Handle one client connection */
static void handle_client(int client_fd)
{
    HelperEncoder enc = {0};

    HELPER_LOG("Client connected (fd=%d)", client_fd);

    while (running) {
        NVEncIPCMsgHeader hdr;
        if (!recv_all(client_fd, &hdr, sizeof(hdr))) {
            HELPER_LOG("Client disconnected");
            break;
        }

        switch (hdr.cmd) {
        case NVENC_IPC_CMD_INIT: {
            if (hdr.payload_size != sizeof(NVEncIPCInitParams)) {
                send_response(client_fd, -1, NULL, 0);
                break;
            }
            NVEncIPCInitParams params;
            if (!recv_all(client_fd, &params, sizeof(params))) goto done;

            if (enc.initialized) {
                encoder_close(&enc);
            }

            cu->cuCtxPushCurrent(NULL); /* Ensure clean CUDA state */
            bool ok = encoder_init(&enc, &params);
            send_response(client_fd, ok ? 0 : -1, NULL, 0);
            break;
        }

        case NVENC_IPC_CMD_ENCODE: {
            if (!enc.initialized) {
                /* Drain the payload */
                if (hdr.payload_size > 0) {
                    void *tmp = malloc(hdr.payload_size);
                    if (tmp) { recv_all(client_fd, tmp, hdr.payload_size); free(tmp); }
                }
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            NVEncIPCEncodeParams ep;
            if (!recv_all(client_fd, &ep, sizeof(ep))) goto done;

            /* Receive frame data */
            void *frame = malloc(ep.frame_size);
            if (frame == NULL) {
                send_response(client_fd, -1, NULL, 0);
                goto done;
            }
            if (!recv_all(client_fd, frame, ep.frame_size)) {
                free(frame);
                goto done;
            }

            cu->cuCtxPushCurrent(enc.cudaCtx);

            void *bitstream = NULL;
            uint32_t bsSize = 0;
            bool ok = encoder_encode(&enc, frame, ep.width, ep.height, ep.frame_size, ep.force_idr, &bitstream, &bsSize);
            free(frame);

            cu->cuCtxPopCurrent(NULL);

            if (ok) {
                send_response(client_fd, 0, bitstream, bsSize);
                free(bitstream);
            } else {
                send_response(client_fd, -1, NULL, 0);
            }
            break;
        }

        case NVENC_IPC_CMD_ENCODE_DMABUF: {
            if (!enc.initialized) {
                if (hdr.payload_size > 0) {
                    void *tmp = malloc(hdr.payload_size);
                    if (tmp) { recv_all(client_fd, tmp, hdr.payload_size); free(tmp); }
                }
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            /* Receive params WITH per-plane DMA-BUF fds via SCM_RIGHTS */
            NVEncIPCEncodeDmaBufParams dp;
            int dmabuf_fds[4] = {-1, -1, -1, -1};
            int num_fds = 0;
            {
                struct iovec iov = { .iov_base = &dp, .iov_len = sizeof(dp) };
                union {
                    char buf[CMSG_SPACE(sizeof(int) * 4)];
                    struct cmsghdr align;
                } cmsg_buf;
                memset(&cmsg_buf, 0, sizeof(cmsg_buf));

                struct msghdr msg = {
                    .msg_iov = &iov,
                    .msg_iovlen = 1,
                    .msg_control = cmsg_buf.buf,
                    .msg_controllen = sizeof(cmsg_buf.buf),
                };

                ssize_t n = recvmsg(client_fd, &msg, 0);
                if (n != sizeof(dp)) {
                    HELPER_LOG("DMABUF: recvmsg failed: %zd (errno=%d)", n, errno);
                    send_response(client_fd, -1, NULL, 0);
                    break;
                }

                struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                if (cmsg && cmsg->cmsg_level == SOL_SOCKET &&
                    cmsg->cmsg_type == SCM_RIGHTS) {
                    num_fds = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                    if (num_fds > 4) num_fds = 4;
                    memcpy(dmabuf_fds, CMSG_DATA(cmsg), (size_t)num_fds * sizeof(int));
                }
            }

            if (num_fds < 1 || dmabuf_fds[0] < 0) {
                HELPER_LOG("DMABUF: no fds received");
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            cu->cuCtxPushCurrent(enc.cudaCtx);

            if (enc.frameCount < 3) {
                HELPER_LOG("DMABUF: fds=[%d,%d] %ux%u planes=%u bppc=%u sizes=[%u,%u]",
                           dmabuf_fds[0], dmabuf_fds[1],
                           dp.width, dp.height, dp.num_planes, dp.bppc,
                           dp.sizes[0], dp.sizes[1]);
            }

            /* Import each plane's DMA-BUF into CUDA as a CUarray,
             * same as the driver's import_to_cuda in direct-export-buf.c */
            CUexternalMemory extMems[4] = {0};
            CUmipmappedArray mipmaps[4] = {0};
            CUarray arrays[4] = {0};
            bool importOk = true;

            for (int i = 0; i < (int)dp.num_planes && i < num_fds; i++) {
                CUDA_EXTERNAL_MEMORY_HANDLE_DESC extMemDesc = {
                    .type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD,
                    .handle.fd = dmabuf_fds[i],
                    .size = dp.sizes[i],
                    .flags = 0,
                };

                CUresult cres = cu->cuImportExternalMemory(&extMems[i], &extMemDesc);
                /* CUDA takes ownership of the fd on success */
                if (cres != CUDA_SUCCESS) {
                    HELPER_LOG("DMABUF: cuImportExternalMemory plane %d failed: %d", i, cres);
                    close(dmabuf_fds[i]);
                    importOk = false;
                    break;
                }

                /* Determine plane format */
                int bpc = 8 * dp.bppc;
                int channels = (i == 0) ? 1 : 2; /* Y=1ch, UV=2ch interleaved */
                uint32_t planeW = (i == 0) ? dp.width : dp.width / 2;
                uint32_t planeH = (i == 0) ? dp.height : dp.height / 2;

                CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapDesc = {
                    .arrayDesc = {
                        .Width = planeW,
                        .Height = planeH,
                        .Depth = 0,
                        .Format = (bpc == 8) ? CU_AD_FORMAT_UNSIGNED_INT8 : CU_AD_FORMAT_UNSIGNED_INT16,
                        .NumChannels = (unsigned int)channels,
                        .Flags = 0,
                    },
                    .numLevels = 1,
                    .offset = 0,
                };

                cres = cu->cuExternalMemoryGetMappedMipmappedArray(&mipmaps[i], extMems[i], &mipmapDesc);
                if (cres != CUDA_SUCCESS) {
                    HELPER_LOG("DMABUF: cuExternalMemoryGetMappedMipmappedArray plane %d failed: %d", i, cres);
                    importOk = false;
                    break;
                }

                cres = cu->cuMipmappedArrayGetLevel(&arrays[i], mipmaps[i], 0);
                if (cres != CUDA_SUCCESS) {
                    HELPER_LOG("DMABUF: cuMipmappedArrayGetLevel plane %d failed: %d", i, cres);
                    importOk = false;
                    break;
                }
            }

            if (!importOk) {
                for (int i = 0; i < 4; i++) {
                    if (mipmaps[i]) cu->cuMipmappedArrayDestroy(mipmaps[i]);
                    if (extMems[i]) cu->cuDestroyExternalMemory(extMems[i]);
                    else if (dmabuf_fds[i] >= 0) close(dmabuf_fds[i]);
                }
                cu->cuCtxPopCurrent(NULL);
                send_response(client_fd, -1, NULL, 0);
                break;
            }

            /* Copy CUarrays to linear buffer (same as nvEndPictureEncode direct path) */
            uint32_t bpp = dp.is10bit ? 2 : 1;
            uint32_t pitch = dp.width * bpp;
            pitch = (pitch + 255) & ~255; /* Align to 256 */
            uint32_t lumaSize = pitch * dp.height;
            uint32_t chromaSize = pitch * (dp.height / 2);
            uint32_t totalSize = lumaSize + chromaSize;

            CUdeviceptr linearBuf = 0;
            cu->cuMemAlloc(&linearBuf, totalSize);
            cu->cuMemsetD8Async(linearBuf, 0, totalSize, 0);

            /* Copy luma */
            CUDA_MEMCPY2D cpy = {0};
            cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
            cpy.srcArray = arrays[0];
            cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
            cpy.dstDevice = linearBuf;
            cpy.dstPitch = pitch;
            cpy.WidthInBytes = dp.width * bpp;
            cpy.Height = dp.height;
            cu->cuMemcpy2D(&cpy);

            /* Copy chroma */
            if (dp.num_planes >= 2 && arrays[1]) {
                memset(&cpy, 0, sizeof(cpy));
                cpy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
                cpy.srcArray = arrays[1];
                cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
                cpy.dstDevice = linearBuf + lumaSize;
                cpy.dstPitch = pitch;
                cpy.WidthInBytes = dp.width * bpp;
                cpy.Height = dp.height / 2;
                cu->cuMemcpy2D(&cpy);
            }

            /* Register linear buffer with NVENC */
            NV_ENC_BUFFER_FORMAT bufFmt = dp.is10bit
                ? NV_ENC_BUFFER_FORMAT_YUV420_10BIT : NV_ENC_BUFFER_FORMAT_NV12;

            NV_ENC_REGISTER_RESOURCE regRes = {0};
            regRes.version = NV_ENC_REGISTER_RESOURCE_VER;
            regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
            regRes.resourceToRegister = (void *)linearBuf;
            regRes.width = dp.width;
            regRes.height = dp.height;
            regRes.pitch = pitch;
            regRes.bufferFormat = bufFmt;
            regRes.bufferUsage = NV_ENC_INPUT_IMAGE;

            NVENCSTATUS nvst = enc.funcs.nvEncRegisterResource(enc.encoder, &regRes);
            if (nvst != NV_ENC_SUCCESS) {
                HELPER_LOG("DMABUF: nvEncRegisterResource failed: %d", nvst);
                cu->cuMemFree(linearBuf);
                goto dmabuf_cleanup;
            }

            NV_ENC_MAP_INPUT_RESOURCE mapRes = {0};
            mapRes.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
            mapRes.registeredResource = regRes.registeredResource;
            nvst = enc.funcs.nvEncMapInputResource(enc.encoder, &mapRes);
            if (nvst != NV_ENC_SUCCESS) {
                enc.funcs.nvEncUnregisterResource(enc.encoder, regRes.registeredResource);
                cu->cuMemFree(linearBuf);
                goto dmabuf_cleanup;
            }

            /* Encode */
            NV_ENC_PIC_PARAMS picParams = {0};
            picParams.version = NV_ENC_PIC_PARAMS_VER;
            picParams.inputBuffer = mapRes.mappedResource;
            picParams.bufferFmt = mapRes.mappedBufferFmt;
            picParams.inputWidth = dp.width;
            picParams.inputHeight = dp.height;
            picParams.inputPitch = pitch;
            picParams.outputBitstream = enc.outputBuffer;
            picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
            picParams.pictureType = NV_ENC_PIC_TYPE_UNKNOWN;
            picParams.encodePicFlags = (enc.frameCount == 0)
                ? (NV_ENC_PIC_FLAG_OUTPUT_SPSPPS | NV_ENC_PIC_FLAG_FORCEIDR) : 0;
            picParams.frameIdx = (uint32_t)enc.frameCount;
            picParams.inputTimeStamp = enc.frameCount;

            nvst = enc.funcs.nvEncEncodePicture(enc.encoder, &picParams);

            enc.funcs.nvEncUnmapInputResource(enc.encoder, mapRes.mappedResource);
            enc.funcs.nvEncUnregisterResource(enc.encoder, regRes.registeredResource);
            cu->cuMemFree(linearBuf);

            if (nvst != NV_ENC_SUCCESS) {
                HELPER_LOG("DMABUF: nvEncEncodePicture failed: %d", nvst);
                goto dmabuf_cleanup;
            }

            enc.frameCount++;
            if (enc.frameCount % 300 == 0) {
                HELPER_LOG("Encoded %lu frames (DMABUF)", (unsigned long)enc.frameCount);
            }

            /* Lock and send bitstream */
            {
                NV_ENC_LOCK_BITSTREAM lockOut = {0};
                lockOut.version = NV_ENC_LOCK_BITSTREAM_VER;
                lockOut.outputBitstream = enc.outputBuffer;
                nvst = enc.funcs.nvEncLockBitstream(enc.encoder, &lockOut);
                if (nvst == NV_ENC_SUCCESS) {
                    send_response(client_fd, 0, lockOut.bitstreamBufferPtr,
                                  lockOut.bitstreamSizeInBytes);
                    enc.funcs.nvEncUnlockBitstream(enc.encoder, enc.outputBuffer);
                } else {
                    send_response(client_fd, -1, NULL, 0);
                }
            }

dmabuf_cleanup:
            for (int i = 0; i < 4; i++) {
                if (mipmaps[i]) cu->cuMipmappedArrayDestroy(mipmaps[i]);
                if (extMems[i]) cu->cuDestroyExternalMemory(extMems[i]);
            }
            cu->cuCtxPopCurrent(NULL);
            break;
        }

        case NVENC_IPC_CMD_CLOSE:
            encoder_close(&enc);
            send_response(client_fd, 0, NULL, 0);
            goto done;

        default:
            HELPER_LOG("Unknown command: %u", hdr.cmd);
            send_response(client_fd, -1, NULL, 0);
            break;
        }
    }

done:
    if (enc.initialized) {
        cu->cuCtxPushCurrent(enc.cudaCtx);
        encoder_close(&enc);
        cu->cuCtxPopCurrent(NULL);
    }
    close(client_fd);
    HELPER_LOG("Client handler done");
}

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Always log to stderr — this is a daemon, logs are essential for diagnostics */
    log_enabled = 1;

    signal(SIGTERM, sighandler);
    signal(SIGINT, sighandler);
    signal(SIGPIPE, SIG_IGN);

    HELPER_LOG("Starting nvenc-helper (pid=%d)", getpid());

    /* Load CUDA */
    if (cuda_load_functions(&cu, NULL) != 0 || cu == NULL) {
        HELPER_LOG("Failed to load CUDA");
        return 1;
    }

    CUresult cres = cu->cuInit(0);
    if (cres != CUDA_SUCCESS) {
        HELPER_LOG("cuInit failed: %d", cres);
        cuda_free_functions(&cu);
        return 1;
    }

    /* Load NVENC */
    if (nvenc_load_functions(&nv_dl, NULL) != 0 || nv_dl == NULL) {
        HELPER_LOG("Failed to load NVENC");
        cuda_free_functions(&cu);
        return 1;
    }

    HELPER_LOG("CUDA and NVENC loaded");

    /* Create socket */
    char sock_path[256];
    if (!nvenc_ipc_get_socket_path(sock_path, sizeof(sock_path))) {
        HELPER_LOG("Failed to get socket path");
        return 1;
    }

    unlink(sock_path); /* Remove stale socket */

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        HELPER_LOG("socket: %s", strerror(errno));
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        HELPER_LOG("bind(%s): %s", sock_path, strerror(errno));
        close(listen_fd);
        return 1;
    }

    /* Restrict socket permissions to current user */
    chmod(sock_path, 0700);

    if (listen(listen_fd, 2) < 0) {
        HELPER_LOG("listen: %s", strerror(errno));
        close(listen_fd);
        unlink(sock_path);
        return 1;
    }

    HELPER_LOG("Listening on %s", sock_path);

    /* Accept loop — runs until SIGTERM/SIGINT */
    while (running) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, -1); /* Block forever until connection or signal */

        if (ret < 0) {
            if (errno == EINTR) continue;
            HELPER_LOG("poll: %s", strerror(errno));
            break;
        }

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            HELPER_LOG("accept: %s", strerror(errno));
            continue; /* Don't exit on accept error — keep listening */
        }

        /* Set recv timeout so we detect dead clients instead of blocking forever.
         * A streaming encode at 60fps sends a frame every ~16ms.
         * 5 seconds of silence means the client is gone. */
        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        /* Handle one client at a time (sufficient for Steam's single encode stream) */
        handle_client(client_fd);
        HELPER_LOG("Ready for next client");
    }

    close(listen_fd);
    unlink(sock_path);
    nvenc_free_functions(&nv_dl);
    cuda_free_functions(&cu);
    HELPER_LOG("Exiting");
    return 0;
}
