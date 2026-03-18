#include "log.h"
#include "drm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static uint32_t get_property_id(int fd, uint32_t obj_id,
                                uint32_t obj_type, const char *name)
{
    drmModeObjectProperties *props =
        drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props) return 0;

    uint32_t result = 0;
    for (uint32_t i = 0; i < props->count_props; i++) {
        drmModePropertyRes *prop =
            drmModeGetProperty(fd, props->props[i]);
        if (prop) {
            if (strcmp(prop->name, name) == 0)
                result = prop->prop_id;
            drmModeFreeProperty(prop);
        }
        if (result) break;
    }
    drmModeFreeObjectProperties(props);
    return result;
}

static int plane_supports_nv12(int fd, uint32_t plane_id)
{
    drmModePlane *plane = drmModeGetPlane(fd, plane_id);
    if (!plane) return 0;
    int found = 0;
    for (uint32_t i = 0; i < plane->count_formats; i++) {
        if (plane->formats[i] == DRM_FORMAT_NV12) { found = 1; break; }
    }
    drmModeFreePlane(plane);
    return found;
}

/*
 * Compute letterbox/pillarbox destination rectangle.
 * Fits the video's display aspect ratio into the display,
 * centering with black bars as needed.
 */
static void calc_dest_rect(uint32_t disp_w, uint32_t disp_h,
                            uint32_t vid_w,  uint32_t vid_h,
                            int sar_num, int sar_den,
                            uint32_t *out_x, uint32_t *out_y,
                            uint32_t *out_w, uint32_t *out_h)
{
    if (sar_num <= 0 || sar_den <= 0) { sar_num = 1; sar_den = 1; }

    uint64_t dar_w = (uint64_t)vid_w * (uint32_t)sar_num;
    uint64_t dar_h = (uint64_t)vid_h * (uint32_t)sar_den;
    uint64_t fit_h = (uint64_t)disp_w * dar_h / dar_w;

    if (fit_h <= disp_h) {
        *out_w = disp_w;
        *out_h = (uint32_t)fit_h & ~1u;
        *out_x = 0;
        *out_y = (disp_h - *out_h) / 2;
    } else {
        uint64_t fit_w = (uint64_t)disp_h * dar_w / dar_h;
        *out_w = (uint32_t)fit_w & ~1u;
        *out_h = disp_h;
        *out_x = (disp_w - *out_w) / 2;
        *out_y = 0;
    }

    vlog("drm: dest rect %ux%u+%u+%u (display %ux%u DAR %llu:%llu)\n",
            *out_w, *out_h, *out_x, *out_y, disp_w, disp_h,
            (unsigned long long)dar_w, (unsigned long long)dar_h);
}

/*
 * Find an unclaimed CRTC for a connector.
 * claimed_crtcs is a bitmask of already-used CRTC indices.
 */
static int find_crtc(int fd, drmModeRes *res, drmModeConnector *connector,
                     uint32_t claimed_crtcs,
                     uint32_t *crtc_id_out, int *crtc_idx_out)
{
    if (connector->encoder_id) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, connector->encoder_id);
        if (enc) {
            if (enc->crtc_id) {
                for (int i = 0; i < res->count_crtcs; i++) {
                    if (res->crtcs[i] == enc->crtc_id &&
                        !(claimed_crtcs & (1u << i))) {
                        *crtc_id_out  = enc->crtc_id;
                        *crtc_idx_out = i;
                        drmModeFreeEncoder(enc);
                        return 0;
                    }
                }
            }
            for (int i = 0; i < res->count_crtcs; i++) {
                if ((enc->possible_crtcs & (1u << i)) &&
                    !(claimed_crtcs & (1u << i))) {
                    *crtc_id_out  = res->crtcs[i];
                    *crtc_idx_out = i;
                    drmModeFreeEncoder(enc);
                    return 0;
                }
            }
            drmModeFreeEncoder(enc);
        }
    }

    for (int e = 0; e < connector->count_encoders; e++) {
        drmModeEncoder *enc = drmModeGetEncoder(fd, connector->encoders[e]);
        if (!enc) continue;
        for (int i = 0; i < res->count_crtcs; i++) {
            if ((enc->possible_crtcs & (1u << i)) &&
                !(claimed_crtcs & (1u << i))) {
                *crtc_id_out  = res->crtcs[i];
                *crtc_idx_out = i;
                drmModeFreeEncoder(enc);
                return 0;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return -1;
}

/* Release a GEM handle — method depends on whether it's a dumb buffer */
static void release_gem(int fd, uint32_t gem_handle, int is_dumb)
{
    if (!gem_handle) return;
    if (is_dumb) {
        struct drm_mode_destroy_dumb dreq = { .handle = gem_handle };
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    } else {
        drmCloseBufferHandle(fd, gem_handle);
    }
}

/* Release previous framebuffer and GEM handle for an output */
static void release_prev(int fd, DrmOutput *out)
{
    if (out->prev_fb_id) {
        drmModeRmFB(fd, out->prev_fb_id);
        out->prev_fb_id = 0;
    }
    if (out->prev_gem_handle) {
        release_gem(fd, out->prev_gem_handle, out->prev_is_dumb);
        out->prev_gem_handle = 0;
        out->prev_is_dumb    = 0;
    }
}

/*
 * Perform an atomic commit.
 * need_modeset=1 adds ACTIVE/MODE_ID/CRTC_ID properties and sets
 * DRM_MODE_ATOMIC_ALLOW_MODESET.
 */
static int do_atomic_commit(int fd, DrmOutput *out,
                             uint32_t fb_id,
                             uint32_t src_w, uint32_t src_h,
                             int need_modeset)
{
    drmModeAtomicReq *areq = drmModeAtomicAlloc();
    if (!areq) return -1;

    if (need_modeset) {
        drmModeAtomicAddProperty(areq, out->crtc_id,
                                 out->prop_active,  1);
        drmModeAtomicAddProperty(areq, out->crtc_id,
                                 out->prop_mode_id, out->mode_blob_id);
        drmModeAtomicAddProperty(areq, out->connector_id,
                                 out->prop_crtc_id, out->crtc_id);
    }

    drmModeAtomicAddProperty(areq, out->plane_id, out->prop_src_x, 0);
    drmModeAtomicAddProperty(areq, out->plane_id, out->prop_src_y, 0);
    drmModeAtomicAddProperty(areq, out->plane_id,
                             out->prop_src_w, (uint64_t)src_w << 16);
    drmModeAtomicAddProperty(areq, out->plane_id,
                             out->prop_src_h, (uint64_t)src_h << 16);
    drmModeAtomicAddProperty(areq, out->plane_id,
                             out->prop_crtc_x, out->dest_x);
    drmModeAtomicAddProperty(areq, out->plane_id,
                             out->prop_crtc_y, out->dest_y);
    drmModeAtomicAddProperty(areq, out->plane_id,
                             out->prop_crtc_w, out->dest_w);
    drmModeAtomicAddProperty(areq, out->plane_id,
                             out->prop_crtc_h, out->dest_h);
    drmModeAtomicAddProperty(areq, out->plane_id,
                             out->prop_fb_id,   fb_id);
    drmModeAtomicAddProperty(areq, out->plane_id,
                             out->prop_crtc_id, out->crtc_id);

    uint32_t flags = need_modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;
    int ret = drmModeAtomicCommit(fd, areq, flags, NULL);
    drmModeAtomicFree(areq);
    return ret;
}

/*
 * Set up one DrmOutput for a connected connector.
 */
static int setup_output(int fd, drmModeRes *res,
                        drmModeConnector *connector,
                        uint32_t *claimed_crtcs,
                        DrmOutput *out)
{
    memset(out, 0, sizeof(*out));
    out->first_frame  = 1;
    out->connector_id = connector->connector_id;

    drmModeModeInfo mode = connector->modes[0];
    out->mode_w = mode.hdisplay;
    out->mode_h = mode.vdisplay;
    vlog("drm: connector %u mode %ux%u @ %uHz\n",
            out->connector_id, out->mode_w, out->mode_h, mode.vrefresh);

    if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode),
                                  &out->mode_blob_id) < 0) {
        fprintf(stderr, "drm: mode blob failed for connector %u\n",
                out->connector_id);
        return -1;
    }

    uint32_t crtc_id  = 0;
    int      crtc_idx = -1;
    if (find_crtc(fd, res, connector, *claimed_crtcs,
                  &crtc_id, &crtc_idx) < 0) {
        fprintf(stderr, "drm: no CRTC available for connector %u\n",
                out->connector_id);
        drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
        return -1;
    }
    out->crtc_id = crtc_id;
    *claimed_crtcs |= (1u << crtc_idx);

    /* Save current CRTC state for restore on exit */
    out->prev_crtc = drmModeGetCrtc(fd, crtc_id);

    vlog("drm: connector %u -> crtc %u (idx %d)\n",
            out->connector_id, out->crtc_id, crtc_idx);

    /* Primary NV12-capable plane for this CRTC.
     * Per 6by9 (RPi engineer): primary plane is always the lowest-numbered
     * plane for a given CRTC, one guaranteed per CRTC. */
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    if (!pr) {
        fprintf(stderr, "drm: no plane resources\n");
        drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
        return -1;
    }
    for (uint32_t i = 0; i < pr->count_planes && !out->plane_id; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
        if (!p) continue;
        if ((p->possible_crtcs & (1u << crtc_idx)) &&
            plane_supports_nv12(fd, p->plane_id))
            out->plane_id = p->plane_id;
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);

    if (!out->plane_id) {
        fprintf(stderr, "drm: no NV12 plane for crtc %u\n", out->crtc_id);
        drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
        return -1;
    }

    out->prop_crtc_id = get_property_id(fd, out->connector_id,
                                         DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    out->prop_active  = get_property_id(fd, out->crtc_id,
                                         DRM_MODE_OBJECT_CRTC, "ACTIVE");
    out->prop_mode_id = get_property_id(fd, out->crtc_id,
                                         DRM_MODE_OBJECT_CRTC, "MODE_ID");
    out->prop_fb_id   = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "FB_ID");
    out->prop_crtc_x  = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "CRTC_X");
    out->prop_crtc_y  = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    out->prop_crtc_w  = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "CRTC_W");
    out->prop_crtc_h  = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "CRTC_H");
    out->prop_src_x   = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "SRC_X");
    out->prop_src_y   = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "SRC_Y");
    out->prop_src_w   = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "SRC_W");
    out->prop_src_h   = get_property_id(fd, out->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "SRC_H");

    if (!out->prop_crtc_id || !out->prop_active || !out->prop_mode_id ||
        !out->prop_fb_id   || !out->prop_src_w  || !out->prop_crtc_w) {
        fprintf(stderr, "drm: missing required properties for connector %u\n",
                out->connector_id);
        drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
        return -1;
    }

    vlog("drm: output ready — connector=%u crtc=%u plane=%u\n",
            out->connector_id, out->crtc_id, out->plane_id);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int drm_open(DrmContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    drmDevicePtr devices[4];
    const int device_max   = sizeof(devices) / sizeof(devices[0]);
    const int device_count = drmGetDevices2(0, devices, device_max);
    for (int i = 0; i < device_count; i++) {
        if (!(devices[i]->available_nodes & (1 << DRM_NODE_PRIMARY)))
            continue;
        ctx->fd = open(devices[i]->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
        if (ctx->fd >= 0) break;
    }
    drmFreeDevices(devices, device_count);

    if (ctx->fd < 0) {
        fprintf(stderr, "drm: no suitable DRM device found\n");
        return -1;
    }

    if (drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
        fprintf(stderr, "drm: no atomic\n"); return -1;
    }
    if (drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        fprintf(stderr, "drm: no universal planes\n"); return -1;
    }

    drmModeRes *res = drmModeGetResources(ctx->fd);
    if (!res) { fprintf(stderr, "drm: no resources\n"); return -1; }

    uint32_t claimed_crtcs = 0;
    for (int i = 0; i < res->count_connectors &&
                    ctx->output_count < DRM_MAX_OUTPUTS; i++) {
        drmModeConnector *c =
            drmModeGetConnector(ctx->fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0) {
            drmModeFreeConnector(c);
            continue;
        }
        if (setup_output(ctx->fd, res, c, &claimed_crtcs,
                         &ctx->outputs[ctx->output_count]) == 0)
            ctx->output_count++;
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);

    if (ctx->output_count == 0) {
        fprintf(stderr, "drm: no connected outputs found\n");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    vlog("drm: %d output(s) found\n", ctx->output_count);
    return 0;
}

/* ------------------------------------------------------------------ */

int drm_present(DrmContext *ctx, int output_idx, DecodedFrame *frame)
{
    if (output_idx < 0 || output_idx >= ctx->output_count) {
        fprintf(stderr, "drm: invalid output index %d\n", output_idx);
        return -1;
    }
    DrmOutput *out = &ctx->outputs[output_idx];

    uint32_t gem_handle = 0;
    if (drmPrimeFDToHandle(ctx->fd, frame->dmabuf_fd, &gem_handle) < 0) {
        perror("drm: drmPrimeFDToHandle");
        return -1;
    }

    uint32_t stride = frame->stride;
    uint32_t y_size = stride * frame->height;

    uint32_t handles[4]   = { gem_handle, gem_handle, 0, 0 };
    uint32_t pitches[4]   = { stride,     stride,     0, 0 };
    uint32_t offsets[4]   = { 0,          y_size,     0, 0 };
    uint64_t modifiers[4] = { DRM_FORMAT_MOD_LINEAR,
                               DRM_FORMAT_MOD_LINEAR, 0, 0 };

    uint32_t fb_id = 0;
    int ret = drmModeAddFB2WithModifiers(ctx->fd,
                                         frame->stride, frame->height,
                                         DRM_FORMAT_NV12,
                                         handles, pitches, offsets, modifiers,
                                         &fb_id, DRM_MODE_FB_MODIFIERS);
    if (ret < 0) {
        perror("drm: drmModeAddFB2WithModifiers");
        drmCloseBufferHandle(ctx->fd, gem_handle);
        return -1;
    }

    /* Recalculate dest rect on first frame or when video dimensions change */
    if (out->first_frame ||
        frame->width      != out->vid_w ||
        frame->src_height != out->vid_h) {
        calc_dest_rect(out->mode_w, out->mode_h,
                       frame->width, frame->src_height,
                       frame->sar_num, frame->sar_den,
                       &out->dest_x, &out->dest_y,
                       &out->dest_w, &out->dest_h);
        out->vid_w = frame->width;
        out->vid_h = frame->src_height;
    }

    /* Need modeset if first frame or switching from image (XRGB) to video (NV12) */
    int need_modeset = out->first_frame ||
                       (out->current_format != DRM_FORMAT_NV12);

    ret = do_atomic_commit(ctx->fd, out, fb_id,
                           frame->width, frame->src_height, need_modeset);
    if (ret < 0) {
        perror("drm: atomic commit failed");
        drmModeRmFB(ctx->fd, fb_id);
        drmCloseBufferHandle(ctx->fd, gem_handle);
        return -1;
    }

    out->first_frame    = 0;
    out->current_format = DRM_FORMAT_NV12;

    release_prev(ctx->fd, out);
    out->prev_fb_id      = fb_id;
    out->prev_gem_handle = gem_handle;
    out->prev_is_dumb    = 0;
    return 0;
}

/* ------------------------------------------------------------------ */

int drm_present_image(DrmContext *ctx, int output_idx,
                       uint8_t *pixels, int width, int height, int stride)
{
    if (output_idx < 0 || output_idx >= ctx->output_count) {
        fprintf(stderr, "drm: invalid output index %d\n", output_idx);
        return -1;
    }
    DrmOutput *out = &ctx->outputs[output_idx];

    /* Create a dumb buffer (CPU-writable, 32bpp XRGB8888) */
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width  = (uint32_t)width;
    creq.height = (uint32_t)height;
    creq.bpp    = 32;

    if (drmIoctl(ctx->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("drm: DRM_IOCTL_MODE_CREATE_DUMB");
        return -1;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (drmIoctl(ctx->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        perror("drm: DRM_IOCTL_MODE_MAP_DUMB");
        goto destroy_dumb;
    }

    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, ctx->fd, mreq.offset);
    if (map == MAP_FAILED) {
        perror("drm: mmap dumb buffer");
        goto destroy_dumb;
    }

    /* Copy rows, accounting for dumb buffer pitch vs our stride */
    for (int y = 0; y < height; y++) {
        memcpy((uint8_t *)map + (size_t)y * creq.pitch,
               pixels          + (size_t)y * stride,
               (size_t)width * 4);
    }
    munmap(map, creq.size);

    /* Register framebuffer */
    uint32_t handles[4] = { creq.handle, 0, 0, 0 };
    uint32_t pitches[4] = { creq.pitch,  0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t fb_id = 0;
    if (drmModeAddFB2(ctx->fd, (uint32_t)width, (uint32_t)height,
                       DRM_FORMAT_XRGB8888,
                       handles, pitches, offsets, &fb_id, 0) < 0) {
        perror("drm: drmModeAddFB2 image");
        goto destroy_dumb;
    }

    /* Recalculate dest rect for image dimensions (SAR is always 1:1) */
    calc_dest_rect(out->mode_w, out->mode_h,
                   (uint32_t)width, (uint32_t)height, 1, 1,
                   &out->dest_x, &out->dest_y,
                   &out->dest_w, &out->dest_h);

    /* Need modeset if first frame or switching from video (NV12) to image (XRGB) */
    int need_modeset = out->first_frame ||
                       (out->current_format != DRM_FORMAT_XRGB8888);

    if (do_atomic_commit(ctx->fd, out, fb_id,
                          (uint32_t)width, (uint32_t)height,
                          need_modeset) < 0) {
        perror("drm: atomic commit image");
        drmModeRmFB(ctx->fd, fb_id);
        goto destroy_dumb;
    }

    out->first_frame    = 0;
    out->current_format = DRM_FORMAT_XRGB8888;
    /* Force dest_rect recalculation on next video frame */
    out->vid_w = 0;
    out->vid_h = 0;

    release_prev(ctx->fd, out);
    out->prev_fb_id      = fb_id;
    out->prev_gem_handle = creq.handle;
    out->prev_is_dumb    = 1;
    return 0;

destroy_dumb: {
        struct drm_mode_destroy_dumb dreq;
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = creq.handle;
        drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
    return -1;
}

/* ------------------------------------------------------------------ */

void drm_close(DrmContext *ctx)
{
    for (int i = 0; i < ctx->output_count; i++) {
        DrmOutput *out = &ctx->outputs[i];

        release_prev(ctx->fd, out);

        if (out->mode_blob_id) {
            drmModeDestroyPropertyBlob(ctx->fd, out->mode_blob_id);
            out->mode_blob_id = 0;
        }

        /* Restore previous CRTC state */
        if (out->prev_crtc) {
            drmModeSetCrtc(ctx->fd,
                           out->prev_crtc->crtc_id,
                           out->prev_crtc->buffer_id,
                           out->prev_crtc->x,
                           out->prev_crtc->y,
                           &out->connector_id, 1,
                           &out->prev_crtc->mode);
            drmModeFreeCrtc(out->prev_crtc);
            out->prev_crtc = NULL;
        }
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}
