#ifndef NVENC_H
#define NVENC_H

#include <ffnvcodec/nvEncodeAPI.h>
#include <ffnvcodec/dynlink_loader.h>
#include <va/va.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Encode-specific context, stored in NVContext->encodeData when
 * the context is created with VAEntrypointEncSlice.
 */

typedef struct {
    NV_ENC_OUTPUT_PTR       bitstreamBuffer;
    bool                    allocated;
    /* Locked state tracking */
    void                   *lockedPtr;
    uint32_t                lockedSize;
    bool                    locked;
} NVENCOutputBuffer;

typedef struct {
    /* NVENC encoder session handle */
    void                           *encoder;
    /* NVENC API function list */
    NV_ENCODE_API_FUNCTION_LIST     funcs;
    /* Encoder initialized flag */
    bool                            initialized;
    /* Codec GUID (H264 or HEVC) */
    GUID                            codecGuid;
    /* Profile GUID */
    GUID                            profileGuid;
    /* Encode configuration (from preset + overrides) */
    NV_ENC_CONFIG                   encodeConfig;
    NV_ENC_INITIALIZE_PARAMS        initParams;
    /* Frame dimensions */
    uint32_t                        width;
    uint32_t                        height;
    /* Buffer format for input surfaces */
    NV_ENC_BUFFER_FORMAT            inputFormat;
    /* Sequence-level params received from VA-API */
    bool                            seqParamSet;
    /* Rate control mode requested via VA-API */
    uint32_t                        rcMode;
    /* Bitrate in bits/sec */
    uint32_t                        bitrate;
    uint32_t                        maxBitrate;
    /* Framerate */
    uint32_t                        frameRateNum;
    uint32_t                        frameRateDen;
    /* Intra period / GOP */
    uint32_t                        intraPeriod;
    uint32_t                        ipPeriod;
    /* Frame counter */
    uint64_t                        frameCount;
    /* Output bitstream buffer for the current encode */
    NVENCOutputBuffer               outputBuffer;
    /* Current coded buffer ID from VAEncPictureParameterBuffer */
    VABufferID                      currentCodedBufId;
    /* Force IDR on next frame (set by picture params idr_pic_flag) */
    bool                            forceIDR;
    /* Picture type from VA-API slice params (used when enablePTD=0) */
    NV_ENC_PIC_TYPE                 picType;
    /* IPC mode: encode via 64-bit helper when CUDA is unavailable */
    bool                            useIPC;
    int                             ipcFd;   /* socket to nvenc-helper, -1 if not connected */
    /* Shared memory for zero-copy frame transfer */
    void                           *shmPtr;  /* mmap'd shared memory, NULL if not available */
    uint32_t                        shmSize; /* size of shm region */
    int                             shmFd;   /* shm file descriptor, -1 if not available */
} NVENCContext;

/*
 * Coded buffer structure used for VAEncCodedBufferType.
 * This wraps the VA-API coded buffer segment with NVENC bitstream data.
 */
typedef struct {
    VACodedBufferSegment    segment;
    void                   *bitstreamData;
    uint32_t                bitstreamSize;
    uint32_t                bitstreamAlloc;
    bool                    hasData;
} NVCodedBuffer;

/* NVENC helper functions */
bool nvenc_load(NvencFunctions **nvenc_dl);
void nvenc_unload(NvencFunctions **nvenc_dl);

bool nvenc_open_session(NVENCContext *nvencCtx, NvencFunctions *nvenc_dl, CUcontext cudaCtx);
void nvenc_close_session(NVENCContext *nvencCtx);

bool nvenc_init_encoder(NVENCContext *nvencCtx, uint32_t width, uint32_t height,
                        GUID codecGuid, GUID profileGuid, GUID presetGuid,
                        NV_ENC_TUNING_INFO tuningInfo);

bool nvenc_alloc_output_buffer(NVENCContext *nvencCtx);
void nvenc_free_output_buffer(NVENCContext *nvencCtx);

bool nvenc_register_cuda_resource(NVENCContext *nvencCtx, CUdeviceptr devPtr,
                                  uint32_t width, uint32_t height, uint32_t pitch,
                                  NV_ENC_BUFFER_FORMAT format,
                                  NV_ENC_REGISTERED_PTR *outRegistered);
bool nvenc_map_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered,
                        NV_ENC_INPUT_PTR *outMapped, NV_ENC_BUFFER_FORMAT *outFmt);
bool nvenc_unmap_resource(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR mapped);
bool nvenc_unregister_resource(NVENCContext *nvencCtx, NV_ENC_REGISTERED_PTR registered);

/* Returns: 1=output ready, 0=needs more input (B-frames), -1=error */
int nvenc_encode_frame(NVENCContext *nvencCtx, NV_ENC_INPUT_PTR inputBuffer,
                       NV_ENC_BUFFER_FORMAT bufferFmt,
                       uint32_t inputWidth, uint32_t inputHeight, uint32_t inputPitch,
                       NV_ENC_PIC_TYPE picType, uint32_t picFlags);

bool nvenc_lock_bitstream(NVENCContext *nvencCtx, void **outPtr, uint32_t *outSize);
bool nvenc_unlock_bitstream(NVENCContext *nvencCtx);

/* Profile/entrypoint query helpers */
bool nvenc_is_encode_profile(VAProfile profile);
GUID nvenc_va_profile_to_codec_guid(VAProfile profile);
GUID nvenc_va_profile_to_profile_guid(VAProfile profile);
NV_ENC_BUFFER_FORMAT nvenc_surface_format(VAProfile profile);

#endif /* NVENC_H */
