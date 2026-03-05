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
#include <libavformat/avformat.h>
#include "queue.h"
#include "demux.h"
#include "audio.h"
#include "vdec.h"
#include "drm.h"
#include "log.h"

int g_verbose = 0;

/* ------------------------------------------------------------------ */
/* Globals                                                              */
/* ------------------------------------------------------------------ */

static DemuxContext demux;
static VdecContext  vdec;
static AudioContext audio;
static DrmContext   drm;

static Queue video_queue;
static Queue audio_queue;
static Queue frame_queue;

static pthread_t dtid, vtid, atid;

static volatile sig_atomic_t g_signal_quit = 0;

static void signal_handler(int sig)
{
    (void)sig;
    g_signal_quit = 1;
}

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

typedef struct {
    const char *filename;
    int         loop;          /* --loop         */
    int         no_audio;      /* --no-audio      */
    float       vol;           /* --vol 0-200     */
    double      start_pos;     /* --pos seconds   */
    const char *audio_device;  /* --audio-device  */
} Options;

static void print_usage(void)
{
    fprintf(stderr,
        "usage: zeroplay [options] <file>\n"
        "\n"
        "options:\n"
        "  --loop              loop playback indefinitely\n"
        "  --no-audio          disable audio\n"
        "  --vol n             initial volume 0-200 (default 100)\n"
        "  --pos n             start position in seconds\n"
        "  --audio-device dev  ALSA device (default: plughw:CARD=vc4hdmi,DEV=0)\n"
        "  --verbose           print decoder/driver info\n"
        "  --help              show this message\n"
        "\n"
        "controls:\n"
        "  p / space           pause / resume\n"
        "  left / right        seek -/+ 1 minute\n"
        "  up / down           seek -/+ 5 minutes\n"
        "  + / -               volume up / down\n"
        "  m                   mute / unmute\n"
        "  i / o               previous / next chapter\n"
        "  q / esc             quit\n"
    );
}

static int parse_args(int argc, char *argv[], Options *opt)
{
    memset(opt, 0, sizeof(*opt));
    opt->vol = 100.0f;

    static struct option long_opts[] = {
        { "loop",         no_argument,       NULL, 'l' },
        { "no-audio",     no_argument,       NULL, 'n' },
        { "vol",          required_argument, NULL, 'v' },
        { "pos",          required_argument, NULL, 'p' },
        { "audio-device", required_argument, NULL, 'a' },
        { "verbose",      no_argument,       NULL, 'V' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "", long_opts, NULL)) != -1) {
        switch (c) {
            case 'l': opt->loop         = 1;              break;
            case 'n': opt->no_audio     = 1;              break;
            case 'v': opt->vol          = atof(optarg);   break;
            case 'p': opt->start_pos    = atof(optarg);   break;
            case 'a': opt->audio_device = optarg;         break;
            case 'V': g_verbose         = 1;              break;
            case 'h': print_usage(); exit(0);
            default:
                fprintf(stderr, "unknown option — run with --help\n");
                return -1;
        }
    }

    if (optind >= argc) {
        print_usage();
        return -1;
    }

    opt->filename = argv[optind];

    /* Clamp volume */
    if (opt->vol < 0)   opt->vol = 0;
    if (opt->vol > 200) opt->vol = 200;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Terminal raw mode                                                    */
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
    if (read(STDIN_FILENO, &c, 1) != 1)
        return 0;
    if (c != 27)
        return (int)c;
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
/* Threads                                                              */
/* ------------------------------------------------------------------ */

static void *demux_thread(void *arg) { (void)arg; demux_run(&demux); return NULL; }
static void *vdec_thread(void *arg)  { (void)arg; vdec_run(&vdec);   return NULL; }
static void *audio_thread(void *arg) {
    (void)arg;
    audio_run(&audio);
    return NULL;
}

static int  g_no_audio = 0;

static void threads_start(void)
{
    pthread_create(&dtid, NULL, demux_thread, NULL);
    pthread_create(&vtid, NULL, vdec_thread,  NULL);
    if (!g_no_audio)
        pthread_create(&atid, NULL, audio_thread, NULL);
}

static void threads_stop(void)
{
    queue_close(&video_queue);
    queue_close(&audio_queue);
    queue_close(&frame_queue);
    pthread_join(dtid, NULL);
    pthread_join(vtid, NULL);
    if (!g_no_audio)
        pthread_join(atid, NULL);
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
/* Seek                                                                 */
/* ------------------------------------------------------------------ */

static void do_seek(int64_t *wall_start, int *frame_count,
                    DecodedFrame **prev_frame, DecodedFrame **held_frame,
                    int64_t target_us)
{
    fprintf(stderr, "zeroplay: seeking to %.1fs...\n", target_us / 1e6);

    if (*held_frame) { vdec_requeue_frame(&vdec, *held_frame); *held_frame = NULL; }
    if (*prev_frame) { vdec_requeue_frame(&vdec, *prev_frame); *prev_frame  = NULL; }

    threads_stop();

    demux_seek(&demux, target_us);
    vdec_flush(&vdec);
    if (!g_no_audio) audio_flush(&audio);

    queue_init(&video_queue);
    queue_init(&audio_queue);
    queue_init_size(&frame_queue, FRAME_QUEUE_SIZE);

    demux.video_queue = &video_queue;
    demux.audio_queue = &audio_queue;
    vdec.packet_queue = &video_queue;
    vdec.frame_queue  = &frame_queue;
    if (!g_no_audio) audio.audio_queue = &audio_queue;

    *wall_start  = 0;
    *frame_count = 0;

    threads_start();
}

/* ------------------------------------------------------------------ */
/* Loop restart                                                         */
/* ------------------------------------------------------------------ */

static void do_loop(int64_t *wall_start, int *frame_count,
                    DecodedFrame **prev_frame, DecodedFrame **held_frame)
{
    if (*held_frame) { vdec_requeue_frame(&vdec, *held_frame); *held_frame = NULL; }
    if (*prev_frame) { vdec_requeue_frame(&vdec, *prev_frame); *prev_frame  = NULL; }

    threads_stop();

    demux_seek(&demux, 0);
    vdec_flush(&vdec);
    if (!g_no_audio) audio_flush(&audio);

    queue_init(&video_queue);
    queue_init(&audio_queue);
    queue_init_size(&frame_queue, FRAME_QUEUE_SIZE);

    demux.video_queue = &video_queue;
    demux.audio_queue = &audio_queue;
    vdec.packet_queue = &video_queue;
    vdec.frame_queue  = &frame_queue;
    if (!g_no_audio) audio.audio_queue = &audio_queue;

    *wall_start  = 0;
    *frame_count = 0;

    threads_start();
}

/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    Options opt;
    if (parse_args(argc, argv, &opt) < 0)
        return 1;

    g_no_audio = opt.no_audio;

    queue_init(&video_queue);
    queue_init(&audio_queue);
    queue_init_size(&frame_queue, FRAME_QUEUE_SIZE);

    if (demux_open(&demux, opt.filename, &video_queue, &audio_queue) < 0)
        return 1;

    /* With --no-audio, tell demuxer to discard audio packets so the
     * audio queue never fills and blocks the demux thread. */
    if (opt.no_audio)
        demux.audio_stream_idx = -1;

    AVStream *video_stream =
        demux.fmt_ctx->streams[demux.video_stream_idx];

    if (vdec_open(&vdec, video_stream, &video_queue, &frame_queue) < 0)
        return 1;

    if (!opt.no_audio && demux.audio_stream_idx >= 0) {
        AVStream *audio_stream =
            demux.fmt_ctx->streams[demux.audio_stream_idx];
        if (audio_open(&audio, audio_stream, opt.audio_device,
                       &audio_queue) < 0)
            return 1;
        audio.volume = opt.vol / 100.0f;
    } else {
        g_no_audio = 1;
    }

    if (drm_open(&drm) < 0)
        return 1;

    term_raw();
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    int64_t duration_us = demux.duration_us;

    /* Seek to start position if requested */
    if (opt.start_pos > 0.0) {
        int64_t start_us = (int64_t)(opt.start_pos * 1000000.0);
        if (start_us > duration_us) start_us = duration_us;
        demux_seek(&demux, start_us);
    }

    threads_start();

    /* Raise main thread to realtime so the display loop isn't starved
     * by audio decode on the Pi Zero 2W's limited CPU budget. */
    {
        struct sched_param sp = { .sched_priority = 10 };
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }

    int          frame_count = 0;
    int64_t      wall_start  = 0;
    int64_t      current_pts = (int64_t)(opt.start_pos * 1000000.0);
    int          paused      = 0;
    int          quit        = 0;
    void        *item        = NULL;
    DecodedFrame *prev_frame = NULL;
    DecodedFrame *held_frame = NULL;

    while (!quit) {
        if (g_signal_quit) { quit = 1; break; }

        int key = key_poll();

        if (key == 'q' || key == 'Q' || key == 27) {
            quit = 1; break;
        }

        if (key == 'p' || key == 'P' || key == ' ') {
            paused = !paused;
            if (paused) {
                if (!g_no_audio) audio_pause(&audio);
                fprintf(stderr, "zeroplay: paused at %.1fs\n", current_pts / 1e6);
            } else {
                if (held_frame) wall_start = now_us() - held_frame->pts_us;
                if (!g_no_audio) audio_resume(&audio);
                fprintf(stderr, "zeroplay: playing\n");
            }
        }

        if (key == '+' || key == '=') {
            if (!g_no_audio) {
                float v = audio_volume_up(&audio);
                fprintf(stderr, "zeroplay: volume %.0f%%\n", v * 100.0f);
            }
        }
        if (key == '-' || key == '_') {
            if (!g_no_audio) {
                float v = audio_volume_down(&audio);
                fprintf(stderr, "zeroplay: volume %.0f%%\n", v * 100.0f);
            }
        }
        if (key == 'm' || key == 'M') {
            if (!g_no_audio) {
                int muted = audio_toggle_mute(&audio);
                fprintf(stderr, "zeroplay: %s\n", muted ? "muted" : "unmuted");
            }
        }

        if (key == 'o' || key == 'O' || key == 'i' || key == 'I') {
            int64_t target = 0;
            int found = (key == 'o' || key == 'O')
                ? demux_next_chapter(&demux, current_pts, &target)
                : demux_prev_chapter(&demux, current_pts, &target);
            if (found == 0) {
                int was_paused = paused; paused = 0;
                do_seek(&wall_start, &frame_count, &prev_frame, &held_frame, target);
                current_pts = target;
                if (was_paused) { paused = 1; if (!g_no_audio) audio_pause(&audio); }
            }
        }

        if (key == KEY_RIGHT || key == KEY_LEFT ||
            key == KEY_UP    || key == KEY_DOWN) {

            int64_t delta = 0;
            if      (key == KEY_RIGHT) delta = +SEEK_SHORT_US;
            else if (key == KEY_LEFT)  delta = -SEEK_SHORT_US;
            else if (key == KEY_UP)    delta = +SEEK_LONG_US;
            else if (key == KEY_DOWN)  delta = -SEEK_LONG_US;

            int64_t target = current_pts + delta;
            if (target < 0) target = 0;
            if (duration_us > 0 && target > duration_us) target = duration_us;

            int was_paused = paused; paused = 0;
            do_seek(&wall_start, &frame_count, &prev_frame, &held_frame, target);
            current_pts = target;
            if (was_paused) { paused = 1; if (!g_no_audio) audio_pause(&audio); }
        }

        if (paused) { sleep_us(10000); continue; }

        /* -- Get next frame -- */
        DecodedFrame *frame;

        if (held_frame) {
            frame      = held_frame;
            held_frame = NULL;
        } else {
            int rc = queue_trypop(&frame_queue, &item);
            if (rc == 0) {
                /* No frame ready yet — yield and re-poll keys next iteration */
                sleep_us(2000);
                continue;
            }
            if (rc < 0) {
                /* Queue closed — end of stream */
                if (opt.loop) {
                    do_loop(&wall_start, &frame_count, &prev_frame, &held_frame);
                    continue;
                }
                break;
            }
            frame = (DecodedFrame *)item;
            frame_count++;
        }

        current_pts = frame->pts_us;

        if (frame_count == 1 || wall_start == 0)
            wall_start = now_us() - frame->pts_us;

        int64_t due   = wall_start + frame->pts_us;
        int64_t delay = due - now_us();

        while (delay > 5000 && !paused && !quit) {
            sleep_us(5000);
            if (g_signal_quit) { quit = 1; break; }
            delay = due - now_us();

            key = key_poll();
            if (key == 'q' || key == 'Q' || key == 27) {
                quit = 1; break;
            }
            if (key == 'p' || key == 'P' || key == ' ') {
                paused = 1;
                if (!g_no_audio) audio_pause(&audio);
                fprintf(stderr, "zeroplay: paused at %.1fs\n", current_pts / 1e6);
                break;
            }
            if (key == '+' || key == '=') {
                if (!g_no_audio) {
                    float v = audio_volume_up(&audio);
                    fprintf(stderr, "zeroplay: volume %.0f%%\n", v * 100.0f);
                }
            }
            if (key == '-' || key == '_') {
                if (!g_no_audio) {
                    float v = audio_volume_down(&audio);
                    fprintf(stderr, "zeroplay: volume %.0f%%\n", v * 100.0f);
                }
            }
            if (key == 'm' || key == 'M') {
                if (!g_no_audio) {
                    int muted = audio_toggle_mute(&audio);
                    fprintf(stderr, "zeroplay: %s\n", muted ? "muted" : "unmuted");
                }
            }
            if (key == KEY_RIGHT || key == KEY_LEFT ||
                key == KEY_UP    || key == KEY_DOWN) {

                int64_t delta = 0;
                if      (key == KEY_RIGHT) delta = +SEEK_SHORT_US;
                else if (key == KEY_LEFT)  delta = -SEEK_SHORT_US;
                else if (key == KEY_UP)    delta = +SEEK_LONG_US;
                else if (key == KEY_DOWN)  delta = -SEEK_LONG_US;

                held_frame = frame;
                frame = NULL;

                int64_t target = current_pts + delta;
                if (target < 0) target = 0;
                if (duration_us > 0 && target > duration_us) target = duration_us;

                do_seek(&wall_start, &frame_count, &prev_frame, &held_frame, target);
                current_pts = target;
                break;
            }
        }

        if (quit)   { if (frame) held_frame = frame; break; }
        if (!frame) continue;
        if (paused) { held_frame = frame; continue; }

        drm_present(&drm, frame);

        if (prev_frame)
            vdec_requeue_frame(&vdec, prev_frame);
        prev_frame = frame;
    }

    if (held_frame) vdec_requeue_frame(&vdec, held_frame);
    if (prev_frame) vdec_requeue_frame(&vdec, prev_frame);

    queue_close(&video_queue);
    queue_close(&audio_queue);
    queue_close(&frame_queue);
    pthread_join(dtid, NULL);
    pthread_join(vtid, NULL);
    if (!g_no_audio)
        pthread_join(atid, NULL);

    drm_close(&drm);
    if (!g_no_audio) audio_close(&audio);
    vdec_close(&vdec);
    demux_close(&demux);
    queue_destroy(&video_queue);
    queue_destroy(&audio_queue);
    queue_destroy(&frame_queue);

    fprintf(stderr, "have a nice day ;)\n");
    return 0;
}
