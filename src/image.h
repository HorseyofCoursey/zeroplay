#pragma once
#include <stdint.h>

/*
 * Decode an image file (JPEG, PNG, BMP) to XRGB8888.
 *
 * On success, *pixels_out is a malloc'd buffer of stride * height bytes,
 * stride = width * 4.  The caller must free() it.
 *
 * Returns 0 on success, -1 on error.
 */
int image_decode_xrgb(const char *path,
                       uint8_t   **pixels_out,
                       int        *width_out,
                       int        *height_out,
                       int        *stride_out);
