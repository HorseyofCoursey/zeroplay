#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>
#include <libgen.h>
#include <libavformat/avformat.h>
#include "queue.h"
#include "demux.h"
#include "audio.h"
#include "vdec.h"
#include "drm.h"
#include "log.h"
#include "playlist.h"
#include "image.h"

int g_verbose = 0;

/* ------------------------------------------------------------------ */
/* Key definitions                                                      */
/* ------------------------------------------------------------------ */

#define KEY_LEFT    1000
#define KEY_RIGHT   1001
#define KEY_UP      1002
#define KEY_DOWN    1003

#define SEEK_SHORT_US   (60LL  * 1000000LL)   /* 1 minute  */
#define SEEK_LONG_US    (300LL * 1000000LL)   /* 5 minutes */

/* ------------------------------------------------------------------ */
/* Options                                                              */
/* ------------------------------------------------------------------ */

#define MAX_FILES 4

typedef struct {
    const char *paths[MAX_FILES];   /* files, dirs, or playlist files */
    int         path_count;
    int         loop;
    int         no_audio;
    float       vol;
    double      start_pos;
    const char *audio_device;
    float       image_duration_s;   /* seconds to show each image */
    int         shuffle;
    int64_t     hls_max_bandwidth;  /* --hls-bitrate or HLS_MAX_BANDWIDTH */
} Options;

static void print_usage(void)
{
    fprintf(stderr,
        "usage: zeroplay [options] <path> [path2 ...]\n"
        "\n"
        "  Each path can be a video/image file, a .txt/.m3u playlist,\n"
        "  or a directory. Up to %d paths may be given; each is assigned\n"
        "  to a connected display in DRM enumeration order.\n"
        "\n"
        "options:\n"
        "  --loop                  loop playlist indefinitely\n"
        "  --shuffle               randomise playlist order\n"
        "  --no-audio              disable audio\n"
        "  --vol n                 initial volume 0-200 (default 100)\n"
        "  --pos n                 start position in seconds (video only)\n"
        "  --audio-device dev      ALSA device (default: auto-detect)\n"
        "  --image-duration n      seconds per image (default 10, 0 = hold forever)\n"
        "  --verbose               print decoder/driver info\n"
        "  --help                  show this message\n"
        "\n"
        "controls:\n"
        "  p / space               pause / resume\n"
        "  left / right            seek -/+ 1 minute\n"
        "  up / down               seek -/+ 5 minutes\n"
        "  + / -                   volume up / down\n"
        "  m                       mute / unmute\n"
        "  i / o                   previous / next chapter\n"
        "  n                       skip to next playlist item\n"
        "  b                       go to previous playlist item\n"
        "  q / esc                 quit\n",
        MAX_FILES
    );
}

static int parse_args(int argc, char *argv[], Options *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->vol              = 100.0f;
    opt->image_duration_s = 10.0f;

    static struct option long_opts[] = {
        { "loop",             no_argument,       NULL, 'l' },
        { "shuffle",          no_argument,       NULL, 'r' },
        { "no-audio",         no_argument,       NULL, 'n' },
        { "vol",              required_argument, NULL, 'v' },
        { "pos",              required_argument, NULL, 'p' },
        { "audio-device",     required_argument, NULL, 'a' },
        { "image-duration",   required_argument, NULL, 'd' },
        { "verbose",          no_argument,       NULL, 'V' },
        { "hls-bitrate",      required_argument, NULL, 'B' },
        { "help",             no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
            case 'l': opt->loop             = 1;            break;
            case 'r': opt->shuffle          = 1;            break;
            case 'n': opt->no_audio         = 1;            break;
            case 'v': opt->vol              = atof(optarg); break;
            case 'p': opt->start_pos        = atof(optarg); break;
            case 'a': opt->audio_device     = optarg;       break;
            case 'd': opt->image_duration_s = atof(optarg); break;
            case 'V': g_verbose             = 1;            break;
            case 'B': opt->hls_max_bandwidth = atoll(optarg); break;
            case 'h': print_usage(); exit(0);
            default:
                fprintf(stderr, "unknown option — run with --help\n");
                return -1;
        }
    }

    /* Environment variable fallbacks */
    if (opt->hls_max_bandwidth == 0) {
        const char *hb = getenv("HLS_MAX_BANDWIDTH");
        if (hb) opt->hls_max_bandwidth = atoll(hb);
    }

    if (optind >= argc) { print_usage(); return -1; }

    while (optind < argc && opt->path_count < MAX_FILES)
        opt->paths[opt->path_count++] = argv[optind++];

    if (opt->vol < 0)   opt->vol = 0;
    if (opt->vol > 200) opt->vol = 200;
    if (opt->image_duration_s < 0) opt->image_duration_s = 0;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Terminal                                                             */
/* ------------------------------------------------------------------ */

static struct termios orig_termios;

static void term_restore(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    write(STDOUT_FILENO, "\033[?25h", 6);
}

static void term_raw(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(term_restore);
    struct termios raw = orig_termios;
    raw.c_lflag &= (unsigned int)~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    write(STDOUT_FILENO, "\033[?25l", 6);
}

static int key_poll(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;
    if (c != 27) return (int)c;
    unsigned char seq[2] = {0, 0};
    if (read(STDIN_FILENO, &seq[0], 1) != 1) return 27;
    if (seq[0] != '[') return 27;
    if (read(STDIN_FILENO, &seq[1], 1) != 1) return 27;
    switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
    }
    return 27;
}

/* ------------------------------------------------------------------ */
/* Wall clock helpers                                                   */
/* ------------------------------------------------------------------ */

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static void sleep_us(int64_t us)
{
    if (us <= 0) return;
    struct timespec ts = {
        .tv_sec  = us / 1000000LL,
        .tv_nsec = (us % 1000000LL) * 1000LL
    };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/* Signal                                                               */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_signal_quit = 0;
static void signal_handler(int sig) { (void)sig; g_signal_quit = 1; }

/* ------------------------------------------------------------------ */
/* PlayerContext — all state for one playlist/output pair              */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Playlist */
    Playlist     playlist;

    /* Video pipeline (only valid when !image_mode && pipeline_open) */
    DemuxContext demux;
    VdecContext  vdec;
    AudioContext audio;
    Queue        video_queue;
    Queue        audio_queue;
    Queue        frame_queue;
    pthread_t    dtid, vtid, atid;
    int          pipeline_open;
    int          audio_active;   /* 1 if audio was successfully opened */

    /* Playback state */
    int64_t      wall_start;
    int          frame_count;
    int64_t      current_pts;
    DecodedFrame *prev_frame;
    DecodedFrame *held_frame;
    int          eos;

    /* Image mode */
    int          image_mode;
    int64_t      image_end_us;

    /* Config */
    int          output_idx;
    int          no_audio;       /* user flag: audio disabled for this player */
    int64_t      image_duration_us;
    int64_t      duration_us;
} PlayerContext;

/* ------------------------------------------------------------------ */
/* Per-player thread trampolines                                        */
/* ------------------------------------------------------------------ */

typedef struct { PlayerContext *p; } ThreadArg;

static void *demux_thread(void *arg)
{
    PlayerContext *p = ((ThreadArg *)arg)->p; free(arg);
    demux_run(&p->demux); return NULL;
}
static void *vdec_thread(void *arg)
{
    PlayerContext *p = ((ThreadArg *)arg)->p; free(arg);
    vdec_run(&p->vdec); return NULL;
}
static void *audio_thread(void *arg)
{
    PlayerContext *p = ((ThreadArg *)arg)->p; free(arg);
    audio_run(&p->audio); return NULL;
}

static void player_threads_start(PlayerContext *p)
{
    ThreadArg *da = malloc(sizeof(*da)); da->p = p;
    ThreadArg *va = malloc(sizeof(*va)); va->p = p;
    pthread_create(&p->dtid, NULL, demux_thread, da);
    pthread_create(&p->vtid, NULL, vdec_thread,  va);
    if (p->audio_active) {
        ThreadArg *aa = malloc(sizeof(*aa)); aa->p = p;
        pthread_create(&p->atid, NULL, audio_thread, aa);
    }
}

static void player_threads_stop(PlayerContext *p)
{
    queue_close(&p->video_queue);
    queue_close(&p->audio_queue);
    queue_close(&p->frame_queue);
    pthread_join(p->dtid, NULL);
    pthread_join(p->vtid, NULL);
    if (p->audio_active)
        pthread_join(p->atid, NULL);
}

static void player_queues_reinit(PlayerContext *p)
{
    queue_init(&p->video_queue);
    queue_init(&p->audio_queue);
    queue_init_size(&p->frame_queue, FRAME_QUEUE_SIZE);
    p->demux.video_queue = &p->video_queue;
    p->demux.audio_queue = &p->audio_queue;
    p->vdec.packet_queue = &p->video_queue;
    p->vdec.frame_queue  = &p->frame_queue;
    if (p->audio_active) p->audio.audio_queue = &p->audio_queue;
}

/* ------------------------------------------------------------------ */
/* Close the video pipeline without touching the playlist              */
/* ------------------------------------------------------------------ */

static void player_close_pipeline(PlayerContext *p)
{
    if (!p->pipeline_open) return;

    if (p->held_frame) {
        vdec_requeue_frame(&p->vdec, p->held_frame); p->held_frame = NULL;
    }
    if (p->prev_frame) {
        vdec_requeue_frame(&p->vdec, p->prev_frame); p->prev_frame = NULL;
    }

    player_threads_stop(p);
    if (p->audio_active) audio_close(&p->audio);
    vdec_close(&p->vdec);
    demux_close(&p->demux);
    queue_destroy(&p->video_queue);
    queue_destroy(&p->audio_queue);
    queue_destroy(&p->frame_queue);

    p->pipeline_open  = 0;
    p->audio_active   = 0;
    p->wall_start     = 0;
    p->frame_count    = 0;
    p->current_pts    = 0;
    p->eos            = 0;
}

/* ------------------------------------------------------------------ */
/* Open the video pipeline for a single file                           */
/* Does NOT start threads — call player_threads_start separately.      */
/* ------------------------------------------------------------------ */

static int player_open_video(PlayerContext *p, const char *filename,
                              const Options *opt)
{
    queue_init(&p->video_queue);
    queue_init(&p->audio_queue);
    queue_init_size(&p->frame_queue, FRAME_QUEUE_SIZE);

    if (demux_open(&p->demux, filename, &p->video_queue, &p->audio_queue,
                   opt->hls_max_bandwidth) < 0)
        return -1;

    if (p->no_audio)
        p->demux.audio_stream_idx = -1;

    AVStream *video_stream =
        p->demux.fmt_ctx->streams[p->demux.video_stream_idx];

    if (vdec_open(&p->vdec, video_stream, &p->video_queue, &p->frame_queue) < 0)
        return -1;

    p->audio_active = 0;
    if (!p->no_audio && p->demux.audio_stream_idx >= 0) {
        AVStream *audio_stream =
            p->demux.fmt_ctx->streams[p->demux.audio_stream_idx];
        if (audio_open(&p->audio, audio_stream, opt->audio_device,
                       &p->audio_queue) == 0) {
            p->audio.volume = opt->vol / 100.0f;
            p->audio_active = 1;
        }
    }

    p->duration_us   = p->demux.duration_us;
    p->pipeline_open = 1;

    if (opt->start_pos > 0.0) {
        int64_t start_us = (int64_t)(opt->start_pos * 1000000.0);
        if (start_us > p->duration_us) start_us = p->duration_us;
        demux_seek(&p->demux, start_us);
        p->current_pts = start_us;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Seek within the current video                                        */
/* ------------------------------------------------------------------ */

static void player_seek(PlayerContext *p, int64_t target_us)
{
    if (!p->pipeline_open) return;
    fprintf(stderr, "zeroplay[%d]: seeking to %.1fs...\n",
            p->output_idx, target_us / 1e6);

    if (p->held_frame) { vdec_requeue_frame(&p->vdec, p->held_frame); p->held_frame = NULL; }
    if (p->prev_frame) { vdec_requeue_frame(&p->vdec, p->prev_frame); p->prev_frame  = NULL; }

    player_threads_stop(p);
    demux_seek(&p->demux, target_us);
    vdec_flush(&p->vdec);
    if (p->audio_active) audio_flush(&p->audio);
    player_queues_reinit(p);

    p->wall_start  = 0;
    p->frame_count = 0;
    p->eos         = 0;

    player_threads_start(p);
}

/* ------------------------------------------------------------------ */
/* Advance to the next playlist item                                    */
/* Returns 0 on success, -1 if the playlist is exhausted.              */
/* ------------------------------------------------------------------ */

static int player_advance_to_next(PlayerContext *p, DrmContext *drm,
                                   const Options *opt)
{
    if (playlist_advance(&p->playlist) < 0)
        return -1;

    PlaylistItem *item = playlist_current(&p->playlist);
    if (!item) return -1;

    if (item->type == ITEM_IMAGE) {
        player_close_pipeline(p);
        p->image_mode = 0;

        uint8_t *pixels = NULL;
        int w = 0, h = 0, stride = 0;
        if (image_decode_xrgb(item->path, &pixels, &w, &h, &stride) == 0) {
            drm_present_image(drm, p->output_idx, pixels, w, h, stride);
            free(pixels);
            fprintf(stderr, "zeroplay[%d]: showing '%s'\n",
                    p->output_idx, basename(item->path));
        } else {
            fprintf(stderr, "zeroplay[%d]: failed to decode image '%s', skipping\n",
                    p->output_idx, item->path);
            /* Try the next item immediately */
            return player_advance_to_next(p, drm, opt);
        }
        p->image_mode   = 1;
        p->image_end_us = (p->image_duration_us > 0) ? now_us() + p->image_duration_us : 0;

    } else {
        /* Video */
        player_close_pipeline(p);
        p->image_mode = 0;

        if (player_open_video(p, item->path, opt) < 0) {
            fprintf(stderr, "zeroplay[%d]: failed to open '%s', skipping\n",
                    p->output_idx, item->path);
            return player_advance_to_next(p, drm, opt);
        }
        fprintf(stderr, "zeroplay[%d]: playing '%s'\n",
                p->output_idx, basename(item->path));
        player_threads_start(p);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Open a player (playlist + first item)                               */
/* ------------------------------------------------------------------ */

static int player_open(PlayerContext *p, const char *path,
                        const Options *opt, int output_idx, DrmContext *drm)
{
    memset(p, 0, sizeof(*p));
    p->output_idx       = output_idx;
    p->no_audio         = opt->no_audio || (output_idx != 0);
    p->image_duration_us = (int64_t)(opt->image_duration_s * 1000000.0);

    if (playlist_open(&p->playlist, path, opt->loop, opt->shuffle) < 0)
        return -1;

    PlaylistItem *item = playlist_current(&p->playlist);
    if (!item) { playlist_close(&p->playlist); return -1; }

    if (item->type == ITEM_IMAGE) {
        uint8_t *pixels = NULL;
        int w = 0, h = 0, stride = 0;
        if (image_decode_xrgb(item->path, &pixels, &w, &h, &stride) == 0) {
            drm_present_image(drm, output_idx, pixels, w, h, stride);
            free(pixels);
        } else {
            /* First item failed — try advancing */
            if (player_advance_to_next(p, drm, opt) < 0) {
                playlist_close(&p->playlist);
                return -1;
            }
            return 0;
        }
        p->image_mode   = 1;
        p->image_end_us = (p->image_duration_us > 0) ? now_us() + p->image_duration_us : 0;
        fprintf(stderr, "zeroplay[%d]: image '%s' (%.0fs)\n",
                output_idx, item->path,
                (double)p->image_duration_us / 1e6);

    } else {
        if (player_open_video(p, item->path, opt) < 0) {
            playlist_close(&p->playlist);
            return -1;
        }
        fprintf(stderr, "zeroplay[%d]: playing '%s'\n",
                output_idx, basename(item->path));
        /* Threads will be started in main after all players are opened */
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Full player teardown                                                 */
/* ------------------------------------------------------------------ */

static void player_shutdown(PlayerContext *p)
{
    player_close_pipeline(p);
    playlist_close(&p->playlist);
}

static void player_go_to_prev(PlayerContext *p, DrmContext *drm,
                               const Options *opt)
{
    player_close_pipeline(p);
    p->image_mode = 0;

    if (playlist_prev(&p->playlist) < 0) return;

    PlaylistItem *item = playlist_current(&p->playlist);
    if (!item) return;

    if (item->type == ITEM_IMAGE) {
        uint8_t *pixels = NULL;
        int w = 0, h = 0, stride = 0;
        if (image_decode_xrgb(item->path, &pixels, &w, &h, &stride) == 0) {
            drm_present_image(drm, p->output_idx, pixels, w, h, stride);
            free(pixels);
        }
        p->image_mode   = 1;
        p->image_end_us = (p->image_duration_us > 0) ? now_us() + p->image_duration_us : 0;
        fprintf(stderr, "zeroplay[%d]: showing '%s'\n",
                p->output_idx, basename(item->path));
    } else {
        if (player_open_video(p, item->path, opt) == 0)
            player_threads_start(p);
    }
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    Options opt;
    if (parse_args(argc, argv, &opt) < 0)
        return 1;

    avformat_network_init();

    DrmContext drm;
    if (drm_open(&drm) < 0)
        return 1;

    /* Clamp path count to available outputs */
    int player_count = opt.path_count;
    if (player_count > drm.output_count) {
        fprintf(stderr,
                "zeroplay: %d path(s) given but only %d output(s) connected"
                " — ignoring extra\n",
                player_count, drm.output_count);
        player_count = drm.output_count;
    }

    /* Open all players */
    PlayerContext players[MAX_FILES];
    int opened = 0;
    for (int i = 0; i < player_count; i++) {
        if (player_open(&players[i], opt.paths[i], &opt, i, &drm) < 0) {
            fprintf(stderr, "zeroplay: failed to open '%s'\n", opt.paths[i]);
            for (int j = 0; j < opened; j++) player_shutdown(&players[j]);
            drm_close(&drm);
            return 1;
        }
        opened++;
    }

    term_raw();
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* Start video pipelines (skip image-mode players) */
    for (int i = 0; i < opened; i++)
        if (!players[i].image_mode && players[i].pipeline_open)
            player_threads_start(&players[i]);

    /* Raise main thread to realtime */
    {
        struct sched_param sp = { .sched_priority = 10 };
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }

    int paused = 0;
    int quit   = 0;

    while (!quit) {
        if (g_signal_quit) { quit = 1; break; }

        int key = key_poll();

        if (key == 'q' || key == 'Q' || key == 27) { quit = 1; break; }

        if (key == 'p' || key == 'P' || key == ' ') {
            paused = !paused;
            for (int i = 0; i < opened; i++) {
                if (players[i].image_mode) continue;
                if (paused) {
                    if (players[i].audio_active) audio_pause(&players[i].audio);
                } else {
                    if (players[i].held_frame)
                        players[i].wall_start =
                            now_us() - players[i].held_frame->pts_us;
                    if (players[i].audio_active) audio_resume(&players[i].audio);
                }
            }
            fprintf(stderr, "zeroplay: %s\n", paused ? "paused" : "playing");
        }

        if (key == '+' || key == '=') {
            for (int i = 0; i < opened; i++)
                if (players[i].audio_active) {
                    float v = audio_volume_up(&players[i].audio);
                    if (i == 0)
                        fprintf(stderr, "zeroplay: volume %.0f%%\n", v * 100.0f);
                }
        }
        if (key == '-' || key == '_') {
            for (int i = 0; i < opened; i++)
                if (players[i].audio_active) {
                    float v = audio_volume_down(&players[i].audio);
                    if (i == 0)
                        fprintf(stderr, "zeroplay: volume %.0f%%\n", v * 100.0f);
                }
        }
        if (key == 'm' || key == 'M') {
            for (int i = 0; i < opened; i++)
                if (players[i].audio_active) {
                    int muted = audio_toggle_mute(&players[i].audio);
                    if (i == 0)
                        fprintf(stderr, "zeroplay: %s\n",
                                muted ? "muted" : "unmuted");
                }
        }

        if (key == 'n' || key == 'N') {
            for (int i = 0; i < opened; i++) {
                player_close_pipeline(&players[i]);
                players[i].image_mode = 0;
                if (player_advance_to_next(&players[i], &drm, &opt) < 0)
                    players[i].eos = 1;
            }
            fprintf(stderr, "zeroplay: skip\n");
        }

        if (key == 'b' || key == 'B') {
            for (int i = 0; i < opened; i++)
                player_go_to_prev(&players[i], &drm, &opt);
            fprintf(stderr, "zeroplay: previous\n");
        }

        if (key == 'o' || key == 'O' || key == 'i' || key == 'I') {
            for (int i = 0; i < opened; i++) {
                if (players[i].image_mode || !players[i].pipeline_open) continue;
                int64_t target = 0;
                int found = (key == 'o' || key == 'O')
                    ? demux_next_chapter(&players[i].demux,
                                         players[i].current_pts, &target)
                    : demux_prev_chapter(&players[i].demux,
                                         players[i].current_pts, &target);
                if (found == 0) {
                    int was_paused = paused; paused = 0;
                    player_seek(&players[i], target);
                    players[i].current_pts = target;
                    if (was_paused) {
                        paused = 1;
                        if (players[i].audio_active)
                            audio_pause(&players[i].audio);
                    }
                }
            }
        }

        if (key == KEY_RIGHT || key == KEY_LEFT ||
            key == KEY_UP    || key == KEY_DOWN) {
            int64_t delta = 0;
            if      (key == KEY_RIGHT) delta = +SEEK_SHORT_US;
            else if (key == KEY_LEFT)  delta = -SEEK_SHORT_US;
            else if (key == KEY_UP)    delta = +SEEK_LONG_US;
            else if (key == KEY_DOWN)  delta = -SEEK_LONG_US;

            int was_paused = paused; paused = 0;
            for (int i = 0; i < opened; i++) {
                if (players[i].image_mode || !players[i].pipeline_open) continue;
                int64_t target = players[i].current_pts + delta;
                if (target < 0) target = 0;
                if (players[i].duration_us > 0 &&
                    target > players[i].duration_us)
                    target = players[i].duration_us;
                player_seek(&players[i], target);
                players[i].current_pts = target;
            }
            if (was_paused) {
                paused = 1;
                for (int i = 0; i < opened; i++)
                    if (players[i].audio_active) audio_pause(&players[i].audio);
            }
        }

        if (paused) { sleep_us(10000); continue; }

        /* ---- Advance each player ---- */
        int64_t next_due = INT64_MAX;
        int     all_eos  = 1;

        for (int i = 0; i < opened; i++) {
            PlayerContext *p = &players[i];
            if (p->eos) continue;
            all_eos = 0;

            /* ---- Image mode ---- */
            if (p->image_mode) {
                if (p->image_end_us > 0 && now_us() >= p->image_end_us) {
                    if (player_advance_to_next(p, &drm, &opt) < 0)
                        p->eos = 1;
                    else
                        next_due = 0;
                } else if (p->image_end_us > 0) {
                    if (p->image_end_us < next_due)
                        next_due = p->image_end_us;
                } else {
                    /* duration 0 = hold forever, sleep a bit */
                    sleep_us(50000);
                }
                continue;
            }

            /* ---- Video mode ---- */
            if (!p->held_frame) {
                void *item = NULL;
                int rc = queue_trypop(&p->frame_queue, &item);
                if (rc == 0) {
                    next_due = 0;
                    continue;
                }
                if (rc < 0) {
                    /* End of stream — advance to next playlist item */
                    if (player_advance_to_next(p, &drm, &opt) < 0)
                        p->eos = 1;
                    else
                        next_due = 0;
                    continue;
                }
                p->held_frame = (DecodedFrame *)item;
                p->frame_count++;
            }

            DecodedFrame *frame = p->held_frame;
            p->current_pts = frame->pts_us;

            if (p->frame_count == 1 || p->wall_start == 0)
                p->wall_start = now_us() - frame->pts_us;

            int64_t due = p->wall_start + frame->pts_us;
            int64_t now = now_us();

            if (due <= now) {
                drm_present(&drm, p->output_idx, frame);
                p->held_frame = NULL;
                if (p->prev_frame)
                    vdec_requeue_frame(&p->vdec, p->prev_frame);
                p->prev_frame = frame;
            } else {
                if (due < next_due) next_due = due;
            }
        }

        if (all_eos) break;

        if (next_due != INT64_MAX) {
            int64_t sl = next_due - now_us();
            if (sl > 2000) sl = 2000;
            if (sl > 0)    sleep_us(sl);
        } else {
            sleep_us(2000);
        }
    }

    for (int i = 0; i < opened; i++) player_shutdown(&players[i]);
    drm_close(&drm);

    fprintf(stderr, "have a nice day ;)\n");
    return 0;
}
