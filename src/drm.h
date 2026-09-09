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

    /* Video plane property IDs */
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

    /* DRM_FORMAT_NV12 (scaled path) or DRM_FORMAT_RGB565 (SPI/DBI crop path) */
    uint32_t plane_format;

    /* SPI/DBI fit mode: a persistent panel-sized RGB565 buffer that the
     * decoded frame is aspect-fit-scaled into (letterboxed) each present. */
    uint32_t           fit_gem;
    uint32_t           fit_pitch;
    size_t             fit_size;
    void              *fit_map;
    struct SwsContext *fit_sws;
    uint32_t           fit_src_w, fit_src_h;   /* source size fit_sws was built for */
    uint32_t           fit_dx, fit_dy, fit_dw, fit_dh;  /* scaled rect in the panel */

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
    int      prev_is_dumb;
    int      first_frame;

    /* Saved CRTC state for restore on close */
    drmModeCrtcPtr prev_crtc;

    /* ---- Subtitle overlay plane ---- */
    uint32_t ovl_plane_id;      /* 0 = no overlay plane available      */

    /* Overlay plane property IDs */
    uint32_t ovl_prop_fb_id;
    uint32_t ovl_prop_crtc_id;
    uint32_t ovl_prop_crtc_x;
    uint32_t ovl_prop_crtc_y;
    uint32_t ovl_prop_crtc_w;
    uint32_t ovl_prop_crtc_h;
    uint32_t ovl_prop_src_x;
    uint32_t ovl_prop_src_y;
    uint32_t ovl_prop_src_w;
    uint32_t ovl_prop_src_h;

    /* Persistent ARGB8888 dumb buffer for the overlay */
    uint32_t ovl_gem;           /* GEM handle                          */
    uint32_t ovl_fb_id;         /* registered framebuffer              */
    uint32_t ovl_pitch;         /* bytes per row                       */
    size_t   ovl_buf_size;      /* total buffer size in bytes          */
    void    *ovl_map;           /* mmap pointer (NULL = not allocated) */
    int      ovl_showing;       /* 1 = overlay plane currently active  */
} DrmOutput;

typedef struct {
    int        fd;
    DrmOutput  outputs[DRM_MAX_OUTPUTS];
    int        output_count;
} DrmContext;

/*
 * Set by drm_open(): 1 when the video plane is an RGB565 SPI/DBI panel
 * (no NV12 plane, no scaling). vdec then decodes straight to RGB565 and
 * drm_present() centre-crops a panel-sized window instead of scaling.
 */
extern int g_drm_spi_panel;

/* SPI/DBI panels only: 0 = aspect-fit (letterbox, default), 1 = crop-to-fill. */
extern int g_spi_fill;

int  drm_open(DrmContext *ctx);

/* Present a decoded video frame (NV12, DMABUF) */
int  drm_present(DrmContext *ctx, int output_idx, DecodedFrame *frame);

/* Present a still image (XRGB8888 pixels, CPU memory) */
int  drm_present_image(DrmContext *ctx, int output_idx,
                        uint8_t *pixels, int width, int height, int stride);

/*
 * Update the subtitle overlay.
 * text=NULL  — clear and hide the overlay plane.
 * text!=NULL — render text and show the overlay plane.
 * No-op if no overlay plane was found at open time.
 */
int  drm_subtitle_update(DrmContext *ctx, int output_idx, const char *text);

void drm_close(DrmContext *ctx);

#endif
