#ifndef DEMUX_H
#define DEMUX_H

#include <libavformat/avformat.h>
#include "queue.h"

typedef struct {
    AVFormatContext *fmt_ctx;
    int              video_stream_idx;
    int              audio_stream_idx;
    int64_t          duration_us;    /* total file duration in microseconds */
    Queue           *video_queue;
    Queue           *audio_queue;
} DemuxContext;

int  demux_open(DemuxContext *ctx, const char *filename,
                Queue *video_queue, Queue *audio_queue,
                int64_t hls_max_bandwidth);
void demux_run(DemuxContext *ctx);
int  demux_seek(DemuxContext *ctx, int64_t target_us);
int  demux_next_chapter(DemuxContext *ctx, int64_t current_us, int64_t *target_us);
int  demux_prev_chapter(DemuxContext *ctx, int64_t current_us, int64_t *target_us);
void demux_close(DemuxContext *ctx);

#endif
