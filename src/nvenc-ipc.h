#ifndef NVENC_IPC_H
#define NVENC_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * IPC protocol between the 32-bit VA-API driver and the 64-bit NVENC helper.
 *
 * The 32-bit driver cannot use CUDA (cuInit fails on Blackwell GPUs),
 * so it delegates all GPU encoding work to a 64-bit helper process via
 * a Unix domain socket.
 *
 * Socket path: /run/user/<uid>/nvenc-helper.sock
 *
 * All integers are in host byte order (both processes are on the same machine).
 * Messages are: header + payload. Responses are: header + payload.
 */

#define NVENC_IPC_SOCK_NAME "nvenc-helper.sock"

/* Commands */
#define NVENC_IPC_CMD_INIT    1  /* Initialize encoder */
#define NVENC_IPC_CMD_ENCODE  2  /* Encode a frame (host pixel data) */
#define NVENC_IPC_CMD_CLOSE   3  /* Close encoder and disconnect */
#define NVENC_IPC_CMD_ENCODE_DMABUF 4  /* Encode from DMA-BUF fd (GPU zero-copy) */

/* Message header (client → helper) */
typedef struct {
    uint32_t cmd;
    uint32_t payload_size;
} NVEncIPCMsgHeader;

/* Response header (helper → client) */
typedef struct {
    int32_t  status;        /* 0 = success, <0 = error code */
    uint32_t payload_size;  /* size of following data */
} NVEncIPCRespHeader;

/* CMD_INIT payload */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t codec;         /* 0 = H.264, 1 = HEVC */
    uint32_t profile;       /* VA-API profile value */
    uint32_t frameRateNum;
    uint32_t frameRateDen;
    uint32_t bitrate;
    uint32_t maxBitrate;
    uint32_t gopLength;
    uint32_t is10bit;       /* 0 = 8-bit NV12, 1 = 10-bit P010 */
} NVEncIPCInitParams;

/* CMD_ENCODE payload header (followed by frame_size bytes of NV12/P010 data) */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frame_size;    /* total bytes of pixel data */
} NVEncIPCEncodeParams;

/* CMD_ENCODE_DMABUF payload (DMA-BUF fd sent via SCM_RIGHTS ancillary data) */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitches[4];     /* stride per plane */
    uint32_t offsets[4];     /* offset per plane */
    uint32_t num_planes;
    uint32_t data_size;      /* total buffer size */
    uint32_t is10bit;
} NVEncIPCEncodeDmaBufParams;

/* IPC client functions (used by the 32-bit driver) */

/* Get the socket path for this user */
bool nvenc_ipc_get_socket_path(char *buf, size_t bufsize);

/* Try to connect to the helper. Returns socket fd or -1. */
int nvenc_ipc_connect(void);

/* Start the helper if not running, then connect. Returns socket fd or -1. */
int nvenc_ipc_connect_or_start(const char *helper_path);

/* Send init command. Returns 0 on success. */
int nvenc_ipc_init(int fd, const NVEncIPCInitParams *params);

/* Send frame data and receive encoded bitstream.
 * bitstream_out is malloc'd by this function, caller must free.
 * Returns 0 on success. */
int nvenc_ipc_encode(int fd, const void *frame_data,
                     uint32_t width, uint32_t height, uint32_t frame_size,
                     void **bitstream_out, uint32_t *bitstream_size_out);

/* Send DMA-BUF fd and receive encoded bitstream (GPU zero-copy path).
 * The fd is sent via SCM_RIGHTS ancillary data.
 * bitstream_out is malloc'd by this function, caller must free.
 * Returns 0 on success. */
int nvenc_ipc_encode_dmabuf(int fd, int dmabuf_fd,
                            const NVEncIPCEncodeDmaBufParams *params,
                            void **bitstream_out, uint32_t *bitstream_size_out);

/* Send close command and close the socket. */
void nvenc_ipc_close(int fd);

#endif /* NVENC_IPC_H */
