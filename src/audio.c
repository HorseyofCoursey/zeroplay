#include "log.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/version.h>

/* Output format we ask ALSA and swresample to produce */
#define ALSA_FORMAT     SND_PCM_FORMAT_S16_LE
#define AV_OUT_FORMAT   AV_SAMPLE_FMT_S16

/* After this many consecutive write errors, attempt to reopen ALSA */
#define ALSA_REOPEN_THRESHOLD 50

/* Delay (microseconds) before reopening ALSA to let HDMI settle */
#define ALSA_REOPEN_DELAY_US  200000

/*
 * FFmpeg 5.1 (libavutil 57.28) introduced AVChannelLayout and
 * av_opt_set_chlayout. For Bullseye (FFmpeg 4.x) we use the old
 * channels/channel_layout int64 API.
 */
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
#  define HAVE_CH_LAYOUT 1
#else
#  define HAVE_CH_LAYOUT 0
#endif

#define VIDEO_AUDIO_DESYNC_THRESHOLD 0.5
#define VIDEO_AUDIO_DESYNC_EPSILON   0.01

static int get_channels(AVCodecParameters *par)
{
#if HAVE_CH_LAYOUT
    return par->ch_layout.nb_channels;
#else
    return par->channels;
#endif
}

static int get_frame_channels(AVFrame *frame)
{
#if HAVE_CH_LAYOUT
    return frame->ch_layout.nb_channels;
#else
    return frame->channels;
#endif
}

static void set_swr_layout(SwrContext *swr, AVCodecContext *codec_ctx)
{
#if HAVE_CH_LAYOUT
    av_opt_set_chlayout(swr, "in_chlayout",  &codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &codec_ctx->ch_layout, 0);
#else
    int64_t layout = codec_ctx->channel_layout
                   ? codec_ctx->channel_layout
                   : av_get_default_channel_layout(codec_ctx->channels);
    av_opt_set_int(swr, "in_channel_layout",  layout, 0);
    av_opt_set_int(swr, "out_channel_layout", layout, 0);
#endif
}

/* ------------------------------------------------------------------ */
/* Probe the raw ALSA hardware for its native sample rate.             */
/*                                                                     */
/* plughw: silently resamples audio to the hardware's native rate,     */
/* but on the Pi's vc4-hdmi this conversion can introduce pitch        */
/* distortion ("chipmunk" effect).  By discovering the real hardware   */
/* rate we can let libswresample do a correct conversion and feed      */
/* ALSA at the rate it actually runs at — no plughw conversion.        */
/* ------------------------------------------------------------------ */

static int probe_native_rate(const char *dev_name,
                             unsigned int target, int channels)
{
    /* Build the raw hw: name from plughw: (or hw: as-is) */
    char hw_name[64];
    if (strncmp(dev_name, "plughw:", 7) == 0)
        snprintf(hw_name, sizeof(hw_name), "hw:%s", dev_name + 7);
    else if (strncmp(dev_name, "hw:", 3) == 0)
        snprintf(hw_name, sizeof(hw_name), "%s", dev_name);
    else
        return (int)target;   /* unknown device type — keep target */

    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, hw_name, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "audio: probe — cannot open %s, using %u Hz\n",
                hw_name, target);
        return (int)target;
    }

    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(pcm, params);

    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, ALSA_FORMAT);
    snd_pcm_hw_params_set_channels(pcm, params, (unsigned int)channels);

    /* Log supported rate range */
    unsigned int rate_min = 0, rate_max = 0;
    snd_pcm_hw_params_get_rate_min(params, &rate_min, NULL);
    snd_pcm_hw_params_get_rate_max(params, &rate_max, NULL);

    /* Find nearest supported rate to our target */
    unsigned int rate = target;
    int dir = 0;
    snd_pcm_hw_params_set_rate_near(pcm, params, &rate, &dir);

    snd_pcm_close(pcm);

    fprintf(stderr, "audio: hw probe %s — hw rates %u..%u Hz, "
            "target=%u → nearest=%u\n",
            hw_name, rate_min, rate_max, target, rate);

    return (int)rate;
}

/* ------------------------------------------------------------------ */
/* ALSA device open / reopen                                           */
/* ------------------------------------------------------------------ */

static int alsa_setup_device(AudioContext *ctx, const char *dev_name,
                             snd_pcm_format_t fmt)
{
    int err;

    err = snd_pcm_open(&ctx->pcm, dev_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "audio: cannot open device '%s': %s\n",
                dev_name, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(ctx->pcm, hw_params);

    snd_pcm_hw_params_set_access(ctx->pcm, hw_params,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    err = snd_pcm_hw_params_set_format(ctx->pcm, hw_params, fmt);
    if (err < 0) {
        fprintf(stderr, "audio: device '%s' rejected format %s: %s\n",
                dev_name, snd_pcm_format_name(fmt), snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        ctx->pcm = NULL;
        return -1;
    }
    snd_pcm_hw_params_set_channels(ctx->pcm, hw_params,
                                   (unsigned int)ctx->channels);

    unsigned int rate = (unsigned int)ctx->alsa_rate;
    snd_pcm_hw_params_set_rate_near(ctx->pcm, hw_params, &rate, 0);
    ctx->alsa_rate = (int)rate;

    snd_pcm_uframes_t buffer_size = (snd_pcm_uframes_t)(ctx->alsa_rate / 5);
    snd_pcm_uframes_t period_size = buffer_size / 4;
    snd_pcm_hw_params_set_buffer_size_near(ctx->pcm, hw_params, &buffer_size);
    snd_pcm_hw_params_set_period_size_near(ctx->pcm, hw_params,
                                           &period_size, NULL);

    err = snd_pcm_hw_params(ctx->pcm, hw_params);
    if (err < 0) {
        fprintf(stderr, "audio: cannot set hw params on '%s': %s\n",
                dev_name, snd_strerror(err));
        snd_pcm_close(ctx->pcm);
        ctx->pcm = NULL;
        return -1;
    }

    snd_pcm_sw_params_t *sw_params;
    snd_pcm_sw_params_alloca(&sw_params);
    snd_pcm_sw_params_current(ctx->pcm, sw_params);
    snd_pcm_sw_params_set_start_threshold(ctx->pcm, sw_params, period_size);
    snd_pcm_sw_params(ctx->pcm, sw_params);

    /* Log negotiated values */
    snd_pcm_uframes_t actual_buffer = 0, actual_period = 0;
    snd_pcm_hw_params_get_buffer_size(hw_params, &actual_buffer);
    snd_pcm_hw_params_get_period_size(hw_params, &actual_period, NULL);

    snd_pcm_format_t actual_fmt;
    unsigned int actual_ch = 0, actual_rate = 0;
    snd_pcm_hw_params_get_format(hw_params, &actual_fmt);
    snd_pcm_hw_params_get_channels(hw_params, &actual_ch);
    snd_pcm_hw_params_get_rate(hw_params, &actual_rate, NULL);

    fprintf(stderr, "audio: ALSA opened — device=%s rate=%u ch=%u fmt=%s "
            "buffer=%lu period=%lu\n",
            dev_name, actual_rate, actual_ch,
            snd_pcm_format_name(actual_fmt),
            (unsigned long)actual_buffer, (unsigned long)actual_period);

    return 0;
}

static int alsa_open_device(AudioContext *ctx)
{
    return alsa_setup_device(ctx, ctx->device, ALSA_FORMAT);
}

/*
 * Close and reopen the ALSA PCM device.  Used to recover from
 * persistent errors (e.g. HDMI state changes during playback).
 */
static int audio_reopen_alsa(AudioContext *ctx)
{
    fprintf(stderr, "audio: reopening ALSA device '%s'\n", ctx->device);

    if (ctx->pcm) {
        snd_pcm_drop(ctx->pcm);
        snd_pcm_close(ctx->pcm);
        ctx->pcm = NULL;
    }

    /* Small delay to let HDMI/DRM settle */
    usleep(ALSA_REOPEN_DELAY_US);

    int ret = alsa_open_device(ctx);
    if (ret == 0)
        ctx->frames_written = 0;
    else
        fprintf(stderr, "audio: ALSA reopen FAILED\n");

    return ret;
}

/* ------------------------------------------------------------------ */

int audio_open(AudioContext *ctx, AVStream *stream,
               const char *device, Queue *audio_queue)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->audio_queue    = audio_queue;
    ctx->sample_rate    = stream->codecpar->sample_rate;
    ctx->channels       = get_channels(stream->codecpar);
    ctx->time_base      = stream->time_base;
    ctx->frames_written = 0;
    ctx->paused         = 0;
    ctx->volume         = 1.0f;
    ctx->muted          = 0;

    ctx->video_pts      = NULL;
    ctx->audio_pts      = 0;

    pthread_mutex_init(&ctx->pause_mutex, NULL);
    pthread_cond_init(&ctx->pause_cond, NULL);

    /* Choose device.
     *
     * The Pi's vc4-hdmi ALSA driver exposes HDMI audio as
     * IEC958_SUBFRAME_LE.  The 'hdmi:' ALSA device name routes through
     * the iec958 plugin chain defined in vc4-hdmi.conf:
     *
     *   plug → softvol → iec958 → hw
     *
     * This chain converts standard PCM (S16, S32, etc.) into IEC958
     * subframes automatically.  Using 'plughw:' would bypass this chain
     * and cause noise because plughw cannot do S16→IEC958 conversion.
     *
     * mpv uses 'default' (which maps to the same chain) — we use the
     * explicit 'hdmi:' name to avoid dmix and stay closer to hardware. */
    if (device && device[0]) {
        strncpy(ctx->device, device, sizeof(ctx->device) - 1);
    } else {
        /* Try hdmi: first (goes through iec958 plugin), then plughw: */
        static const char *try_devices[] = {
            "hdmi:CARD=vc4hdmi,DEV=0",     /* Pi Zero 2W, Pi 3 */
            "hdmi:CARD=vc4hdmi0,DEV=0",    /* Pi 4 (HDMI port 0) */
            "plughw:CARD=vc4hdmi,DEV=0",   /* fallback */
            "plughw:CARD=vc4hdmi0,DEV=0",  /* fallback */
            NULL
        };
        ctx->device[0] = '\0';
        for (int i = 0; try_devices[i]; i++) {
            snd_pcm_t *test = NULL;
            if (snd_pcm_open(&test, try_devices[i],
                             SND_PCM_STREAM_PLAYBACK, 0) == 0) {
                snd_pcm_close(test);
                strncpy(ctx->device, try_devices[i],
                        sizeof(ctx->device) - 1);
                fprintf(stderr, "audio: selected device '%s'\n",
                        ctx->device);
                break;
            }
        }
        if (!ctx->device[0]) {
            fprintf(stderr, "audio: no HDMI audio device found\n");
            return -1;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 0. Probe hardware native rate so we can bypass plughw resampling    */
    /* ------------------------------------------------------------------ */
    ctx->alsa_rate = probe_native_rate(ctx->device,
                                       (unsigned int)ctx->sample_rate,
                                       ctx->channels);

    /* ------------------------------------------------------------------ */
    /* 1. Initialise libavcodec audio decoder                              */
    /* ------------------------------------------------------------------ */
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "audio: no decoder for audio codec\n");
        return -1;
    }

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        fprintf(stderr, "audio: failed to alloc codec context\n");
        return -1;
    }

    if (avcodec_parameters_to_context(ctx->codec_ctx,
                                      stream->codecpar) < 0) {
        fprintf(stderr, "audio: failed to copy codec parameters\n");
        return -1;
    }

    if (avcodec_open2(ctx->codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "audio: failed to open codec\n");
        return -1;
    }

    fprintf(stderr, "audio: decoder opened — %s profile=%d codecpar_rate=%d "
            "codec_ctx_rate=%d ch=%d (fmt=%s)\n",
            codec->name, ctx->codec_ctx->profile,
            ctx->sample_rate, ctx->codec_ctx->sample_rate,
            ctx->channels,
            av_get_sample_fmt_name(ctx->codec_ctx->sample_fmt));

    /* If the codec context reports a different rate after open (e.g.
     * HE-AAC base rate vs SBR-extended rate), trust the codec context. */
    if (ctx->codec_ctx->sample_rate > 0 &&
        ctx->codec_ctx->sample_rate != ctx->sample_rate) {
        fprintf(stderr, "audio: codec_ctx rate %d != codecpar rate %d, "
                "using codec_ctx rate\n",
                ctx->codec_ctx->sample_rate, ctx->sample_rate);
        ctx->sample_rate = ctx->codec_ctx->sample_rate;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Initialise swresample: float-planar → S16 interleaved            */
    /* ------------------------------------------------------------------ */
    ctx->swr_ctx = swr_alloc();
    if (!ctx->swr_ctx) {
        fprintf(stderr, "audio: failed to alloc swresample\n");
        return -1;
    }

    set_swr_layout(ctx->swr_ctx, ctx->codec_ctx);
    av_opt_set_int       (ctx->swr_ctx, "in_sample_rate",
                          ctx->sample_rate, 0);
    av_opt_set_sample_fmt(ctx->swr_ctx, "in_sample_fmt",
                          ctx->codec_ctx->sample_fmt, 0);
    av_opt_set_int       (ctx->swr_ctx, "out_sample_rate",
                          ctx->alsa_rate, 0);
    av_opt_set_sample_fmt(ctx->swr_ctx, "out_sample_fmt",
                          AV_OUT_FORMAT, 0);

    if (swr_init(ctx->swr_ctx) < 0) {
        fprintf(stderr, "audio: failed to init swresample\n");
        return -1;
    }

    if (ctx->alsa_rate != ctx->sample_rate)
        fprintf(stderr, "audio: resampling %d → %d Hz (hw native rate)\n",
                ctx->sample_rate, ctx->alsa_rate);

    /* ------------------------------------------------------------------ */
    /* 3. Open ALSA PCM device                                             */
    /* ------------------------------------------------------------------ */
    if (alsa_open_device(ctx) < 0)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */

void audio_pause(AudioContext *ctx)
{
    pthread_mutex_lock(&ctx->pause_mutex);
    ctx->paused = 1;
    pthread_mutex_unlock(&ctx->pause_mutex);

    /* Drop buffered samples immediately so sound stops now */
    if (ctx->pcm)
        snd_pcm_drop(ctx->pcm);
}

void audio_resume(AudioContext *ctx)
{
    /* Prepare ALSA to accept new samples after drop */
    if (ctx->pcm)
        snd_pcm_prepare(ctx->pcm);

    pthread_mutex_lock(&ctx->pause_mutex);
    ctx->paused = 0;
    pthread_cond_signal(&ctx->pause_cond);
    pthread_mutex_unlock(&ctx->pause_mutex);
}

/* ------------------------------------------------------------------ */

void audio_run(AudioContext *ctx)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "audio: frame alloc failed\n");
        return;
    }

    int consecutive_errors = 0;
    int total_frames       = 0;
    int total_errors       = 0;
    int rate_checked       = 0;
    int64_t prev_pts       = AV_NOPTS_VALUE;
    int     prev_nb_samples = 0;

    fprintf(stderr, "audio: playback thread started\n");

    while (1) {
        /* Block while paused */
        pthread_mutex_lock(&ctx->pause_mutex);
        while (ctx->paused)
            pthread_cond_wait(&ctx->pause_cond, &ctx->pause_mutex);
        pthread_mutex_unlock(&ctx->pause_mutex);

        void *item = NULL;

        /* If queue is closed free all items */
        if (ctx->audio_queue->closed) {
            while (!queue_pop(ctx->audio_queue, &item)) {
                AVPacket *pkt = (AVPacket *)item;
                av_packet_free(&pkt);
            }
            break;
        }

        /* Pull next packet from queue */
        if (!queue_pop(ctx->audio_queue, &item))
            break;       /* queue closed — EOS */

        /* Decode the packet.  Note: pkt is local — no leak. */
        AVPacket *pkt = (AVPacket *)item;

        /* Block while ahead of video */
        if (ctx->video_pts) {
            int64_t audio_pts = pkt->pts;

            double delta;
            int first_iter = 1;
            do {
                delta = audio_pts * ctx->audio_time_base - *ctx->video_pts * ctx->video_time_base;

                if (first_iter) {
                    first_iter = 0;
                    if (delta < VIDEO_AUDIO_DESYNC_THRESHOLD) break;
                }
            } while (!ctx->audio_queue->closed && (delta > VIDEO_AUDIO_DESYNC_EPSILON));

            ctx->audio_pts = audio_pts;
        }

        if (avcodec_send_packet(ctx->codec_ctx, pkt) < 0) {
            av_packet_free(&pkt);
            continue;
        }
        av_packet_free(&pkt);

        while (avcodec_receive_frame(ctx->codec_ctx, frame) == 0) {
            total_frames++;

            /* Log first decoded frame for diagnostics */
            if (total_frames == 1) {
                fprintf(stderr,
                    "audio: first decoded frame — fmt=%s rate=%d ch=%d "
                    "samples=%d (codec=%d alsa=%d)\n",
                    av_get_sample_fmt_name(frame->format),
                    frame->sample_rate,
                    get_frame_channels(frame),
                    frame->nb_samples,
                    ctx->sample_rate,
                    ctx->alsa_rate);

                /* Warn if decoded format doesn't match configured */
                if (frame->sample_rate != ctx->sample_rate) {
                    fprintf(stderr,
                        "audio: WARNING — frame rate %d != codec rate %d "
                        "(potential HE-AAC SBR mismatch)\n",
                        frame->sample_rate, ctx->sample_rate);
                }
            }

            /* ---------------------------------------------------------- */
            /* PTS-based sample rate detection (runs once, on frame 3)  */
            /*                                                          */
            /* HE-AAC with SBR that isn't being applied will report     */
            /* 48 kHz / 1024 samples but the PTS gap between frames     */
            /* reveals the true rate (e.g. 24 kHz).  If we detect a     */
            /* mismatch we reconfigure SwrContext so it resamples from   */
            /* the real input rate to the ALSA output rate.              */
            /* ---------------------------------------------------------- */
            if (!rate_checked && total_frames >= 3 &&
                prev_pts != AV_NOPTS_VALUE &&
                frame->pts != AV_NOPTS_VALUE &&
                frame->pts > prev_pts && prev_nb_samples > 0) {

                double pts_dur = (frame->pts - prev_pts)
                               * av_q2d(ctx->time_base);
                int detected = (pts_dur > 0.001)
                             ? (int)(prev_nb_samples / pts_dur + 0.5)
                             : 0;

                vlog("audio: PTS rate detect — %d Hz "
                     "(configured %d → %d)\n",
                     detected, ctx->sample_rate, ctx->alsa_rate);

                /* If the real input rate is significantly different,
                 * reconfigure swr so it resamples correctly. */
                if (detected > 0 &&
                    abs(detected - ctx->sample_rate) > 2000) {

                    fprintf(stderr,
                        "audio: RATE MISMATCH — reconfiguring swr "
                        "%d → %d Hz (was %d → %d)\n",
                        detected, ctx->alsa_rate,
                        ctx->sample_rate, ctx->alsa_rate);

                    swr_close(ctx->swr_ctx);
                    set_swr_layout(ctx->swr_ctx, ctx->codec_ctx);
                    av_opt_set_int(ctx->swr_ctx, "in_sample_rate",
                                   detected, 0);
                    av_opt_set_sample_fmt(ctx->swr_ctx, "in_sample_fmt",
                                   ctx->codec_ctx->sample_fmt, 0);
                    av_opt_set_int(ctx->swr_ctx, "out_sample_rate",
                                   ctx->alsa_rate, 0);
                    av_opt_set_sample_fmt(ctx->swr_ctx, "out_sample_fmt",
                                   AV_OUT_FORMAT, 0);
                    swr_init(ctx->swr_ctx);
                    ctx->sample_rate = detected;
                }
                rate_checked = 1;
            }
            prev_pts        = frame->pts;
            prev_nb_samples = frame->nb_samples;

            /* Periodic progress log (every ~21s at 48 kHz) */
            if (total_frames % 1000 == 0)
                vlog("audio: progress — %d frames, %d errors, "
                     "%lld ALSA frames written\n",
                     total_frames, total_errors, ctx->frames_written);

            /* Check pause again between frames */
            pthread_mutex_lock(&ctx->pause_mutex);
            while (ctx->paused)
                pthread_cond_wait(&ctx->pause_cond, &ctx->pause_mutex);
            pthread_mutex_unlock(&ctx->pause_mutex);

            /* Convert to S16 interleaved */
            int out_samples = swr_get_out_samples(ctx->swr_ctx,
                                                  frame->nb_samples);
            uint8_t *out_buf    = NULL;
            int      out_linesize = 0;
            av_samples_alloc(&out_buf, &out_linesize,
                             ctx->channels, out_samples,
                             AV_OUT_FORMAT, 0);

            int converted = swr_convert(ctx->swr_ctx,
                                        &out_buf, out_samples,
                                        (const uint8_t **)frame->data,
                                        frame->nb_samples);

            if (converted > 0 && out_buf && ctx->pcm) {
                /* Apply software volume gain or mute (on S16 data) */
                int16_t *s16_data = (int16_t *)out_buf;
                int total_samps = converted * ctx->channels;
                float gain = ctx->muted ? 0.0f : ctx->volume;
                if (gain != 1.0f) {
                    for (int s = 0; s < total_samps; s++) {
                        int32_t v = (int32_t)(s16_data[s] * gain);
                        if (v >  32767) v =  32767;
                        if (v < -32768) v = -32768;
                        s16_data[s] = (int16_t)v;
                    }
                }

                snd_pcm_sframes_t written =
                    snd_pcm_writei(ctx->pcm, out_buf,
                                   (snd_pcm_uframes_t)converted);

                if (written < 0) {
                    total_errors++;
                    consecutive_errors++;

                    if (consecutive_errors >= ALSA_REOPEN_THRESHOLD) {
                        /* Persistent failure — fully reopen ALSA device */
                        fprintf(stderr,
                            "audio: %d consecutive errors, "
                            "attempting ALSA device reopen\n",
                            consecutive_errors);

                        if (audio_reopen_alsa(ctx) == 0) {
                            consecutive_errors = 0;
                            /* Retry after reopen */
                            written = snd_pcm_writei(ctx->pcm, out_buf,
                                        (snd_pcm_uframes_t)converted);
                        }
                    } else {
                        /* Normal recovery path: recover + retry */
                        int rc = snd_pcm_recover(ctx->pcm, (int)written, 1);
                        if (rc < 0) {
                            snd_pcm_drop(ctx->pcm);
                            snd_pcm_prepare(ctx->pcm);
                        }
                        written = snd_pcm_writei(ctx->pcm, out_buf,
                                    (snd_pcm_uframes_t)converted);
                    }

                    if (written < 0) {
                        if (total_errors <= 5 || total_errors % 200 == 0)
                            fprintf(stderr,
                                "audio: write error #%d (consec=%d): %s\n",
                                total_errors, consecutive_errors,
                                snd_strerror((int)written));
                    } else {
                        consecutive_errors = 0;
                    }
                } else {
                    consecutive_errors = 0;
                }

                if (written > 0)
                    ctx->frames_written += written;
            }

            av_freep(&out_buf);
            av_frame_unref(frame);
        }
    }

    fprintf(stderr, "audio: thread exiting (decoded=%d errors=%d "
            "frames_written=%lld)\n",
            total_frames, total_errors, ctx->frames_written);
    av_frame_free(&frame);
}

/* ------------------------------------------------------------------ */

long long audio_get_clock_us(AudioContext *ctx)
{
    if (!ctx->pcm || ctx->frames_written == 0)
        return 0;

    snd_pcm_sframes_t delay = 0;
    snd_pcm_delay(ctx->pcm, &delay);
    if (delay < 0) delay = 0;

    long long played_frames = ctx->frames_written - delay;
    if (played_frames < 0) played_frames = 0;

    return played_frames * 1000000LL / ctx->alsa_rate;
}

/* ------------------------------------------------------------------ */

float audio_volume_up(AudioContext *ctx)
{
    ctx->volume += 0.1f;
    if (ctx->volume > 2.0f) ctx->volume = 2.0f;
    return ctx->volume;
}

float audio_volume_down(AudioContext *ctx)
{
    ctx->volume -= 0.1f;
    if (ctx->volume < 0.0f) ctx->volume = 0.0f;
    return ctx->volume;
}

int audio_toggle_mute(AudioContext *ctx)
{
    ctx->muted = !ctx->muted;
    return ctx->muted;
}

/* ------------------------------------------------------------------ */

void audio_flush(AudioContext *ctx)
{
    /*
     * Flush the software decoder's internal buffers after a seek.
     * Drops any queued ALSA samples and prepares for new data.
     */
    avcodec_flush_buffers(ctx->codec_ctx);
    swr_init(ctx->swr_ctx);   /* reset resampler state */

    if (ctx->pcm) {
        snd_pcm_drop(ctx->pcm);
        snd_pcm_prepare(ctx->pcm);
    }

    ctx->frames_written = 0;
}

/* ------------------------------------------------------------------ */

void audio_close(AudioContext *ctx)
{
    if (ctx->pcm) {
        snd_pcm_drain(ctx->pcm);
        snd_pcm_close(ctx->pcm);
        ctx->pcm = NULL;
    }
    if (ctx->codec_ctx)
        avcodec_free_context(&ctx->codec_ctx);
    if (ctx->swr_ctx)
        swr_free(&ctx->swr_ctx);

    pthread_mutex_destroy(&ctx->pause_mutex);
    pthread_cond_destroy(&ctx->pause_cond);
}
