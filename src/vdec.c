#include "log.h"
#include "vdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <time.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static int set_output_format(VdecContext *ctx)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width       = ctx->stream_width;
    fmt.fmt.pix_mp.height      = ctx->stream_height;
    fmt.fmt.pix_mp.pixelformat = ctx->v4l2_pixfmt;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes  = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage =
        ctx->stream_width * ctx->stream_height;

    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "vdec: hardware decoder does not support this codec — try a different file\n");
        return -1;
    }
    ctx->out_buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
    vlog("vdec: output format set — %.4s %ux%u, buf_size=%u\n",
            (char*)&ctx->v4l2_pixfmt,
            ctx->stream_width, ctx->stream_height, ctx->out_buf_size);
    return 0;
}

static int alloc_output_buffers(VdecContext *ctx)
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = VDEC_OUTPUT_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("vdec: VIDIOC_REQBUFS OUTPUT");
        return -1;
    }

    for (int i = 0; i < VDEC_OUTPUT_BUFS; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane  plane;
        memset(&buf,   0, sizeof(buf));
        memset(&plane, 0, sizeof(plane));

        buf.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = (uint32_t)i;
        buf.m.planes = &plane;
        buf.length   = 1;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("vdec: VIDIOC_QUERYBUF OUTPUT");
            return -1;
        }

        ctx->out_mem[i] = mmap(NULL, plane.length,
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               ctx->fd, plane.m.mem_offset);
        if (ctx->out_mem[i] == MAP_FAILED) {
            perror("vdec: mmap OUTPUT");
            return -1;
        }
    }

    vlog("vdec: %d output buffers allocated\n", VDEC_OUTPUT_BUFS);
    return 0;
}

static int stream_off(VdecContext *ctx)
{
    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    return 0;
}

static int handle_source_change(VdecContext *ctx)
{
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_G_FMT, &fmt) < 0) {
        perror("vdec: VIDIOC_G_FMT");
        return -1;
    }

    uint32_t w = fmt.fmt.pix_mp.width;
    uint32_t h = fmt.fmt.pix_mp.height;

    /* Some files cause a spurious SOURCE_CHANGE with zero/tiny dimensions
     * before the real one. Fall back to the declared stream dimensions. */
    if (w <= 32 || h <= 32) {
        w = ctx->stream_width;
        h = ctx->stream_height;
    }
    if (w == 0 || h == 0)
        return 0;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < VDEC_CAPTURE_BUFS; i++) {
        if (ctx->cap_dmabuf_fd[i] > 0) {
            close(ctx->cap_dmabuf_fd[i]);
            ctx->cap_dmabuf_fd[i] = -1;
        }
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 0;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(ctx->fd, VIDIOC_REQBUFS, &req);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = w;
    fmt.fmt.pix_mp.height      = h;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes  = 1;

    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("vdec: VIDIOC_S_FMT CAPTURE");
        return -1;
    }

    ctx->width        = fmt.fmt.pix_mp.width;
    ctx->height       = fmt.fmt.pix_mp.height;
    ctx->stride       = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    ctx->cap_buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

    memset(&req, 0, sizeof(req));
    req.count  = VDEC_CAPTURE_BUFS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("vdec: VIDIOC_REQBUFS CAPTURE");
        return -1;
    }

    for (int i = 0; i < VDEC_CAPTURE_BUFS; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane  plane;
        memset(&buf,   0, sizeof(buf));
        memset(&plane, 0, sizeof(plane));

        buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory   = V4L2_MEMORY_MMAP;
        buf.index    = (uint32_t)i;
        buf.m.planes = &plane;
        buf.length   = 1;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("vdec: VIDIOC_QUERYBUF CAPTURE");
            return -1;
        }

        struct v4l2_exportbuffer expbuf;
        memset(&expbuf, 0, sizeof(expbuf));
        expbuf.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        expbuf.index = (uint32_t)i;
        expbuf.plane = 0;
        expbuf.flags = O_RDONLY;

        if (xioctl(ctx->fd, VIDIOC_EXPBUF, &expbuf) < 0) {
            perror("vdec: VIDIOC_EXPBUF");
            return -1;
        }
        ctx->cap_dmabuf_fd[i] = expbuf.fd;

        memset(&plane, 0, sizeof(plane));
        buf.m.planes = &plane;
        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("vdec: VIDIOC_QBUF CAPTURE");
            return -1;
        }
    }

    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("vdec: VIDIOC_STREAMON CAPTURE");
        return -1;
    }

    ctx->fmt_negotiated = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Feed one Annex B packet — convert PTS to microseconds               */
/* ------------------------------------------------------------------ */

static int feed_packet(VdecContext *ctx, int buf_idx, AVPacket *pkt)
{
    uint32_t copy_size = (uint32_t)pkt->size;
    if (copy_size > ctx->out_buf_size) {
        fprintf(stderr, "vdec: packet truncated %u -> %u bytes — increase output buffer\n",
                copy_size, ctx->out_buf_size);
        copy_size = ctx->out_buf_size;
    }
    memcpy(ctx->out_mem[buf_idx], pkt->data, copy_size);

    int64_t pts_us = 0;
    if (pkt->pts != AV_NOPTS_VALUE)
        pts_us = av_rescale_q(pkt->pts, ctx->time_base,
                              (AVRational){1, 1000000});

    struct v4l2_buffer buf;
    struct v4l2_plane  plane;
    memset(&buf,   0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));

    buf.type              = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory            = V4L2_MEMORY_MMAP;
    buf.index             = (uint32_t)buf_idx;
    buf.m.planes          = &plane;
    buf.length            = 1;
    buf.field             = V4L2_FIELD_NONE;
    if (pkt->flags & AV_PKT_FLAG_KEY)
        buf.flags |= V4L2_BUF_FLAG_KEYFRAME;
    buf.timestamp.tv_sec  = (long)(pts_us / 1000000LL);
    buf.timestamp.tv_usec = (long)(pts_us % 1000000LL);
    plane.bytesused       = copy_size;
    plane.length          = ctx->out_buf_size;

    if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("vdec: VIDIOC_QBUF OUTPUT");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int vdec_open(VdecContext *ctx, AVStream *stream,
              Queue *packet_queue, Queue *frame_queue)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->packet_queue  = packet_queue;
    ctx->frame_queue   = frame_queue;
    ctx->time_base     = stream->time_base;
    ctx->orig_height   = (uint32_t)stream->codecpar->height;
    ctx->stream_width  = (uint32_t)stream->codecpar->width;
    ctx->stream_height = (uint32_t)stream->codecpar->height;

    ctx->sar_num = stream->codecpar->sample_aspect_ratio.num;
    ctx->sar_den = stream->codecpar->sample_aspect_ratio.den;
    if (ctx->sar_num <= 0 || ctx->sar_den <= 0) {
        ctx->sar_num = 1;
        ctx->sar_den = 1;
    }

    for (int i = 0; i < VDEC_CAPTURE_BUFS; i++)
        ctx->cap_dmabuf_fd[i] = -1;

    /* Map codec to V4L2 pixel format and select appropriate BSF */
    enum AVCodecID codec_id = stream->codecpar->codec_id;
    uint32_t v4l2_fmt;
    const char *bsf_name = NULL;

    switch (codec_id) {
        case AV_CODEC_ID_H264:
            v4l2_fmt = V4L2_PIX_FMT_H264;
            /* MP4/MOV wrap H264 in AVCC format — needs BSF to convert to
             * Annex B. Detect by extradata: AVCC starts with 0x01,
             * Annex B starts with 0x00. */
            if (stream->codecpar->extradata_size > 0 &&
                stream->codecpar->extradata[0] != 0x00)
                bsf_name = "h264_mp4toannexb";
            break;
        case AV_CODEC_ID_VP8:
            v4l2_fmt = V4L2_PIX_FMT_VP8;
            break;
        case AV_CODEC_ID_VP9:
            v4l2_fmt = V4L2_PIX_FMT_VP9;
            break;
        case AV_CODEC_ID_HEVC:
            fprintf(stderr, "vdec: HEVC is not supported by the bcm2835 hardware decoder\n");
            return -1;
        default:
            fprintf(stderr, "vdec: unsupported codec %s\n",
                    avcodec_get_name(codec_id));
            return -1;
    }
    ctx->v4l2_pixfmt = v4l2_fmt;

    /* Initialise BSF if needed */
    if (bsf_name) {
        const AVBitStreamFilter *bsf_filter = av_bsf_get_by_name(bsf_name);
        if (!bsf_filter) {
            fprintf(stderr, "vdec: BSF %s not found\n", bsf_name);
            return -1;
        }
        if (av_bsf_alloc(bsf_filter, &ctx->bsf) < 0 ||
            avcodec_parameters_copy(ctx->bsf->par_in, stream->codecpar) < 0) {
            fprintf(stderr, "vdec: BSF alloc failed\n");
            return -1;
        }
        ctx->bsf->time_base_in = stream->time_base;
        if (av_bsf_init(ctx->bsf) < 0) {
            fprintf(stderr, "vdec: BSF init failed\n");
            return -1;
        }
    } else {
        ctx->bsf = NULL;
    }

    ctx->fd = open("/dev/video10", O_RDWR | O_NONBLOCK);
    if (ctx->fd < 0) {
        perror("vdec: cannot open /dev/video10");
        return -1;
    }

    struct v4l2_capability cap;
    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        perror("vdec: VIDIOC_QUERYCAP");
        return -1;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
        fprintf(stderr, "vdec: FATAL — not M2M multiplanar\n");
        return -1;
    }
    vlog("vdec: opened %s (%s)\n", cap.card, cap.driver);

    struct v4l2_event_subscription sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    if (xioctl(ctx->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
        perror("vdec: VIDIOC_SUBSCRIBE_EVENT");
        return -1;
    }

    if (set_output_format(ctx)    < 0) return -1;
    if (alloc_output_buffers(ctx) < 0) return -1;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("vdec: VIDIOC_STREAMON OUTPUT");
        return -1;
    }

    /* Pre-allocate capture buffers at the declared stream resolution and
     * start capture streaming. ffmpeg does this before feeding any packets.
     * The bcm2835 driver needs capture buffers queued before it will start
     * parsing the bitstream and emitting SOURCE_CHANGE. When SOURCE_CHANGE
     * fires we reinitialise at the correct decoded resolution. */
    {
        struct v4l2_format cap_fmt;
        memset(&cap_fmt, 0, sizeof(cap_fmt));
        cap_fmt.type                   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cap_fmt.fmt.pix_mp.width       = ctx->stream_width;
        cap_fmt.fmt.pix_mp.height      = ctx->stream_height;
        cap_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        cap_fmt.fmt.pix_mp.field       = V4L2_FIELD_NONE;
        cap_fmt.fmt.pix_mp.num_planes  = 1;
        xioctl(ctx->fd, VIDIOC_S_FMT, &cap_fmt);

        ctx->width        = cap_fmt.fmt.pix_mp.width;
        ctx->height       = cap_fmt.fmt.pix_mp.height;
        ctx->stride       = cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        ctx->cap_buf_size = cap_fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.count  = VDEC_CAPTURE_BUFS;
        req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(ctx->fd, VIDIOC_REQBUFS, &req);

        /* Export and queue all capture buffers */
        for (int i = 0; i < VDEC_CAPTURE_BUFS; i++) {
            struct v4l2_buffer buf;
            struct v4l2_plane  plane;
            memset(&buf,   0, sizeof(buf));
            memset(&plane, 0, sizeof(plane));
            buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            buf.memory   = V4L2_MEMORY_MMAP;
            buf.index    = (uint32_t)i;
            buf.m.planes = &plane;
            buf.length   = 1;
            if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) continue;

            struct v4l2_exportbuffer expbuf;
            memset(&expbuf, 0, sizeof(expbuf));
            expbuf.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            expbuf.index = (uint32_t)i;
            expbuf.plane = 0;
            expbuf.flags = O_RDONLY;
            if (xioctl(ctx->fd, VIDIOC_EXPBUF, &expbuf) < 0) continue;
            ctx->cap_dmabuf_fd[i] = expbuf.fd;

            memset(&plane, 0, sizeof(plane));
            buf.m.planes = &plane;
            xioctl(ctx->fd, VIDIOC_QBUF, &buf);
        }

        enum v4l2_buf_type cap_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(ctx->fd, VIDIOC_STREAMON, &cap_type) < 0) {
            perror("vdec: VIDIOC_STREAMON CAPTURE (pre-alloc)");
        } else {
            ctx->fmt_negotiated = 1;
        }
    }

    vlog("vdec: output streaming started — waiting for SPS\n");
    return 0;
}

/* ------------------------------------------------------------------ */

void vdec_run(VdecContext *ctx)
{
    int out_buf_free[VDEC_OUTPUT_BUFS];
    for (int i = 0; i < VDEC_OUTPUT_BUFS; i++)
        out_buf_free[i] = 1;

    int eos       = 0;
    int eos_polls = 0;
    int sps_timing = 0;
    struct timespec sps_start = {0};

    struct pollfd pfd;
    pfd.fd     = ctx->fd;
    pfd.events = POLLIN | POLLPRI;

    while (1) {
        /* Reclaim OUTPUT buffers */
        {
            struct v4l2_buffer buf;
            struct v4l2_plane  plane;
            while (1) {
                memset(&buf,   0, sizeof(buf));
                memset(&plane, 0, sizeof(plane));
                buf.type     = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                buf.memory   = V4L2_MEMORY_MMAP;
                buf.m.planes = &plane;
                buf.length   = 1;
                if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) break;
                out_buf_free[buf.index] = 1;
            }
        }

        /* Feed packets */
        if (!eos) {
            for (int i = 0; i < VDEC_OUTPUT_BUFS; i++) {
                if (!out_buf_free[i]) continue;

                void *item = NULL;
                if (!queue_pop(ctx->packet_queue, &item)) {
                    eos = 1;
                    break;
                }
                AVPacket *raw_pkt = (AVPacket *)item;

                if (ctx->bsf) {
                    if (av_bsf_send_packet(ctx->bsf, raw_pkt) < 0) {
                        av_packet_free(&raw_pkt);
                        continue;
                    }
                    av_packet_free(&raw_pkt);

                    AVPacket *out_pkt = av_packet_alloc();
                    if (!out_pkt) continue;

                    if (av_bsf_receive_packet(ctx->bsf, out_pkt) < 0) {
                        av_packet_free(&out_pkt);
                        continue;
                    }

                    out_buf_free[i] = 0;
                    int err = feed_packet(ctx, i, out_pkt);
                    av_packet_free(&out_pkt);
                    if (err < 0) out_buf_free[i] = 1;
                } else {
                    out_buf_free[i] = 0;
                    int err = feed_packet(ctx, i, raw_pkt);
                    av_packet_free(&raw_pkt);
                    if (err < 0) out_buf_free[i] = 1;
                }
            }
        }

        /* Poll */
        int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("vdec: poll");
            break;
        }

        /* Handle SOURCE_CHANGE */
        if (pfd.revents & POLLPRI) {
            struct v4l2_event evt;
            memset(&evt, 0, sizeof(evt));
            if (xioctl(ctx->fd, VIDIOC_DQEVENT, &evt) == 0) {
                if (evt.type == V4L2_EVENT_SOURCE_CHANGE) {
                    sps_timing = 0;
                    if (handle_source_change(ctx) < 0) {
                        fprintf(stderr, "vdec: source change failed\n");
                        break;
                    }
                }
            }
        }

        /* Dequeue decoded frames */
        if ((pfd.revents & POLLIN) && ctx->fmt_negotiated) {
            while (1) {
                struct v4l2_buffer buf;
                struct v4l2_plane  plane;
                memset(&buf,   0, sizeof(buf));
                memset(&plane, 0, sizeof(plane));
                buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                buf.memory   = V4L2_MEMORY_MMAP;
                buf.m.planes = &plane;
                buf.length   = 1;

                if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) < 0) break;

                DecodedFrame *frame = malloc(sizeof(DecodedFrame));
                if (!frame) {
                    xioctl(ctx->fd, VIDIOC_QBUF, &buf);
                    continue;
                }

                frame->dmabuf_fd  = ctx->cap_dmabuf_fd[buf.index];
                frame->buf_index  = (int)buf.index;
                frame->width      = ctx->stream_width;   /* visible width */
                frame->stride     = ctx->stride;          /* bytesperline from driver */
                frame->height     = ctx->height;
                frame->src_height = ctx->orig_height;
                frame->sar_num    = ctx->sar_num;
                frame->sar_den    = ctx->sar_den;
                frame->pts_us     = (int64_t)buf.timestamp.tv_sec  * 1000000LL
                                  + (int64_t)buf.timestamp.tv_usec;

                if (!queue_push(ctx->frame_queue, frame)) {
                    free(frame);
                    goto done;
                }
            }
        }

        if (eos) {
            if (++eos_polls > 20) break;
        }

        /* If SOURCE_CHANGE never arrives after 10 real seconds, the
         * hardware can't decode this stream — bail out cleanly. */
        if (!ctx->fmt_negotiated) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (!sps_timing) {
                sps_start  = now;
                sps_timing = 1;
            } else if ((now.tv_sec - sps_start.tv_sec) >= 10) {
                fprintf(stderr, "vdec: hardware decoder timed out — unsupported codec or profile\n");
                break;
            }
        } else {
            sps_timing = 0;
        }
    }

done:
    queue_close(ctx->frame_queue);
}

/* ------------------------------------------------------------------ */

void vdec_requeue_frame(VdecContext *ctx, DecodedFrame *frame)
{
    struct v4l2_buffer buf;
    struct v4l2_plane  plane;
    memset(&buf,   0, sizeof(buf));
    memset(&plane, 0, sizeof(plane));

    buf.type     = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory   = V4L2_MEMORY_MMAP;
    buf.index    = (uint32_t)frame->buf_index;
    buf.m.planes = &plane;
    buf.length   = 1;

    xioctl(ctx->fd, VIDIOC_QBUF, &buf);
    free(frame);
}

/* ------------------------------------------------------------------ */

int vdec_flush(VdecContext *ctx)
{
    /*
     * Called after a seek, before restarting the vdec thread.
     * If we already know the frame dimensions (from a previous SOURCE_CHANGE),
     * re-setup the capture buffers at the same size — the bcm2835 decoder
     * doesn't re-emit SOURCE_CHANGE when the resolution hasn't changed.
     * If dimensions aren't known yet, reset fmt_negotiated and wait for
     * SOURCE_CHANGE as normal.
     */
    enum v4l2_buf_type type;

    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);

    /* Free existing capture buffers */
    for (int i = 0; i < VDEC_CAPTURE_BUFS; i++) {
        if (ctx->cap_dmabuf_fd[i] > 0) {
            close(ctx->cap_dmabuf_fd[i]);
            ctx->cap_dmabuf_fd[i] = -1;
        }
    }
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = 0;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(ctx->fd, VIDIOC_REQBUFS, &req);

    /* Flush BSF (may be NULL for Annex B streams like HLS/TS) */
    if (ctx->bsf)
        av_bsf_flush(ctx->bsf);

    /* Restart output streaming */
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("vdec: VIDIOC_STREAMON OUTPUT after flush");
        return -1;
    }

    /* If we already know the dimensions, re-setup capture now.
     * The decoder won't re-fire SOURCE_CHANGE for the same resolution.
     * Must clear fmt_negotiated first or the guard in handle_source_change
     * will return immediately without allocating anything. */
    ctx->fmt_negotiated = 0;
    if (ctx->width > 0 && ctx->height > 0) {
        if (handle_source_change(ctx) < 0) {
            fprintf(stderr, "vdec: flush re-setup failed\n");
            return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */

void vdec_close(VdecContext *ctx)
{
    stream_off(ctx);

    if (ctx->bsf) {
        av_bsf_free(&ctx->bsf);
        ctx->bsf = NULL;
    }

    for (int i = 0; i < VDEC_CAPTURE_BUFS; i++) {
        if (ctx->cap_dmabuf_fd[i] > 0) {
            close(ctx->cap_dmabuf_fd[i]);
            ctx->cap_dmabuf_fd[i] = -1;
        }
    }

    for (int i = 0; i < VDEC_OUTPUT_BUFS; i++) {
        if (ctx->out_mem[i] && ctx->out_mem[i] != MAP_FAILED) {
            munmap(ctx->out_mem[i], ctx->out_buf_size);
            ctx->out_mem[i] = NULL;
        }
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}
