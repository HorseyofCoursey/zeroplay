#include "log.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>

/* Output format we ask ALSA and swresample to produce */
#define ALSA_FORMAT     SND_PCM_FORMAT_S16_LE
#define AV_OUT_FORMAT   AV_SAMPLE_FMT_S16

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

static int get_channels(AVCodecParameters *par)
{
#if HAVE_CH_LAYOUT
    return par->ch_layout.nb_channels;
#else
    return par->channels;
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

int audio_open(AudioContext *ctx, AVStream *stream,
               const char *device, Queue *audio_queue)
{
    int err;
    memset(ctx, 0, sizeof(*ctx));

    ctx->audio_queue    = audio_queue;
    ctx->sample_rate    = stream->codecpar->sample_rate;
    ctx->channels       = get_channels(stream->codecpar);
    ctx->time_base      = stream->time_base;
    ctx->frames_written = 0;
    ctx->paused         = 0;
    ctx->volume         = 1.0f;
    ctx->muted          = 0;

    pthread_mutex_init(&ctx->pause_mutex, NULL);
    pthread_cond_init(&ctx->pause_cond, NULL);

    /* Choose device */
    if (device && device[0])
        strncpy(ctx->device, device, sizeof(ctx->device) - 1);
    else
        strncpy(ctx->device, "plughw:CARD=vc4hdmi,DEV=0",
                sizeof(ctx->device) - 1);

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

    vlog("audio: decoder opened — %s %d Hz %d ch\n",
            codec->name, ctx->sample_rate, ctx->channels);

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
                          ctx->sample_rate, 0);
    av_opt_set_sample_fmt(ctx->swr_ctx, "out_sample_fmt",
                          AV_OUT_FORMAT, 0);

    if (swr_init(ctx->swr_ctx) < 0) {
        fprintf(stderr, "audio: failed to init swresample\n");
        return -1;
    }

    /* ------------------------------------------------------------------ */
    /* 3. Open ALSA PCM device                                             */
    /* ------------------------------------------------------------------ */
    err = snd_pcm_open(&ctx->pcm, ctx->device,
                       SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "audio: cannot open device '%s': %s\n",
                ctx->device, snd_strerror(err));
        return -1;
    }

    snd_pcm_hw_params_t *hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(ctx->pcm, hw_params);

    snd_pcm_hw_params_set_access(ctx->pcm, hw_params,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(ctx->pcm, hw_params, ALSA_FORMAT);
    snd_pcm_hw_params_set_channels(ctx->pcm, hw_params,
                                   (unsigned int)ctx->channels);

    unsigned int rate = (unsigned int)ctx->sample_rate;
    snd_pcm_hw_params_set_rate_near(ctx->pcm, hw_params, &rate, 0);

    snd_pcm_uframes_t buffer_size = (snd_pcm_uframes_t)(ctx->sample_rate / 5);
    snd_pcm_uframes_t period_size = buffer_size / 4;
    snd_pcm_hw_params_set_buffer_size_near(ctx->pcm, hw_params, &buffer_size);
    snd_pcm_hw_params_set_period_size_near(ctx->pcm, hw_params,
                                           &period_size, NULL);

    err = snd_pcm_hw_params(ctx->pcm, hw_params);
    if (err < 0) {
        fprintf(stderr, "audio: cannot set hw params: %s\n",
                snd_strerror(err));
        return -1;
    }

    snd_pcm_sw_params_t *sw_params;
    snd_pcm_sw_params_alloca(&sw_params);
    snd_pcm_sw_params_current(ctx->pcm, sw_params);
    snd_pcm_sw_params_set_start_threshold(ctx->pcm, sw_params, period_size);
    snd_pcm_sw_params(ctx->pcm, sw_params);

    vlog("audio: ALSA opened — device=%s rate=%u\n",
            ctx->device, rate);

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
    AVPacket *pkt   = av_packet_alloc();
    AVFrame  *frame = av_frame_alloc();

    if (!pkt || !frame) {
        fprintf(stderr, "audio: alloc failed\n");
        goto done;
    }

    while (1) {
        /* Block while paused */
        pthread_mutex_lock(&ctx->pause_mutex);
        while (ctx->paused)
            pthread_cond_wait(&ctx->pause_cond, &ctx->pause_mutex);
        pthread_mutex_unlock(&ctx->pause_mutex);

        /* Pull next packet from queue */
        void *item = NULL;
        if (!queue_pop(ctx->audio_queue, &item))
            break;

        pkt = (AVPacket *)item;

        if (avcodec_send_packet(ctx->codec_ctx, pkt) < 0) {
            av_packet_free(&pkt);
            pkt = av_packet_alloc();
            continue;
        }
        av_packet_free(&pkt);
        pkt = av_packet_alloc();

        while (avcodec_receive_frame(ctx->codec_ctx, frame) == 0) {

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

            if (converted > 0) {
                /* Apply software volume gain or mute */
                float gain = ctx->muted ? 0.0f : ctx->volume;
                if (gain != 1.0f) {
                    int16_t *samples = (int16_t *)out_buf;
                    int total = converted * ctx->channels;
                    for (int s = 0; s < total; s++) {
                        int32_t v = (int32_t)(samples[s] * gain);
                        if (v >  32767) v =  32767;
                        if (v < -32768) v = -32768;
                        samples[s] = (int16_t)v;
                    }
                }

                snd_pcm_sframes_t written =
                    snd_pcm_writei(ctx->pcm, out_buf,
                                   (snd_pcm_uframes_t)converted);

                if (written == -EPIPE) {
                    snd_pcm_prepare(ctx->pcm);
                    snd_pcm_writei(ctx->pcm, out_buf,
                                   (snd_pcm_uframes_t)converted);
                    written = converted;
                } else if (written < 0 && !ctx->paused) {
                    fprintf(stderr, "audio: write error: %s\n",
                            snd_strerror((int)written));
                }

                if (written > 0)
                    ctx->frames_written += written;
            }

            av_freep(&out_buf);
            av_frame_unref(frame);
        }
    }

done:
    av_packet_free(&pkt);
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

    return played_frames * 1000000LL / ctx->sample_rate;
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
