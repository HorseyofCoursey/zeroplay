#ifndef DEMUX_H
#define DEMUX_H

#include <libavformat/avformat.h>
#include "queue.h"

typedef struct {
    AVFormatContext *fmt_ctx;
    int              video_stream_idx;
    int              audio_stream_idx;
    int              subtitle_stream_idx;  /* -1 if none */
    int64_t          duration_us;          /* total file duration in microseconds */
    Queue           *video_queue;
    Queue           *audio_queue;
    Queue           *subtitle_queue;       /* NULL = drop subtitle packets */
} DemuxContext;

int  demux_open(DemuxContext *ctx, const char *filename,
                Queue *video_queue, Queue *audio_queue,
                int64_t hls_max_bandwidth, int separate_audio);
void demux_run(DemuxContext *ctx);
int  demux_seek(DemuxContext *ctx, int64_t target_us);
int  demux_next_chapter(DemuxContext *ctx, int64_t current_us, int64_t *target_us);
int  demux_prev_chapter(DemuxContext *ctx, int64_t current_us, int64_t *target_us);
int  demux_has_subtitles(DemuxContext *ctx);  /* 1 if embedded subtitle stream found */
void demux_close(DemuxContext *ctx);

#endif
