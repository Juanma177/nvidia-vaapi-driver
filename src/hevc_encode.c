#include "vabackend.h"
#include "nvenc.h"
#include "encode_handlers.h"

#include <string.h>
#include <va/va.h>

/*
 * HEVC VA-API encode buffer handlers.
 */

void hevcenc_handle_sequence_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncSequenceParameterBufferHEVC *seq =
        (VAEncSequenceParameterBufferHEVC*) buffer->ptr;

    LOG("HEVC encode: seq params %ux%u, intra_period=%u, ip_period=%u",
        seq->pic_width_in_luma_samples, seq->pic_height_in_luma_samples,
        seq->intra_period, seq->ip_period);

    nvencCtx->width = seq->pic_width_in_luma_samples;
    nvencCtx->height = seq->pic_height_in_luma_samples;

    if (seq->intra_period > 0) {
        nvencCtx->intraPeriod = seq->intra_period;
    }
    if (seq->ip_period > 0) {
        nvencCtx->ipPeriod = seq->ip_period;
    }

    /* VUI timing info */
    if (seq->vui_num_units_in_tick > 0 && seq->vui_time_scale > 0) {
        nvencCtx->frameRateNum = seq->vui_time_scale;
        nvencCtx->frameRateDen = seq->vui_num_units_in_tick * 2;
    }

    /* Bitrate (VA-API provides in bits/sec) */
    if (seq->bits_per_second > 0) {
        nvencCtx->bitrate = seq->bits_per_second;
        if (nvencCtx->maxBitrate == 0) {
            nvencCtx->maxBitrate = seq->bits_per_second;
        }
    }

    nvencCtx->seqParamSet = true;
}

void hevcenc_handle_picture_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncPictureParameterBufferHEVC *pic =
        (VAEncPictureParameterBufferHEVC*) buffer->ptr;

    LOG("HEVC encode: picture params, coded_buf=%d", pic->coded_buf);
    nvencCtx->currentCodedBufId = pic->coded_buf;
}

void hevcenc_handle_slice_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    (void)nvencCtx;
    (void)buffer;
}

void hevcenc_handle_misc_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer*) buffer->ptr;

    switch (misc->type) {
    case VAEncMiscParameterTypeRateControl: {
        VAEncMiscParameterRateControl *rc =
            (VAEncMiscParameterRateControl*) misc->data;
        LOG("HEVC encode: rate control bits_per_second=%u", rc->bits_per_second);
        if (rc->bits_per_second > 0) {
            nvencCtx->maxBitrate = rc->bits_per_second;
            if (rc->target_percentage > 0) {
                nvencCtx->bitrate = (uint32_t)((uint64_t)rc->bits_per_second * rc->target_percentage / 100);
            } else {
                nvencCtx->bitrate = rc->bits_per_second;
            }
        }
        break;
    }
    case VAEncMiscParameterTypeFrameRate: {
        VAEncMiscParameterFrameRate *fr =
            (VAEncMiscParameterFrameRate*) misc->data;
        if (fr->framerate > 0) {
            uint32_t num = fr->framerate & 0xffff;
            uint32_t den = (fr->framerate >> 16) & 0xffff;
            if (den == 0) den = 1;
            nvencCtx->frameRateNum = num;
            nvencCtx->frameRateDen = den;
        }
        break;
    }
    default:
        LOG("HEVC encode: unhandled misc param type %d", misc->type);
        break;
    }
}
