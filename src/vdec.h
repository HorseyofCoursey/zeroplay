#ifndef VDEC_H
#define VDEC_H

#include <stdint.h>
#include <linux/videodev2.h>
#include <libavcodec/bsf.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include "queue.h"

#define VDEC_OUTPUT_BUFS  4
#define VDEC_CAPTURE_BUFS 8

typedef struct {
    int         dmabuf_fd;
    int         buf_index;
    uint32_t    width;
    uint32_t    height;
    uint32_t    src_height;
    int         sar_num;
    int         sar_den;
    int64_t     pts_us;
} DecodedFrame;

typedef struct {
    int             fd;
    Queue          *packet_queue;
    Queue          *frame_queue;

    AVBSFContext   *bsf;
    AVRational      time_base;
    uint32_t        v4l2_pixfmt;   /* V4L2 format for this stream's codec */

    uint32_t        stream_width;
    uint32_t        stream_height;
    int             sar_num;
    int             sar_den;

    uint32_t        out_buf_size;
    void           *out_mem[VDEC_OUTPUT_BUFS];

    uint32_t        width;
    uint32_t        height;
    uint32_t        orig_height;
    uint32_t        cap_buf_size;
    int             cap_dmabuf_fd[VDEC_CAPTURE_BUFS];

    int             fmt_negotiated;
} VdecContext;

int  vdec_open(VdecContext *ctx, AVStream *stream,
               Queue *packet_queue, Queue *frame_queue);
void vdec_run(VdecContext *ctx);
int  vdec_flush(VdecContext *ctx);   /* call after seek, before restart */
void vdec_requeue_frame(VdecContext *ctx, DecodedFrame *frame);
void vdec_close(VdecContext *ctx);

#endif
