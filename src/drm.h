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

    drmModeCrtcPtr prev_mode;

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

    /* Aspect-correct destination rectangle — computed once per frame size */
    uint32_t dest_x;
    uint32_t dest_y;
    uint32_t dest_w;
    uint32_t dest_h;

    uint32_t prev_fb_id;
    uint32_t prev_gem_handle;
    int      first_frame;
} DrmOutput;

typedef struct {
    int        fd;
    DrmOutput  outputs[DRM_MAX_OUTPUTS];
    int        output_count;
} DrmContext;

int  drm_open(DrmContext *ctx);
int  drm_present(DrmContext *ctx, int output_idx, DecodedFrame *frame);
void drm_close(DrmContext *ctx);

#endif
