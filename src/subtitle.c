#include "subtitle.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* ASS/SRT text helpers                                                 */
/* ------------------------------------------------------------------ */


/* ------------------------------------------------------------------ */
/* Trim trailing whitespace/newlines from a string in-place            */
/* ------------------------------------------------------------------ */

static void trim_trailing(char *s)
{
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r' || s[len-1] == ' '))
        s[--len] = '\0';
}


/* ------------------------------------------------------------------ */
/* Common: fill a cue directly from raw packet data (subrip/SRT)       */
/* ------------------------------------------------------------------ */

static void fill_cue_from_packet(SubtitleContext *ctx,
                                  const uint8_t *data, int size,
                                  int64_t start_us, int64_t end_us)
{
    if (size <= 0 || ctx->cue_count >= SUBTITLE_MAX_CUES) return;

    SubtitleCue *cue = &ctx->cues[ctx->cue_count];
    cue->start_us = start_us;
    cue->end_us   = end_us;

    int copy = size < (int)sizeof(cue->text) - 1
             ? size : (int)sizeof(cue->text) - 1;
    memcpy(cue->text, data, (size_t)copy);
    cue->text[copy] = '\0';
    trim_trailing(cue->text);

    if (!cue->text[0]) return;

    vlog("subtitle: cue %d [%.2f-%.2f] \"%s\"\n",
         ctx->cue_count,
         cue->start_us / 1e6, cue->end_us / 1e6,
         cue->text);
    ctx->cue_count++;
}

/* ------------------------------------------------------------------ */
/* subtitle_open — embedded stream mode                                 */
/* ------------------------------------------------------------------ */

int subtitle_open(SubtitleContext *ctx, AVStream *stream,
                  Queue *subtitle_queue)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->subtitle_queue = subtitle_queue;
    pthread_mutex_init(&ctx->cue_mutex, NULL);

    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "subtitle: no decoder for codec id %d\n",
                stream->codecpar->codec_id);
        return -1;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) return -1;

    avcodec_parameters_to_context(ctx->codec_ctx, stream->codecpar);

    if (avcodec_open2(ctx->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "subtitle: failed to open %s decoder\n", codec->name);
        avcodec_free_context(&ctx->codec_ctx);
        return -1;
    }

    ctx->time_base = stream->time_base;
    fprintf(stderr, "subtitle: decoder opened — %s\n", codec->name);
    return 0;
}

/* ------------------------------------------------------------------ */
/* subtitle_open_file — external file, eager decode                    */
/* ------------------------------------------------------------------ */

int subtitle_open_file(SubtitleContext *ctx, const char *path, int64_t base_us)
{
    AVFormatContext *fmt = NULL;

    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        fprintf(stderr, "subtitle: cannot open '%s'\n", path);
        return -1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    int sub_idx = -1;
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            sub_idx = (int)i;
            break;
        }
    }
    if (sub_idx < 0) {
        fprintf(stderr, "subtitle: no subtitle stream in '%s'\n", path);
        avformat_close_input(&fmt);
        return -1;
    }

    AVStream *stream = fmt->streams[sub_idx];

    if (subtitle_open(ctx, stream, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }

    AVPacket *pkt = av_packet_alloc();
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != sub_idx || pkt->size <= 0 ||
            pkt->pts == AV_NOPTS_VALUE) {
            av_packet_unref(pkt);
            continue;
        }

        /* pts/duration in milliseconds for standalone SRT files */
        int64_t start_us = pkt->pts * 1000LL + base_us;
        int64_t end_us   = (pkt->duration > 0)
                         ? (pkt->pts + pkt->duration) * 1000LL + base_us
                         : start_us + 5000000LL;

        pthread_mutex_lock(&ctx->cue_mutex);
        fill_cue_from_packet(ctx, pkt->data, pkt->size, start_us, end_us);
        pthread_mutex_unlock(&ctx->cue_mutex);

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    avformat_close_input(&fmt);

    fprintf(stderr, "subtitle: loaded %d cues from '%s'\n",
            ctx->cue_count, path);
    return ctx->cue_count > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* subtitle_run — embedded stream decode thread                        */
/* ------------------------------------------------------------------ */

void subtitle_run(SubtitleContext *ctx)
{
    fprintf(stderr, "subtitle: decode thread started\n");

    while (1) {
        void *item = NULL;

        /* If queue is closed free all items */
        if (ctx->subtitle_queue->closed) {
            while (!queue_pop(ctx->subtitle_queue, &item)) {
                AVPacket *pkt = (AVPacket *)item;
                av_packet_free(&pkt);
            }
            break;
        }

        if (!queue_pop(ctx->subtitle_queue, &item))
            break;  /* queue closed — EOS */

        AVPacket *pkt = (AVPacket *)item;

        /* For subrip/SRT packets the raw UTF-8 text is in pkt->data.
         * avcodec_decode_subtitle2 often returns got_sub=0 for embedded
         * subrip streams, so read directly from the packet instead. */
        if (pkt->size > 0 && pkt->pts != AV_NOPTS_VALUE) {
            int64_t start_us = av_rescale_q(pkt->pts, ctx->time_base,
                                             (AVRational){1, 1000000});
            int64_t end_us   = (pkt->duration > 0)
                             ? av_rescale_q(pkt->pts + pkt->duration,
                                            ctx->time_base,
                                            (AVRational){1, 1000000})
                             : start_us + 5000000LL;

            pthread_mutex_lock(&ctx->cue_mutex);
            fill_cue_from_packet(ctx, pkt->data, pkt->size,
                                  start_us, end_us);
            pthread_mutex_unlock(&ctx->cue_mutex);
        }

        av_packet_free(&pkt);
    }

    fprintf(stderr, "subtitle: thread exiting (%d cues decoded)\n",
            ctx->cue_count);
}

/* ------------------------------------------------------------------ */
/* subtitle_get_active                                                  */
/* ------------------------------------------------------------------ */

const char *subtitle_get_active(SubtitleContext *ctx, int64_t pts_us)
{
    pthread_mutex_lock(&ctx->cue_mutex);
    const char *result = NULL;
    for (int i = 0; i < ctx->cue_count; i++) {
        if (pts_us >= ctx->cues[i].start_us &&
            pts_us <  ctx->cues[i].end_us) {
            result = ctx->cues[i].text;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->cue_mutex);
    return result;
}

/* ------------------------------------------------------------------ */
/* subtitle_flush                                                       */
/* ------------------------------------------------------------------ */

void subtitle_flush(SubtitleContext *ctx)
{
    if (ctx->codec_ctx)
        avcodec_flush_buffers(ctx->codec_ctx);
}

/* ------------------------------------------------------------------ */
/* subtitle_close                                                       */
/* ------------------------------------------------------------------ */

void subtitle_close(SubtitleContext *ctx)
{
    if (ctx->codec_ctx)
        avcodec_free_context(&ctx->codec_ctx);
    pthread_mutex_destroy(&ctx->cue_mutex);
}
