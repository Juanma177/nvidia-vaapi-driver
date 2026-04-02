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
                           uint32_t frame_size,
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

    /* Copy NV12/P010 data into NVENC's buffer, respecting pitch */
    uint32_t bpp = enc->is10bit ? 2 : 1;
    uint32_t srcPitch = enc->width * bpp;
    uint32_t dstPitch = lockIn.pitch;
    uint8_t *src = (uint8_t *)frame_data;
    uint8_t *dst = (uint8_t *)lockIn.bufferDataPtr;

    /* Copy luma */
    for (uint32_t y = 0; y < enc->height; y++) {
        memcpy(dst + y * dstPitch, src + y * srcPitch, srcPitch);
    }

    /* Copy chroma (NV12: interleaved UV, half height) */
    uint32_t chromaOffset_src = srcPitch * enc->height;
    uint32_t chromaOffset_dst = dstPitch * enc->height;
    uint32_t chromaHeight = enc->height / 2;

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
    picParams.encodePicFlags = (enc->frameCount == 0) ? NV_ENC_PIC_FLAG_OUTPUT_SPSPPS : 0;
    picParams.frameIdx = (uint32_t)enc->frameCount;
    picParams.inputTimeStamp = enc->frameCount;

    st = enc->funcs.nvEncEncodePicture(enc->encoder, &picParams);
    if (st != NV_ENC_SUCCESS) {
        HELPER_LOG("nvEncEncodePicture failed: %d", st);
        return false;
    }

    enc->frameCount++;

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
            bool ok = encoder_encode(&enc, frame, ep.frame_size, &bitstream, &bsSize);
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

    log_enabled = (getenv("NVD_LOG") != NULL);

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

    /* Accept loop with idle timeout */
    while (running) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 30000); /* 30s idle timeout */

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            HELPER_LOG("Idle timeout, exiting");
            break;
        }

        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            HELPER_LOG("accept: %s", strerror(errno));
            break;
        }

        /* Handle one client at a time (sufficient for Steam's single encode stream) */
        handle_client(client_fd);
    }

    close(listen_fd);
    unlink(sock_path);
    nvenc_free_functions(&nv_dl);
    cuda_free_functions(&cu);
    HELPER_LOG("Exiting");
    return 0;
}
