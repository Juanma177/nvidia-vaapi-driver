#ifndef ENCODE_HANDLERS_H
#define ENCODE_HANDLERS_H

#include "nvenc.h"
#include "vabackend.h"

/* H.264 encode buffer handlers */
void h264enc_handle_sequence_params(NVENCContext *nvencCtx, NVBuffer *buffer);
void h264enc_handle_picture_params(NVENCContext *nvencCtx, NVBuffer *buffer);
void h264enc_handle_slice_params(NVENCContext *nvencCtx, NVBuffer *buffer);
void h264enc_handle_misc_params(NVENCContext *nvencCtx, NVBuffer *buffer);

/* HEVC encode buffer handlers */
void hevc_enc_handle_sequence_params(NVENCContext *nvencCtx, NVBuffer *buffer);
void hevc_enc_handle_picture_params(NVENCContext *nvencCtx, NVBuffer *buffer);
void hevc_enc_handle_slice_params(NVENCContext *nvencCtx, NVBuffer *buffer);
void hevc_enc_handle_misc_params(NVENCContext *nvencCtx, NVBuffer *buffer);

#endif /* ENCODE_HANDLERS_H */
