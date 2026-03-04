#include "demux.h"
#include <stdio.h>
#include <stdlib.h>
#include <libavutil/version.h>

int demux_open(DemuxContext *ctx, const char *filename,
               Queue *video_queue, Queue *audio_queue)
{
    ctx->fmt_ctx          = NULL;
    ctx->video_stream_idx = -1;
    ctx->audio_stream_idx = -1;
    ctx->duration_us      = 0;
    ctx->video_queue      = video_queue;
    ctx->audio_queue      = audio_queue;

    if (avformat_open_input(&ctx->fmt_ctx, filename, NULL, NULL) < 0) {
        fprintf(stderr, "demux: could not open file: %s\n", filename);
        return -1;
    }

    if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0) {
        fprintf(stderr, "demux: could not find stream info\n");
        return -1;
    }

    for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        AVCodecParameters *par = ctx->fmt_ctx->streams[i]->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
            ctx->video_stream_idx == -1)
            ctx->video_stream_idx = (int)i;

        if (par->codec_type == AVMEDIA_TYPE_AUDIO &&
            ctx->audio_stream_idx == -1)
            ctx->audio_stream_idx = (int)i;
    }

    if (ctx->video_stream_idx == -1) {
        fprintf(stderr, "demux: no video stream found\n");
        return -1;
    }

    /* Duration in microseconds */
    if (ctx->fmt_ctx->duration != AV_NOPTS_VALUE)
        ctx->duration_us = ctx->fmt_ctx->duration;   /* AV_TIME_BASE = 1us */

    AVStream *vs = ctx->fmt_ctx->streams[ctx->video_stream_idx];
    fprintf(stderr, "demux: video stream %d — %s %dx%d @ %d/%d fps\n",
        ctx->video_stream_idx,
        avcodec_get_name(vs->codecpar->codec_id),
        vs->codecpar->width, vs->codecpar->height,
        vs->avg_frame_rate.num, vs->avg_frame_rate.den);

    if (ctx->audio_stream_idx >= 0) {
        AVStream *as = ctx->fmt_ctx->streams[ctx->audio_stream_idx];
        fprintf(stderr, "demux: audio stream %d — %d Hz %d ch\n",
                ctx->audio_stream_idx,
                as->codecpar->sample_rate,
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
                as->codecpar->ch_layout.nb_channels
#else
                as->codecpar->channels
#endif
                );
    }

    fprintf(stderr, "demux: duration %.1f s\n", ctx->duration_us / 1e6);
    return 0;
}

/* ------------------------------------------------------------------ */

void demux_run(DemuxContext *ctx)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "demux: failed to allocate packet\n");
        goto done;
    }

    while (av_read_frame(ctx->fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == ctx->video_stream_idx) {
            AVPacket *queued = av_packet_alloc();
            if (!queued) { av_packet_unref(pkt); continue; }
            av_packet_move_ref(queued, pkt);
            if (!queue_push(ctx->video_queue, queued)) {
                av_packet_free(&queued);
                break;
            }
        } else if (pkt->stream_index == ctx->audio_stream_idx) {
            AVPacket *queued = av_packet_alloc();
            if (!queued) { av_packet_unref(pkt); continue; }
            av_packet_move_ref(queued, pkt);
            if (!queue_push(ctx->audio_queue, queued)) {
                av_packet_free(&queued);
                break;
            }
        } else {
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
done:
    queue_close(ctx->video_queue);
    if (ctx->audio_stream_idx >= 0)
        queue_close(ctx->audio_queue);
}

/* ------------------------------------------------------------------ */

int demux_seek(DemuxContext *ctx, int64_t target_us)
{
    /* Clamp to valid range */
    if (target_us < 0) target_us = 0;
    if (ctx->duration_us > 0 && target_us > ctx->duration_us)
        target_us = ctx->duration_us;

    /* av_seek_frame uses AV_TIME_BASE (microseconds) when stream_index=-1 */
    int ret = av_seek_frame(ctx->fmt_ctx, -1, target_us,
                            target_us < 0 ? 0 : AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        fprintf(stderr, "demux: seek failed\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

/*
 * Chapter times are stored in the chapter's own time_base.
 * Convert to microseconds for comparison with current_us.
 */
static int64_t chapter_start_us(AVChapter *ch)
{
    return av_rescale_q(ch->start, ch->time_base, (AVRational){1, 1000000});
}

int demux_next_chapter(DemuxContext *ctx, int64_t current_us,
                       int64_t *target_us)
{
    unsigned int n = ctx->fmt_ctx->nb_chapters;
    if (n == 0) {
        return -1;
    }

    for (unsigned int i = 0; i < n; i++) {
        int64_t start = chapter_start_us(ctx->fmt_ctx->chapters[i]);
        /* Find first chapter that starts strictly after current position */
        if (start > current_us + 1000000LL) {   /* 1s tolerance */
            *target_us = start;
            fprintf(stderr, "demux: next chapter %u/%u at %.1fs\n",
                    i + 1, n, start / 1e6);
            return 0;
        }
    }

    fprintf(stderr, "demux: already at last chapter\n");
    return -1;
}

int demux_prev_chapter(DemuxContext *ctx, int64_t current_us,
                       int64_t *target_us)
{
    unsigned int n = ctx->fmt_ctx->nb_chapters;
    if (n == 0) {
        return -1;
    }

    /* Walk backwards — find the latest chapter that starts before current,
     * with a 3s grace period so pressing 'i' near a chapter boundary
     * goes to the one before rather than the current one. */
    int64_t best = -1;
    unsigned int best_idx = 0;
    for (unsigned int i = 0; i < n; i++) {
        int64_t start = chapter_start_us(ctx->fmt_ctx->chapters[i]);
        if (start < current_us - 3000000LL && start > best) {
            best     = start;
            best_idx = i;
        }
    }

    if (best < 0) {
        /* Already at or before first chapter — go to start */
        *target_us = 0;
        fprintf(stderr, "demux: before first chapter, seeking to start\n");
        return 0;
    }

    *target_us = best;
    fprintf(stderr, "demux: prev chapter %u/%u at %.1fs\n",
            best_idx + 1, n, best / 1e6);
    return 0;
}

/* ------------------------------------------------------------------ */

void demux_close(DemuxContext *ctx)
{
    if (ctx->fmt_ctx)
        avformat_close_input(&ctx->fmt_ctx);
}
