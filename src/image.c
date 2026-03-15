#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
#include "image.h"

/*
 * Decode any image format that libavcodec can handle (JPEG, PNG, BMP …)
 * to XRGB8888, which is what DRM dumb buffers expect for 32bpp output.
 *
 * DRM_FORMAT_XRGB8888 in little-endian memory = B G R X bytes per pixel.
 * ffmpeg's AV_PIX_FMT_BGR0 matches this exactly.
 */
int image_decode_xrgb(const char *path,
                       uint8_t   **pixels_out,
                       int        *width_out,
                       int        *height_out,
                       int        *stride_out)
{
    AVFormatContext  *fmt_ctx   = NULL;
    AVCodecContext   *codec_ctx = NULL;
    AVPacket         *pkt       = NULL;
    AVFrame          *frame     = NULL;
    struct SwsContext *sws      = NULL;
    uint8_t          *pixels    = NULL;
    int ret = -1;

    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) {
        fprintf(stderr, "image: cannot open '%s'\n", path);
        goto out;
    }
    avformat_find_stream_info(fmt_ctx, NULL);

    int stream_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream_idx = (int)i;
            break;
        }
    }
    if (stream_idx < 0) {
        fprintf(stderr, "image: no video stream in '%s'\n", path);
        goto out;
    }

    AVCodecParameters *par   = fmt_ctx->streams[stream_idx]->codecpar;
    const AVCodec     *codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        fprintf(stderr, "image: no decoder for '%s'\n", path);
        goto out;
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) goto out;
    avcodec_parameters_to_context(codec_ctx, par);
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "image: avcodec_open2 failed for '%s'\n", path);
        goto out;
    }

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) goto out;

    /* Read packets until we decode one video frame */
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != stream_idx) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(codec_ctx, pkt) == 0 &&
            avcodec_receive_frame(codec_ctx, frame) == 0)
            break;
        av_packet_unref(pkt);
    }
    av_packet_unref(pkt);

    if (!frame->width || !frame->height) {
        fprintf(stderr, "image: failed to decode a frame from '%s'\n", path);
        goto out;
    }

    int width  = frame->width;
    int height = frame->height;
    int stride = width * 4;

    pixels = malloc((size_t)stride * (size_t)height);
    if (!pixels) goto out;

    av_log_set_level(AV_LOG_ERROR);
    sws = sws_getContext(width, height, codec_ctx->pix_fmt,
                          width, height, AV_PIX_FMT_BGR0,
                          SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        fprintf(stderr, "image: sws_getContext failed\n");
        free(pixels);
        pixels = NULL;
        goto out;
    }

    {
        uint8_t *dst[4]   = { pixels, NULL, NULL, NULL };
        int      dsts[4]  = { stride, 0,    0,    0    };
        sws_scale(sws,
                  (const uint8_t * const *)frame->data,
                  frame->linesize, 0, height, dst, dsts);
    }

    *pixels_out = pixels;
    *width_out  = width;
    *height_out = height;
    *stride_out = stride;
    ret = 0;

out:
    if (sws)       sws_freeContext(sws);
    if (frame)     av_frame_free(&frame);
    if (pkt)       av_packet_free(&pkt);
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (fmt_ctx)   avformat_close_input(&fmt_ctx);
    return ret;
}
