#ifndef AUDIO_H
#define AUDIO_H

#include <alsa/asoundlib.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <pthread.h>
#include "queue.h"

typedef struct {
    /* ALSA */
    snd_pcm_t       *pcm;
    char             device[64];

    /* libavcodec audio decoder */
    AVCodecContext  *codec_ctx;
    SwrContext      *swr_ctx;

    /* Stream info */
    int              sample_rate;    /* codec sample rate (input to swr) */
    int              alsa_rate;     /* actual ALSA hardware rate (output of swr) */
    int              channels;
    AVRational       time_base;

    /* Queue */
    Queue           *audio_queue;

    /* Clock tracking */
    long long        frames_written;

    /* Volume — software gain, 0.0 (silent) to 2.0 (200%), default 1.0 */
    float            volume;
    int              muted;

    /* Pause state */
    volatile int     paused;
    pthread_mutex_t  pause_mutex;
    pthread_cond_t   pause_cond;

    /* Sync with video if separate */
    volatile int64_t *video_pts;
    int64_t           audio_pts;
    double            video_time_base;
    double            audio_time_base;
} AudioContext;

int       audio_open(AudioContext *ctx, AVStream *stream,
                     const char *device, Queue *audio_queue);
void      audio_run(AudioContext *ctx);
long long audio_get_clock_us(AudioContext *ctx);
void      audio_pause(AudioContext *ctx);
void      audio_resume(AudioContext *ctx);
float     audio_volume_up(AudioContext *ctx);
float     audio_volume_down(AudioContext *ctx);
int       audio_toggle_mute(AudioContext *ctx);  /* returns 1 if now muted */
void      audio_flush(AudioContext *ctx);
void      audio_close(AudioContext *ctx);

#endif
