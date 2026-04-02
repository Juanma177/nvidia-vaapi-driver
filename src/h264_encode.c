#include "vabackend.h"
#include "nvenc.h"
#include "encode_handlers.h"

#include <string.h>
#include <va/va.h>

/*
 * H.264 VA-API encode buffer handlers.
 * These are called from nvRenderPicture when the context is an encode context.
 * They accumulate parameters from the application and set them on the NVENCContext.
 */

void h264enc_handle_sequence_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncSequenceParameterBufferH264 *seq =
        (VAEncSequenceParameterBufferH264*) buffer->ptr;

    LOG("H264 encode: seq params %ux%u, intra_period=%u, ip_period=%u",
        seq->picture_width_in_mbs * 16, seq->picture_height_in_mbs * 16,
        seq->intra_period, seq->ip_period);

    /* Store basic sequence-level encode parameters */
    nvencCtx->width = seq->picture_width_in_mbs * 16;
    nvencCtx->height = seq->picture_height_in_mbs * 16;

    if (seq->intra_period > 0) {
        nvencCtx->intraPeriod = seq->intra_period;
    }
    if (seq->ip_period > 0) {
        nvencCtx->ipPeriod = seq->ip_period;
    }

    /* Frame rate from time_scale / num_units_in_tick / 2 if provided */
    if (seq->num_units_in_tick > 0 && seq->time_scale > 0) {
        nvencCtx->frameRateNum = seq->time_scale;
        nvencCtx->frameRateDen = seq->num_units_in_tick * 2;
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

void h264enc_handle_picture_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncPictureParameterBufferH264 *pic =
        (VAEncPictureParameterBufferH264*) buffer->ptr;

    LOG("H264 encode: picture params, coded_buf=%d, pic_fields=0x%x",
        pic->coded_buf, pic->pic_fields.value);

    /* Track the coded buffer so EndPicture knows where to put the output */
    nvencCtx->currentCodedBufId = pic->coded_buf;
}

void h264enc_handle_slice_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    (void)nvencCtx;
    (void)buffer;
    /* VAEncSliceParameterBufferH264 contains per-slice params.
     * NVENC handles slicing internally. */
}

void h264enc_handle_misc_params(NVENCContext *nvencCtx, NVBuffer *buffer)
{
    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer*) buffer->ptr;

    switch (misc->type) {
    case VAEncMiscParameterTypeRateControl: {
        VAEncMiscParameterRateControl *rc =
            (VAEncMiscParameterRateControl*) misc->data;
        LOG("H264 encode: rate control bits_per_second=%u, target_percentage=%u",
            rc->bits_per_second, rc->target_percentage);
        if (rc->bits_per_second > 0) {
            nvencCtx->maxBitrate = rc->bits_per_second;
            if (rc->target_percentage > 0) {
                nvencCtx->bitrate = rc->bits_per_second * rc->target_percentage / 100;
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
            /* framerate can be packed as (num | (den << 16)) or just num */
            uint32_t num = fr->framerate & 0xffff;
            uint32_t den = (fr->framerate >> 16) & 0xffff;
            if (den == 0) den = 1;
            nvencCtx->frameRateNum = num;
            nvencCtx->frameRateDen = den;
            LOG("H264 encode: framerate %u/%u", num, den);
        }
        break;
    }
    case VAEncMiscParameterTypeHRD: {
        VAEncMiscParameterHRD *hrd =
            (VAEncMiscParameterHRD*) misc->data;
        LOG("H264 encode: HRD buffer_size=%u", hrd->buffer_size);
        (void)hrd;
        break;
    }
    default:
        LOG("H264 encode: unhandled misc param type %d", misc->type);
        break;
    }
}
