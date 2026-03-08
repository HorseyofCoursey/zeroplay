#include "log.h"
#include "drm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <drm/drm_fourcc.h>

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
 *
 * Fits the video's display aspect ratio into the display,
 * centering with black bars as needed.
 */
static void calc_dest_rect(uint32_t disp_w, uint32_t disp_h,
                            uint32_t vid_w,  uint32_t vid_h,
                            int sar_num, int sar_den,
                            uint32_t *out_x, uint32_t *out_y,
                            uint32_t *out_w, uint32_t *out_h)
{
    /* Sanitise SAR — if missing or invalid default to square pixels */
    if (sar_num <= 0 || sar_den <= 0) {
        sar_num = 1;
        sar_den = 1;
    }

    /* Display aspect ratio numerator/denominator */
    uint64_t dar_w = (uint64_t)vid_w * (uint32_t)sar_num;
    uint64_t dar_h = (uint64_t)vid_h * (uint32_t)sar_den;

    /* Try fitting by width first */
    uint64_t fit_h = (uint64_t)disp_w * dar_h / dar_w;

    if (fit_h <= disp_h) {
        /* Letterbox — bars on top and bottom */
        *out_w = disp_w;
        *out_h = (uint32_t)fit_h & ~1u;   /* keep even */
        *out_x = 0;
        *out_y = (disp_h - *out_h) / 2;
    } else {
        /* Pillarbox — bars on left and right */
        uint64_t fit_w = (uint64_t)disp_h * dar_w / dar_h;
        *out_w = (uint32_t)fit_w & ~1u;   /* keep even */
        *out_h = disp_h;
        *out_x = (disp_w - *out_w) / 2;
        *out_y = 0;
    }

    vlog("drm: dest rect %ux%u+%u+%u (display %ux%u DAR %llu:%llu)\n",
            *out_w, *out_h, *out_x, *out_y,
            disp_w, disp_h,
            (unsigned long long)dar_w, (unsigned long long)dar_h);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int drm_open(DrmContext *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->first_frame = 1;
    ctx->fd = -1;

    drmDevicePtr devices[4];
    const int device_max = sizeof(devices) / sizeof(devices[0]);

    const int device_count = drmGetDevices2(0, devices, device_max);

    for (int i = 0; i < device_count; i++) {
        const drmDevicePtr device_ptr = devices[i];

        if ((device_ptr->available_nodes & (1 << DRM_NODE_PRIMARY)) == 0)
        {
            continue;
        }

        ctx->fd = open(device_ptr->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
        if (ctx->fd < 0) {perror("drm: open"); continue; }

        break;
    }

    if (ctx->fd < 0) { fprintf(stderr, "drm: no suitable card fount\n"); return -1; }

    if (drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
        fprintf(stderr, "drm: no atomic\n"); return -1;
    }
    if (drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
        fprintf(stderr, "drm: no universal planes\n"); return -1;
    }

    drmModeRes *res = drmModeGetResources(ctx->fd);
    if (!res) { fprintf(stderr, "drm: no resources\n"); return -1; }

    /* Find connected connector */
    drmModeConnector *connector = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c =
            drmModeGetConnector(ctx->fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            connector = c;
            ctx->connector_id = c->connector_id;
            break;
        }
        if (c) drmModeFreeConnector(c);
    }
    if (!connector) {
        fprintf(stderr, "drm: no connected connector\n");
        drmModeFreeResources(res);
        return -1;
    }
    vlog("drm: connector %u\n", ctx->connector_id);

    /* Use preferred mode (index 0) */
    drmModeModeInfo mode = connector->modes[0];
    ctx->mode_w = mode.hdisplay;
    ctx->mode_h = mode.vdisplay;
    vlog("drm: mode %ux%u @ %uHz\n",
            ctx->mode_w, ctx->mode_h, mode.vrefresh);

    if (drmModeCreatePropertyBlob(ctx->fd, &mode, sizeof(mode),
                                  &ctx->mode_blob_id) < 0) {
        fprintf(stderr, "drm: mode blob failed\n");
        drmModeFreeConnector(connector);
        drmModeFreeResources(res);
        return -1;
    }

    /* Find CRTC — prefer encoder's active one */
    ctx->crtc_id = 0;
    int crtc_idx = -1;

    if (connector->encoder_id) {
        drmModeEncoder *enc =
            drmModeGetEncoder(ctx->fd, connector->encoder_id);
        if (enc) {
            if (enc->crtc_id) {
                ctx->crtc_id = enc->crtc_id;
                for (int i = 0; i < res->count_crtcs; i++) {
                    if (res->crtcs[i] == ctx->crtc_id) {
                        crtc_idx = i; break;
                    }
                }
            }
            if (!ctx->crtc_id) {
                for (int i = 0; i < res->count_crtcs; i++) {
                    if (enc->possible_crtcs & (1 << i)) {
                        ctx->crtc_id = res->crtcs[i];
                        crtc_idx = i; break;
                    }
                }
            }
            drmModeFreeEncoder(enc);
        }
    }

    if (!ctx->crtc_id) {
        for (int e = 0; e < connector->count_encoders && !ctx->crtc_id; e++) {
            drmModeEncoder *enc =
                drmModeGetEncoder(ctx->fd, connector->encoders[e]);
            if (!enc) continue;
            for (int i = 0; i < res->count_crtcs; i++) {
                if (enc->possible_crtcs & (1 << i)) {
                    ctx->crtc_id = res->crtcs[i];
                    crtc_idx = i; break;
                }
            }
            drmModeFreeEncoder(enc);
        }
    }

    drmModeFreeConnector(connector);
    drmModeFreeResources(res);

    if (!ctx->crtc_id || crtc_idx < 0) {
        fprintf(stderr, "drm: no CRTC found\n"); return -1;
    }
    vlog("drm: crtc %u (index %d)\n", ctx->crtc_id, crtc_idx);

    /* Find NV12-capable plane compatible with our CRTC */
    ctx->plane_id = 0;
    drmModePlaneRes *pr = drmModeGetPlaneResources(ctx->fd);
    if (!pr) { fprintf(stderr, "drm: no plane resources\n"); return -1; }

    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *p = drmModeGetPlane(ctx->fd, pr->planes[i]);
        if (!p) continue;
        int crtc_ok = (p->possible_crtcs & (1u << crtc_idx)) != 0;
        int nv12_ok = plane_supports_nv12(ctx->fd, p->plane_id);
        if (crtc_ok && nv12_ok && !ctx->plane_id)
            ctx->plane_id = p->plane_id;
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);

    if (!ctx->plane_id) {
        fprintf(stderr, "drm: no NV12 plane for crtc %u\n", ctx->crtc_id);
        return -1;
    }
    vlog("drm: plane %u\n", ctx->plane_id);

    /* Look up property IDs */
    ctx->prop_crtc_id = get_property_id(ctx->fd, ctx->connector_id,
                                         DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    ctx->prop_active  = get_property_id(ctx->fd, ctx->crtc_id,
                                         DRM_MODE_OBJECT_CRTC, "ACTIVE");
    ctx->prop_mode_id = get_property_id(ctx->fd, ctx->crtc_id,
                                         DRM_MODE_OBJECT_CRTC, "MODE_ID");
    ctx->prop_fb_id   = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "FB_ID");
    ctx->prop_crtc_x  = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "CRTC_X");
    ctx->prop_crtc_y  = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    ctx->prop_crtc_w  = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "CRTC_W");
    ctx->prop_crtc_h  = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "CRTC_H");
    ctx->prop_src_x   = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "SRC_X");
    ctx->prop_src_y   = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "SRC_Y");
    ctx->prop_src_w   = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "SRC_W");
    ctx->prop_src_h   = get_property_id(ctx->fd, ctx->plane_id,
                                         DRM_MODE_OBJECT_PLANE, "SRC_H");

    if (!ctx->prop_crtc_id || !ctx->prop_active || !ctx->prop_mode_id ||
        !ctx->prop_fb_id   || !ctx->prop_src_w  || !ctx->prop_crtc_w) {
        fprintf(stderr, "drm: missing required properties\n");
        return -1;
    }

    vlog("drm: ready — crtc=%u plane=%u connector=%u\n",
            ctx->crtc_id, ctx->plane_id, ctx->connector_id);
    return 0;
}

/* ------------------------------------------------------------------ */

int drm_present(DrmContext *ctx, DecodedFrame *frame)
{
    uint32_t gem_handle = 0;
    if (drmPrimeFDToHandle(ctx->fd, frame->dmabuf_fd, &gem_handle) < 0) {
        perror("drm: drmPrimeFDToHandle");
        return -1;
    }

    uint32_t stride  = frame->stride;
    uint32_t y_size  = stride * frame->height;

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

    /* Calculate aspect-correct dest rect once on first frame */
    if (ctx->first_frame) {
        calc_dest_rect(ctx->mode_w, ctx->mode_h,
                       frame->width, frame->src_height,
                       frame->sar_num, frame->sar_den,
                       &ctx->dest_x, &ctx->dest_y,
                       &ctx->dest_w, &ctx->dest_h);
    }

    drmModeAtomicReq *areq = drmModeAtomicAlloc();
    if (!areq) {
        drmModeRmFB(ctx->fd, fb_id);
        drmCloseBufferHandle(ctx->fd, gem_handle);
        return -1;
    }

    /* First frame: full modeset to activate CRTC */
    if (ctx->first_frame) {
        drmModeAtomicAddProperty(areq, ctx->crtc_id,
                                 ctx->prop_active,  1);
        drmModeAtomicAddProperty(areq, ctx->crtc_id,
                                 ctx->prop_mode_id, ctx->mode_blob_id);
        drmModeAtomicAddProperty(areq, ctx->connector_id,
                                 ctx->prop_crtc_id, ctx->crtc_id);
    }

    /* Source — full decoded frame in 16.16 fixed point */
    drmModeAtomicAddProperty(areq, ctx->plane_id, ctx->prop_src_x, 0);
    drmModeAtomicAddProperty(areq, ctx->plane_id, ctx->prop_src_y, 0);
    drmModeAtomicAddProperty(areq, ctx->plane_id,
                             ctx->prop_src_w, (uint64_t)frame->width      << 16);
    drmModeAtomicAddProperty(areq, ctx->plane_id,
                             ctx->prop_src_h, (uint64_t)frame->src_height << 16);

    /* Destination — aspect-correct rectangle */
    drmModeAtomicAddProperty(areq, ctx->plane_id,
                             ctx->prop_crtc_x, ctx->dest_x);
    drmModeAtomicAddProperty(areq, ctx->plane_id,
                             ctx->prop_crtc_y, ctx->dest_y);
    drmModeAtomicAddProperty(areq, ctx->plane_id,
                             ctx->prop_crtc_w, ctx->dest_w);
    drmModeAtomicAddProperty(areq, ctx->plane_id,
                             ctx->prop_crtc_h, ctx->dest_h);

    drmModeAtomicAddProperty(areq, ctx->plane_id,
                             ctx->prop_fb_id,   fb_id);
    drmModeAtomicAddProperty(areq, ctx->plane_id,
                             ctx->prop_crtc_id, ctx->crtc_id);

    uint32_t flags = ctx->first_frame ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;
    ret = drmModeAtomicCommit(ctx->fd, areq, flags, NULL);
    drmModeAtomicFree(areq);

    if (ret < 0) {
        perror("drm: atomic commit failed");
        drmModeRmFB(ctx->fd, fb_id);
        drmCloseBufferHandle(ctx->fd, gem_handle);
        return -1;
    }

    ctx->first_frame = 0;

    if (ctx->prev_fb_id) {
        drmModeRmFB(ctx->fd, ctx->prev_fb_id);
        ctx->prev_fb_id = 0;
    }
    if (ctx->prev_gem_handle) {
        drmCloseBufferHandle(ctx->fd, ctx->prev_gem_handle);
        ctx->prev_gem_handle = 0;
    }

    ctx->prev_fb_id      = fb_id;
    ctx->prev_gem_handle = gem_handle;
    return 0;
}

/* ------------------------------------------------------------------ */

void drm_close(DrmContext *ctx)
{
    if (ctx->prev_fb_id) {
        drmModeRmFB(ctx->fd, ctx->prev_fb_id);
        ctx->prev_fb_id = 0;
    }
    if (ctx->prev_gem_handle) {
        drmCloseBufferHandle(ctx->fd, ctx->prev_gem_handle);
        ctx->prev_gem_handle = 0;
    }
    if (ctx->mode_blob_id) {
        drmModeDestroyPropertyBlob(ctx->fd, ctx->mode_blob_id);
        ctx->mode_blob_id = 0;
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}
