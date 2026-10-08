#include "log.h"
#include "demux.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <libavutil/version.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
/* AV_PROFILE_* replaced FF_PROFILE_* in FFmpeg 7.0 (libavcodec 61).
 * Map the new names onto the old ones for Bookworm and earlier. */
#if LIBAVCODEC_VERSION_MAJOR < 61
#define AV_PROFILE_H264_BASELINE             FF_PROFILE_H264_BASELINE
#define AV_PROFILE_H264_CONSTRAINED_BASELINE FF_PROFILE_H264_CONSTRAINED_BASELINE
#define AV_PROFILE_H264_MAIN                 FF_PROFILE_H264_MAIN
#define AV_PROFILE_H264_EXTENDED             FF_PROFILE_H264_EXTENDED
#define AV_PROFILE_H264_HIGH                 FF_PROFILE_H264_HIGH
#endif
int demux_open(DemuxContext *ctx, const char *filename,
               Queue *video_queue, Queue *audio_queue,
               int64_t hls_max_bandwidth, int separate_audio)
{
    ctx->fmt_ctx             = NULL;
    ctx->video_stream_idx    = -1;
    ctx->audio_stream_idx    = -1;
    ctx->subtitle_stream_idx = -1;
    ctx->duration_us         = 0;
    ctx->video_queue         = video_queue;
    ctx->audio_queue         = audio_queue;
    ctx->subtitle_queue      = NULL;

    /*
     * Pre-allocate the format context so that memory limits are in effect
     * DURING avformat_open_input — not just for find_stream_info.
     * This is critical on Pi Zero 2W (512 MB total, ~150 MB available)
     * where the HLS demuxer downloading multiple variant playlists and
     * init segments during open can spike memory past the OOM threshold.
     *
     * Defaults: probesize=5MB, max_analyze_duration=5s — far too much.
     */
    ctx->fmt_ctx = avformat_alloc_context();
    if (!ctx->fmt_ctx) {
        fprintf(stderr, "demux: failed to alloc format context\n");
        return -1;
    }
    ctx->fmt_ctx->probesize            = 512 * 1024;        /* 512 KB */
    ctx->fmt_ctx->max_analyze_duration = 2 * AV_TIME_BASE;  /* 2 seconds */

    AVDictionary *opts = NULL;
    if (hls_max_bandwidth > 0) {
        char bw_str[32];
        snprintf(bw_str, sizeof(bw_str), "%lld", (long long)hls_max_bandwidth);
        av_dict_set(&opts, "hls_max_bandwidth", bw_str, 0);
    }

    if (avformat_open_input(&ctx->fmt_ctx, filename, NULL, &opts) < 0) {
        av_dict_free(&opts);
        fprintf(stderr, "demux: could not open: %s\n", filename);
        return -1;
    }
    av_dict_free(&opts);

    if (avformat_find_stream_info(ctx->fmt_ctx, NULL) < 0) {
        fprintf(stderr, "demux: could not find stream info\n");
        return -1;
    }

    /* Select streams — prefer H.264 video over HEVC/other codecs that the
     * Pi Zero's V4L2 M2M decoder doesn't support. */
    for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        AVCodecParameters *par = ctx->fmt_ctx->streams[i]->codecpar;

        if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
            ctx->video_stream_idx == -1 &&
            par->codec_id == AV_CODEC_ID_H264)
            ctx->video_stream_idx = (int)i;

        if (par->codec_type == AVMEDIA_TYPE_AUDIO &&
            ctx->audio_stream_idx == -1)
            ctx->audio_stream_idx = (int)i;

        if (par->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            ctx->subtitle_stream_idx == -1)
            ctx->subtitle_stream_idx = (int)i;
    }

    if (separate_audio) {
        if (ctx->audio_stream_idx == -1) {
            fprintf(stderr, "demux: no audio stream found\n");
            return -1;
        }
        if (ctx->video_stream_idx != -1) {
            fprintf(stderr, "demux: duplicate video stream found\n");
            return -1;
        }
        if (ctx->subtitle_stream_idx != -1) {
            fprintf(stderr, "demux: duplicate subtitle stream found\n");
            return -1;
        }
    } else {
        /* Fallback: if no H.264 stream found, take the first video stream
        * so we at least report the codec to the user. */
        if (ctx->video_stream_idx == -1) {
            for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
                AVCodecParameters *par = ctx->fmt_ctx->streams[i]->codecpar;
                if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                    ctx->video_stream_idx = (int)i;
                    fprintf(stderr, "demux: WARNING — no H.264 stream found, "
                            "selected %s (may not decode on V4L2 M2M)\n",
                            avcodec_get_name(par->codec_id));
                    break;
                }
            }
        }

        if (ctx->video_stream_idx == -1) {
            fprintf(stderr, "demux: no video stream found\n");
            return -1;
        }
    }

    /* Check if the codec-level and the codec-profile are supported by the V4L2 M2M decoder*/
    AVCodecParameters *par = ctx->fmt_ctx->streams[ctx->video_stream_idx]->codecpar;

    const AVCodec *codec = avcodec_find_decoder(par->codec_id);

    if(codec)
        vlog("demux: Codec profile — %s\n", av_get_profile_name(codec, par->profile));
    else
        vlog("demux: Profile — %d (no decoder found)\n", par->profile);

    vlog("demux: Codec level   — %d.%d\n", par->level / 10, par->level % 10);

    if (par->level > 42) {
         fprintf(stderr, "demux: WARNING — Codec-level %d.%d may not be supported. "
                "Highest supported level is 4.2\n", par->level / 10, par->level % 10);
    }

    if (! (
        par->profile == AV_PROFILE_H264_BASELINE ||
        par->profile == AV_PROFILE_H264_CONSTRAINED_BASELINE ||
        par->profile == AV_PROFILE_H264_MAIN ||
        par->profile == AV_PROFILE_H264_EXTENDED ||
        par->profile == AV_PROFILE_H264_HIGH
    )) {
        if(codec)
             fprintf(stderr, "demux: WARNING — Codec-profile %s may not be supported. "
                        "Supported profiles: Baseline, Constrained Baseline, Main, Extended, High (8Bit/4:2:0)\n", av_get_profile_name(codec, par->profile));
        else
             fprintf(stderr, "demux: WARNING — Codec-profile %d may not be supported (no decoder found). "
                                     "Supported profiles: Baseline, Constrained Baseline, Main, Extended, High (8Bit/4:2:0)\n", par->profile);
    }

    /* Duration in microseconds */
    if (ctx->fmt_ctx->duration != AV_NOPTS_VALUE)
        ctx->duration_us = ctx->fmt_ctx->duration;   /* AV_TIME_BASE = 1us */

    if (ctx->video_stream_idx >= 0) {
        AVStream *vs = ctx->fmt_ctx->streams[ctx->video_stream_idx];
        vlog("demux: video stream %d — %s %dx%d @ %d/%d fps\n",
                ctx->video_stream_idx,
                avcodec_get_name(vs->codecpar->codec_id),
                vs->codecpar->width, vs->codecpar->height,
                vs->avg_frame_rate.num, vs->avg_frame_rate.den);
    }

    if (ctx->audio_stream_idx >= 0) {
        AVStream *as = ctx->fmt_ctx->streams[ctx->audio_stream_idx];
        vlog("demux: audio stream %d — %d Hz %d ch\n",
                ctx->audio_stream_idx,
                as->codecpar->sample_rate,
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
                as->codecpar->ch_layout.nb_channels
#else
                as->codecpar->channels
#endif
                );
    }

    if (ctx->subtitle_stream_idx >= 0) {
        AVStream *ss = ctx->fmt_ctx->streams[ctx->subtitle_stream_idx];
        fprintf(stderr, "demux: subtitle stream %d — %s\n",
                ctx->subtitle_stream_idx,
                avcodec_get_name(ss->codecpar->codec_id));
    }

    /* Discard streams we don't use — frees their codec parsing state and
     * (for HLS) tells the demuxer it can skip downloading segments for
     * those variants.  Important for memory on Pi Zero. */
    for (unsigned int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        if ((int)i != ctx->video_stream_idx  &&
            (int)i != ctx->audio_stream_idx  &&
            (int)i != ctx->subtitle_stream_idx)
            ctx->fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
    }

    vlog("demux: duration %.1f s\n", ctx->duration_us / 1e6);
    return 0;
}

/* ------------------------------------------------------------------ */

/*
 * A stream's own duration is not always populated. demux_open() caps
 * probesize and max_analyze_duration to keep startup fast on a Pi, and under
 * a truncated analysis avformat_find_stream_info() can leave
 * AVStream::duration at zero or AV_NOPTS_VALUE. The container duration (mvhd
 * for MP4) is read straight from the header and survives that, so fall back
 * to it. Returns <= 0 when neither is usable.
 */
static double stream_duration_sec(AVFormatContext *fmt, int idx)
{
    const AVStream *st = fmt->streams[idx];

    if (st->duration > 0 && st->duration != AV_NOPTS_VALUE)
        return (double)st->duration * st->time_base.num / st->time_base.den;

    if (fmt->duration > 0 && fmt->duration != AV_NOPTS_VALUE)
        return (double)fmt->duration / AV_TIME_BASE;

    return -1;
}

int demux_init_seamless(DemuxContext *ctx)
{
    double video_loop_sec = -1;
    double audio_loop_sec = -1;
    int audio_frame_ticks = -1;

    ctx->video_rebase           = -1;
    ctx->audio_rebase           = -1;
    ctx->audio_rebase_truncated = -1;
    ctx->sub_rebase             = -1;

    AVRational atb;
    AVRational stb;
    AVRational vtb = ctx->fmt_ctx->streams[ctx->video_stream_idx]->time_base;

    /* Needed below for both the audio rebase and the subtitle rebase — compute
     * it unconditionally so a file with subtitles but no audio track doesn't
     * fall through with this left unset (which previously made sub_rebase
     * come out negative, so every subtitle packet looked like it "exceeded
     * video duration" from the very first one and never showed). */
    video_loop_sec = stream_duration_sec(ctx->fmt_ctx, ctx->video_stream_idx);
    if (video_loop_sec <= 0) {
        fprintf(stderr,
            "demux: no usable video duration — cannot loop seamlessly.\n");
        return -1;
    }

    /* Per-loop PTS advance, back in the video stream's own time_base. */
    ctx->video_rebase = (int64_t)(video_loop_sec * vtb.den / vtb.num);

    if(ctx->audio_stream_idx != -1){
        atb = ctx->fmt_ctx->streams[ctx->audio_stream_idx]->time_base;

        AVCodecParameters *acp = ctx->fmt_ctx->streams[ctx->audio_stream_idx]->codecpar;

        audio_frame_ticks = av_get_audio_frame_duration2(acp, 0);

        //fallback
        if (audio_frame_ticks <= 0)
            audio_frame_ticks = acp->frame_size;

        /* Codecs with no fixed frame size (PCM) report 0 for both. Quantising
         * by 0 is a division by zero: SIGFPE on x86, and on ARM it silently
         * yields 0, which would drop every audio packet. Don't quantise. */
        if (audio_frame_ticks <= 0)
            audio_frame_ticks = 1;

        audio_loop_sec = stream_duration_sec(ctx->fmt_ctx, ctx->audio_stream_idx);

        double diff = fabs(video_loop_sec - audio_loop_sec);

        if (diff != 0) {
            if (video_loop_sec > audio_loop_sec) {
                // trimming would have to cut video mid-GOP — unsafe, refuse
                fprintf(stderr,
                    "demux: video (%.3fs) is longer than audio (%.3fs) by %.3fs. "
                    "Zeroplay cannot safely trim video content for seamless looping.\n"
                    "Re-encode/trim the source so audio and video end together. "
                    "Consult readme for more information.\n",
                    video_loop_sec, audio_loop_sec, diff);
                return -1;
            } else {
                // audio longer than video — trimming audio, this is safe
                fprintf(stderr,
                    "demux: audio (%.3fs) is longer than video (%.3fs). "
                    "The extra %.3fs of audio will be trimmed for a seamless loop.\n",
                    audio_loop_sec, video_loop_sec, diff);
            }
        }

        /* audio_rebase stays exact (used for the per-loop PTS advance, so
         * quantising it would drift the two timelines apart a little more on
         * every loop); audio_rebase_truncated is quantised to a whole number
         * of frames and used only as the packet-skip/is_loop_end threshold. */
        ctx->audio_rebase           = (int64_t)(video_loop_sec * atb.den / atb.num);
        ctx->audio_rebase_truncated = (ctx->audio_rebase / audio_frame_ticks) * audio_frame_ticks;
    }

    if(ctx->subtitle_stream_idx != -1){
        stb = ctx->fmt_ctx->streams[ctx->subtitle_stream_idx]->time_base;
        ctx->sub_rebase = (int64_t)(video_loop_sec * stb.den / stb.num);
    }

    /* Every packet is dropped or faded against these, so a silently wrong
     * value costs the whole stream. Log them. */
    vlog("demux: seamless — video %.3fs (rebase %lld), audio %.3fs "
         "(rebase %lld, truncated %lld, %d ticks/frame)\n",
         video_loop_sec, (long long)ctx->video_rebase,
         audio_loop_sec, (long long)ctx->audio_rebase,
         (long long)ctx->audio_rebase_truncated, audio_frame_ticks);

    return 0;
}
/* ------------------------------------------------------------------ */

void demux_run(DemuxContext *ctx)
{
    int64_t loop_pts_base_video = 0;
    int64_t loop_pts_base_audio = 0;
    int64_t loop_pts_base_subs = 0;
    int audio_loop_pending = 0;

    int video_done = 0;
    int audio_done = (ctx->audio_stream_idx == -1); // no audio stream = done

    AVPacket *pkt = av_packet_alloc();

    if (!pkt) {
        fprintf(stderr, "demux: failed to allocate packet\n");
        goto done;
    }

    if(ctx->loop_seamless)
        if(demux_init_seamless(ctx) < 0)
            goto done;

    while (1) {
        int ret = av_read_frame(ctx->fmt_ctx, pkt);

        if (ret == AVERROR_EOF) {

            if(ctx->loop_seamless) {

                if(!(video_done && audio_done)){
                    fprintf(stderr, "demux: EOF was hit before audio and video-duration were hit\n");
                    break;
                }

                video_done = 0;
                audio_done = (ctx->audio_stream_idx == -1);
                audio_loop_pending = 1;

                //an unchecked failure here returns EOF again immediately and
                //spins the loop at 100% CPU
                if (av_seek_frame(ctx->fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                    fprintf(stderr, "demux: seamless loop — seek to start failed, "
                                    "ending playback\n");
                    break;
                }

                loop_pts_base_video += ctx->video_rebase;

                if(ctx->audio_stream_idx != -1)
                  loop_pts_base_audio += ctx->audio_rebase;

                if(ctx->subtitle_stream_idx != -1)
                  loop_pts_base_subs += ctx->sub_rebase;

                continue;
            }

            break;
          } else if (ret == AVERROR(EAGAIN)) {
              av_usleep(1000);
              continue;
          } else if (ret < 0) {
              char errbuf[AV_ERROR_MAX_STRING_SIZE];
              av_strerror(ret, errbuf, sizeof(errbuf));
              fprintf(stderr, "demux: error reading frame: %i (%s)\n", ret, errbuf);
              break;
          }

        if (pkt->stream_index == ctx->video_stream_idx) {
            if (pkt->pts + pkt->duration >= ctx->video_rebase)
                video_done = 1;

            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += loop_pts_base_video;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += loop_pts_base_video;

            AVPacket *queued = av_packet_alloc();
            if (!queued) { av_packet_unref(pkt); continue; }
            av_packet_move_ref(queued, pkt);
            if (!queue_push(ctx->video_queue, queued)) {
                av_packet_free(&queued);
                break;
            }
        } else if (pkt->stream_index == ctx->audio_stream_idx) {
            //seamless loop: skip audio-packets if they exceed video-duration
            //(checked before allocating, so a skipped packet costs nothing)
            if (ctx->loop_seamless && pkt->pts >= ctx->audio_rebase_truncated + pkt->duration) {
                av_packet_unref(pkt);
                continue;
            }

            AudioPkt *audioPkt = malloc(sizeof(AudioPkt));
            if (!audioPkt) { av_packet_unref(pkt); continue; }

            audioPkt->queued = av_packet_alloc();
            if (!audioPkt->queued) { free(audioPkt); av_packet_unref(pkt); continue; }

            audioPkt->is_loop_start = 0;
            audioPkt->is_loop_end = 0;

            if(ctx->loop_seamless && pkt->pts >= ctx->audio_rebase_truncated){
                audioPkt->is_loop_end = 1;
                audio_done = 1;
                //if the audio-stream is trimmed to the exact video-duration then the last frame is most probably not a complete audio-frame
                audioPkt->last_frame_duration = (pkt->pts + pkt->duration) - ctx->audio_rebase;
            }

            if(audio_loop_pending) {
               audioPkt->is_loop_start = 1;
               audio_loop_pending = 0;
            }

            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += loop_pts_base_audio;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += loop_pts_base_audio;

            av_packet_move_ref(audioPkt->queued, pkt);

            if (!queue_push(ctx->audio_queue, audioPkt)) {
                audio_pkt_free(audioPkt);
                break;
            }
        } else if (pkt->stream_index == ctx->subtitle_stream_idx &&
                   ctx->subtitle_queue) {

            //seamless loop: skip subtitle-packets if they exceed video-duration
            if (ctx->loop_seamless && pkt->pts >= ctx->sub_rebase) {
               av_packet_unref(pkt);
               continue;
            }

            //if cue starts before sub_rebase but would exceed it with its duration, cut it
            //(gated on loop_seamless: outside seamless mode sub_rebase is unset
            //and every cue would otherwise be given a negative duration)
            if (ctx->loop_seamless && pkt->duration > 0 &&
                pkt->pts + pkt->duration > ctx->sub_rebase)
                pkt->duration = ctx->sub_rebase - pkt->pts;

            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += loop_pts_base_subs;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += loop_pts_base_subs;

            AVPacket *queued = av_packet_alloc();
            if (!queued) { av_packet_unref(pkt); continue; }
            av_packet_move_ref(queued, pkt);
            if (!queue_push(ctx->subtitle_queue, queued)) {
                av_packet_free(&queued);
                break;
            }
        } else {
            av_packet_unref(pkt);
        }
    }

    av_packet_free(&pkt);
done:
    if (ctx->video_stream_idx >= 0)
        queue_close(ctx->video_queue);
    if (ctx->audio_stream_idx >= 0)
        queue_close(ctx->audio_queue);
    if (ctx->subtitle_stream_idx >= 0 && ctx->subtitle_queue)
        queue_close(ctx->subtitle_queue);
}

/* ------------------------------------------------------------------ */

int demux_seek(DemuxContext *ctx, int64_t target_us, unsigned int backward)
{
    /* Clamp to valid range */
    if (target_us < 0) target_us = 0;
    if (ctx->duration_us > 0 && target_us > ctx->duration_us)
        target_us = ctx->duration_us;

    /* av_seek_frame uses AV_TIME_BASE (microseconds) when stream_index=-1 */
    // AVSEEK_FLAG_BACKWARD causes max_ts to be set to target_us. 
    int ret = av_seek_frame(ctx->fmt_ctx, -1, target_us, (backward > 0) ? AVSEEK_FLAG_BACKWARD : 0);
    if (ret < 0) {
        fprintf(stderr, "demux: seek failed\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

int demux_has_subtitles(DemuxContext *ctx)
{
    return ctx->subtitle_stream_idx >= 0;
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
            vlog("demux: next chapter %u/%u at %.1fs\n",
                    i + 1, n, start / 1e6);
            return 0;
        }
    }

    vlog("demux: already at last chapter\n");
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
        vlog("demux: before first chapter, seeking to start\n");
        return 0;
    }

    *target_us = best;
    vlog("demux: prev chapter %u/%u at %.1fs\n",
            best_idx + 1, n, best / 1e6);
    return 0;
}

/* ------------------------------------------------------------------ */

void demux_close(DemuxContext *ctx)
{
    if (ctx->fmt_ctx)
        avformat_close_input(&ctx->fmt_ctx);
}
