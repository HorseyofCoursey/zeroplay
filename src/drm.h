#ifndef DRM_H
#define DRM_H
#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "vdec.h"

#define DRM_MAX_OUTPUTS 4

typedef struct {
    uint32_t connector_id;
    uint32_t crtc_id;
    uint32_t plane_id;

    /* DRM property IDs */
    uint32_t prop_crtc_id;
    uint32_t prop_active;
    uint32_t prop_mode_id;
    uint32_t prop_fb_id;
    uint32_t prop_crtc_x;
    uint32_t prop_crtc_y;
    uint32_t prop_crtc_w;
    uint32_t prop_crtc_h;
    uint32_t prop_src_x;
    uint32_t prop_src_y;
    uint32_t prop_src_w;
    uint32_t prop_src_h;

    uint32_t mode_blob_id;
    uint32_t mode_w;
    uint32_t mode_h;

    /* Aspect-correct destination rectangle */
    uint32_t dest_x;
    uint32_t dest_y;
    uint32_t dest_w;
    uint32_t dest_h;

    /* Last video frame dimensions — used to detect size changes */
    uint32_t vid_w;
    uint32_t vid_h;

    /* Currently displayed DRM pixel format (0 = none yet) */
    uint32_t current_format;

    /* Previous framebuffer — freed on next present */
    uint32_t prev_fb_id;
    uint32_t prev_gem_handle;
    int      prev_is_dumb;    /* 1 = dumb buffer, 0 = DMABUF-imported GEM */

    int      first_frame;

    /* Saved CRTC state for restore on close */
    drmModeCrtcPtr prev_crtc;
} DrmOutput;

typedef struct {
    int        fd;
    DrmOutput  outputs[DRM_MAX_OUTPUTS];
    int        output_count;
} DrmContext;

int  drm_open(DrmContext *ctx);

/* Present a decoded video frame (NV12, DMABUF) */
int  drm_present(DrmContext *ctx, int output_idx, DecodedFrame *frame);

/* Present a still image (XRGB8888 pixels, CPU memory) */
int  drm_present_image(DrmContext *ctx, int output_idx,
                        uint8_t *pixels, int width, int height, int stride);

void drm_close(DrmContext *ctx);

#endif
