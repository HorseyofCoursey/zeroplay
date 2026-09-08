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

#ifdef HAVE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

int g_drm_spi_panel = 0;

/* ------------------------------------------------------------------ */
/* Bitmap font — public domain 8x8, chars 0x20-0x7E                   */
/* Each byte = one row (top→bottom). Bit 0 = leftmost pixel.          */
/* ------------------------------------------------------------------ */

static const uint8_t font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x20 ' ' */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* 0x21 '!' */
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x22 '"' */
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, /* 0x23 '#' */
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, /* 0x24 '$' */
    {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, /* 0x25 '%' */
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, /* 0x26 '&' */
    {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, /* 0x27 ''' */
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, /* 0x28 '(' */
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, /* 0x29 ')' */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* 0x2A '*' */
    {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, /* 0x2B '+' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, /* 0x2C ',' */
    {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, /* 0x2D '-' */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, /* 0x2E '.' */
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, /* 0x2F '/' */
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, /* 0x30 '0' */
    {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, /* 0x31 '1' */
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, /* 0x32 '2' */
    {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, /* 0x33 '3' */
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, /* 0x34 '4' */
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, /* 0x35 '5' */
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, /* 0x36 '6' */
    {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, /* 0x37 '7' */
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, /* 0x38 '8' */
    {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, /* 0x39 '9' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, /* 0x3A ':' */
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, /* 0x3B ';' */
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, /* 0x3C '<' */
    {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, /* 0x3D '=' */
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, /* 0x3E '>' */
    {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, /* 0x3F '?' */
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, /* 0x40 '@' */
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, /* 0x41 'A' */
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, /* 0x42 'B' */
    {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, /* 0x43 'C' */
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, /* 0x44 'D' */
    {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, /* 0x45 'E' */
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, /* 0x46 'F' */
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, /* 0x47 'G' */
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, /* 0x48 'H' */
    {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x49 'I' */
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, /* 0x4A 'J' */
    {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, /* 0x4B 'K' */
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, /* 0x4C 'L' */
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, /* 0x4D 'M' */
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, /* 0x4E 'N' */
    {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, /* 0x4F 'O' */
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, /* 0x50 'P' */
    {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, /* 0x51 'Q' */
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, /* 0x52 'R' */
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, /* 0x53 'S' */
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x54 'T' */
    {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, /* 0x55 'U' */
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 0x56 'V' */
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, /* 0x57 'W' */
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, /* 0x58 'X' */
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, /* 0x59 'Y' */
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, /* 0x5A 'Z' */
    {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, /* 0x5B '[' */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, /* 0x5C '\' */
    {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, /* 0x5D ']' */
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, /* 0x5E '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* 0x5F '_' */
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, /* 0x60 '`' */
    {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, /* 0x61 'a' */
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, /* 0x62 'b' */
    {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, /* 0x63 'c' */
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, /* 0x64 'd' */
    {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, /* 0x65 'e' */
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, /* 0x66 'f' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, /* 0x67 'g' */
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, /* 0x68 'h' */
    {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x69 'i' */
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, /* 0x6A 'j' */
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, /* 0x6B 'k' */
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, /* 0x6C 'l' */
    {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, /* 0x6D 'm' */
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, /* 0x6E 'n' */
    {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, /* 0x6F 'o' */
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, /* 0x70 'p' */
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, /* 0x71 'q' */
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, /* 0x72 'r' */
    {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, /* 0x73 's' */
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, /* 0x74 't' */
    {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, /* 0x75 'u' */
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, /* 0x76 'v' */
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, /* 0x77 'w' */
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, /* 0x78 'x' */
    {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, /* 0x79 'y' */
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, /* 0x7A 'z' */
    {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, /* 0x7B '{' */
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* 0x7C '|' */
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, /* 0x7D '}' */
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, /* 0x7E '~' */
};

/* ------------------------------------------------------------------ */
/* Subtitle rendering                                                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_FREETYPE
#define FONT_PATH     "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
#define FONT_SIZE_PX  36
static FT_Library  ft_lib  = NULL;
static FT_Face     ft_face = NULL;

static void ft_init(void)
{
    if (FT_Init_FreeType(&ft_lib)) {
        fprintf(stderr, "drm: FreeType init failed, using bitmap font\n");
        ft_lib = NULL;
        return;
    }
    if (FT_New_Face(ft_lib, FONT_PATH, 0, &ft_face)) {
        fprintf(stderr, "drm: cannot load %s, using bitmap font\n", FONT_PATH);
        FT_Done_FreeType(ft_lib);
        ft_lib = NULL;
        ft_face = NULL;
        return;
    }
    FT_Set_Pixel_Sizes(ft_face, 0, FONT_SIZE_PX);
    vlog("drm: FreeType loaded %s @ %dpx\n", FONT_PATH, FONT_SIZE_PX);
}

static void ft_close(void)
{
    if (ft_face) { FT_Done_Face(ft_face);       ft_face = NULL; }
    if (ft_lib)  { FT_Done_FreeType(ft_lib);    ft_lib  = NULL; }
}

/*
 * Measure the pixel width of a string using the loaded face.
 */
static int ft_measure_string(const char *text)
{
    if (!ft_face) return 0;
    int width = 0;
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(ft_face, (unsigned char)*p, FT_LOAD_ADVANCE_ONLY))
            continue;
        width += (int)(ft_face->glyph->advance.x >> 6);
    }
    return width;
}

/*
 * Blend a single FreeType bitmap glyph into the ARGB buffer.
 * text_color and shadow_color are 0xAARRGGBB.
 */
static void ft_blit_glyph(uint32_t *buf, uint32_t pitch_px,
                            uint32_t disp_w, uint32_t disp_h,
                            FT_Bitmap *bm, int bx, int by,
                            uint32_t text_color, uint32_t shadow_color,
                            int shadow_off)
{
    /* Shadow pass */
    for (int row = 0; row < (int)bm->rows; row++) {
        for (int col = 0; col < (int)bm->width; col++) {
            uint8_t alpha = bm->buffer[row * bm->pitch + col];
            if (!alpha) continue;
            int px = bx + col + shadow_off;
            int py = by + row + shadow_off;
            if (px < 0 || py < 0 ||
                (uint32_t)px >= disp_w || (uint32_t)py >= disp_h) continue;
            uint32_t *dst = &buf[(uint32_t)py * pitch_px + (uint32_t)px];
            uint32_t  sa  = (shadow_color >> 24) & 0xFF;
            uint32_t  a   = (alpha * sa) >> 8;
            uint32_t  ia  = 255 - a;
            uint32_t  sr  = (shadow_color >> 16) & 0xFF;
            uint32_t  sg  = (shadow_color >>  8) & 0xFF;
            uint32_t  sb  = (shadow_color      ) & 0xFF;
            uint32_t  dr  = (*dst >> 16) & 0xFF;
            uint32_t  dg  = (*dst >>  8) & 0xFF;
            uint32_t  db  = (*dst      ) & 0xFF;
            *dst = 0xFF000000 |
                   (((sr * a + dr * ia) / 255) << 16) |
                   (((sg * a + dg * ia) / 255) <<  8) |
                   (((sb * a + db * ia) / 255)      );
        }
    }
    /* Text pass */
    for (int row = 0; row < (int)bm->rows; row++) {
        for (int col = 0; col < (int)bm->width; col++) {
            uint8_t alpha = bm->buffer[row * bm->pitch + col];
            if (!alpha) continue;
            int px = bx + col;
            int py = by + row;
            if (px < 0 || py < 0 ||
                (uint32_t)px >= disp_w || (uint32_t)py >= disp_h) continue;
            uint32_t *dst = &buf[(uint32_t)py * pitch_px + (uint32_t)px];
            uint32_t  ta  = (text_color >> 24) & 0xFF;
            uint32_t  a   = (alpha * ta) >> 8;
            uint32_t  ia  = 255 - a;
            uint32_t  tr  = (text_color >> 16) & 0xFF;
            uint32_t  tg  = (text_color >>  8) & 0xFF;
            uint32_t  tb  = (text_color      ) & 0xFF;
            uint32_t  dr  = (*dst >> 16) & 0xFF;
            uint32_t  dg  = (*dst >>  8) & 0xFF;
            uint32_t  db  = (*dst      ) & 0xFF;
            *dst = 0xFF000000 |
                   (((tr * a + dr * ia) / 255) << 16) |
                   (((tg * a + dg * ia) / 255) <<  8) |
                   (((tb * a + db * ia) / 255)      );
        }
    }
}

/*
 * Render one line of text using FreeType into the ARGB buffer.
 * Returns the actual pixel width rendered.
 */
static int ft_render_line(uint32_t *buf, uint32_t pitch_px,
                           uint32_t disp_w, uint32_t disp_h,
                           const char *text, int x, int baseline_y)
{
    if (!ft_face) return 0;
    int pen_x = x;
    for (const char *p = text; *p; p++) {
        if (FT_Load_Char(ft_face, (unsigned char)*p,
                         FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
            continue;
        FT_GlyphSlot g = ft_face->glyph;
        int bx = pen_x + g->bitmap_left;
        int by = baseline_y - g->bitmap_top;
        ft_blit_glyph(buf, pitch_px, disp_w, disp_h,
                      &g->bitmap, bx, by,
                      0xFFFFFFFF, 0xC0000000, 2);
        pen_x += (int)(g->advance.x >> 6);
    }
    return pen_x - x;
}
#endif /* HAVE_FREETYPE */

#define FONT_SCALE    3         /* bitmap fallback: each 8x8 glyph → 24x24 */
#define CHAR_W        (8  * FONT_SCALE)
#define CHAR_H        (8  * FONT_SCALE)
#define SUB_PADDING   14        /* px above/below text inside bar      */
#define SUB_MARGIN    20        /* px from bottom of display           */
#define SUB_MAX_LINES 4
#define SUB_LINE_MAX  128

/*
 * Fill a rectangle in the ARGB dumb buffer.
 * color is 0xAARRGGBB.
 */
static void fill_rect(uint32_t *buf, uint32_t pitch_px,
                      int x, int y, int w, int h,
                      uint32_t disp_w, uint32_t disp_h,
                      uint32_t color)
{
    for (int row = y; row < y + h; row++) {
        if (row < 0 || (uint32_t)row >= disp_h) continue;
        for (int col = x; col < x + w; col++) {
            if (col < 0 || (uint32_t)col >= disp_w) continue;
            buf[(uint32_t)row * pitch_px + (uint32_t)col] = color;
        }
    }
}

/*
 * Draw a single character glyph at (x, y).
 * Shadow is drawn first (offset +FONT_SCALE), then text on top.
 */
static void draw_char(uint32_t *buf, uint32_t pitch_px,
                      int x, int y, unsigned char ch,
                      uint32_t disp_w, uint32_t disp_h)
{
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *glyph = font8x8[ch - 0x20];

    /* Shadow pass */
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << col))) continue;
            for (int sy = 0; sy < FONT_SCALE; sy++) {
                for (int sx = 0; sx < FONT_SCALE; sx++) {
                    int px = x + col * FONT_SCALE + sx + FONT_SCALE;
                    int py = y + row * FONT_SCALE + sy + FONT_SCALE;
                    if (px < 0 || py < 0 ||
                        (uint32_t)px >= disp_w ||
                        (uint32_t)py >= disp_h) continue;
                    buf[(uint32_t)py * pitch_px + (uint32_t)px] = 0xFF101010;
                }
            }
        }
    }

    /* Text pass */
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (1 << col))) continue;
            for (int sy = 0; sy < FONT_SCALE; sy++) {
                for (int sx = 0; sx < FONT_SCALE; sx++) {
                    int px = x + col * FONT_SCALE + sx;
                    int py = y + row * FONT_SCALE + sy;
                    if (px < 0 || py < 0 ||
                        (uint32_t)px >= disp_w ||
                        (uint32_t)py >= disp_h) continue;
                    buf[(uint32_t)py * pitch_px + (uint32_t)px] = 0xFFFFFFFF;
                }
            }
        }
    }
}

/*
 * Render subtitle text into the overlay ARGB buffer.
 * Uses FreeType2 when available, falls back to bitmap font.
 * Splits on '\n', centres each line, draws a semi-transparent bar.
 */
static void render_subtitle_frame(DrmOutput *out, const char *text)
{
    if (!out->ovl_map || !text || !text[0]) return;

    uint32_t *buf      = (uint32_t *)out->ovl_map;
    uint32_t  pitch_px = out->ovl_pitch / 4;
    uint32_t  disp_w   = out->mode_w;
    uint32_t  disp_h   = out->mode_h;

    /* Clear to fully transparent */
    memset(out->ovl_map, 0, out->ovl_buf_size);

    /* Split text into lines */
    char     lines[SUB_MAX_LINES][SUB_LINE_MAX];
    int      nlines = 0;
    const char *p = text;

    while (*p && nlines < SUB_MAX_LINES) {
        const char *nl  = strchr(p, '\n');
        size_t      len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= SUB_LINE_MAX) len = SUB_LINE_MAX - 1;
        memcpy(lines[nlines], p, len);
        lines[nlines][len] = '\0';
        nlines++;
        p = nl ? nl + 1 : p + len;
        if (!nl) break;
    }
    if (!nlines) return;

#ifdef HAVE_FREETYPE
    if (ft_face) {
        /* FreeType path */
        int line_h   = (int)(ft_face->size->metrics.height >> 6);
        int ascender = (int)(ft_face->size->metrics.ascender >> 6);
        if (line_h <= 0) line_h = FONT_SIZE_PX + 4;

        /* Measure widest line */
        int max_w = 0;
        for (int li = 0; li < nlines; li++) {
            int w = ft_measure_string(lines[li]);
            if (w > max_w) max_w = w;
        }

        int bar_w = max_w + 2 * SUB_PADDING;
        int bar_h = nlines * line_h + 2 * SUB_PADDING;
        int bar_x = ((int)disp_w - bar_w) / 2;
        int bar_y = (int)disp_h - bar_h - SUB_MARGIN;
        if (bar_x < 0) { bar_x = 0; bar_w = (int)disp_w; }
        if (bar_y < 0) bar_y = 0;

        fill_rect(buf, pitch_px, bar_x, bar_y, bar_w, bar_h,
                  disp_w, disp_h, 0xB0000000);

        for (int li = 0; li < nlines; li++) {
            int lw      = ft_measure_string(lines[li]);
            int text_x  = bar_x + (bar_w - lw) / 2;
            int baseline = bar_y + SUB_PADDING + ascender + li * line_h;
            ft_render_line(buf, pitch_px, disp_w, disp_h,
                           lines[li], text_x, baseline);
        }
        return;
    }
#endif

    /* Bitmap font fallback */
    int max_len = 0;
    for (int li = 0; li < nlines; li++) {
        int len = (int)strlen(lines[li]);
        if (len > max_len) max_len = len;
    }
    int bar_w = max_len * CHAR_W + 2 * SUB_PADDING;
    int bar_h = nlines * CHAR_H + 2 * SUB_PADDING;
    int bar_x = ((int)disp_w - bar_w) / 2;
    int bar_y = (int)disp_h - bar_h - SUB_MARGIN;
    if (bar_x < 0) { bar_x = 0; bar_w = (int)disp_w; }
    if (bar_y < 0) bar_y = 0;

    fill_rect(buf, pitch_px, bar_x, bar_y, bar_w, bar_h,
              disp_w, disp_h, 0xB0000000);

    for (int li = 0; li < nlines; li++) {
        int len    = (int)strlen(lines[li]);
        int text_x = bar_x + SUB_PADDING +
                     (bar_w - 2 * SUB_PADDING - len * CHAR_W) / 2;
        int text_y = bar_y + SUB_PADDING + li * CHAR_H;
        int max_chars = (int)disp_w / CHAR_W;
        if (len > max_chars) len = max_chars;
        for (int ci = 0; ci < len; ci++) {
            draw_char(buf, pitch_px,
                      text_x + ci * CHAR_W, text_y,
                      (unsigned char)lines[li][ci],
                      disp_w, disp_h);
        }
    }
}

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

static int plane_supports_format(int fd, uint32_t plane_id, uint32_t fmt)
{
    drmModePlane *plane = drmModeGetPlane(fd, plane_id);
    if (!plane) return 0;
    int found = 0;
    for (uint32_t i = 0; i < plane->count_formats; i++) {
        if (plane->formats[i] == fmt) { found = 1; break; }
    }
    drmModeFreePlane(plane);
    return found;
}

static int plane_supports_nv12(int fd, uint32_t plane_id)
{
    return plane_supports_format(fd, plane_id, DRM_FORMAT_NV12);
}

static int plane_supports_rgb565(int fd, uint32_t plane_id)
{
    return plane_supports_format(fd, plane_id, DRM_FORMAT_RGB565);
}

static int plane_supports_argb8888(int fd, uint32_t plane_id)
{
    return plane_supports_format(fd, plane_id, DRM_FORMAT_ARGB8888);
}

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
 * Atomic commit for the video plane.
 * need_modeset=1 sets ACTIVE/MODE_ID/CRTC_ID and allows modeset.
 */
static int do_atomic_commit(int fd, DrmOutput *out,
                             uint32_t fb_id,
                             uint32_t src_x, uint32_t src_y,
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

    drmModeAtomicAddProperty(areq, out->plane_id, out->prop_src_x,
                             (uint64_t)src_x << 16);
    drmModeAtomicAddProperty(areq, out->plane_id, out->prop_src_y,
                             (uint64_t)src_y << 16);
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
 * Atomic commit for the subtitle overlay plane only.
 * fb_id=0 disables the plane.
 * Uses DRM_MODE_ATOMIC_NONBLOCK — subtitle updates are low-frequency.
 */
static int do_overlay_commit(int fd, DrmOutput *out, uint32_t fb_id)
{
    if (!out->ovl_plane_id) return 0;

    drmModeAtomicReq *areq = drmModeAtomicAlloc();
    if (!areq) return -1;

    if (fb_id) {
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_src_x, 0);
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_src_y, 0);
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_src_w,
                                 (uint64_t)out->mode_w << 16);
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_src_h,
                                 (uint64_t)out->mode_h << 16);
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_crtc_x, 0);
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_crtc_y, 0);
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_crtc_w, out->mode_w);
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_crtc_h, out->mode_h);
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_crtc_id, out->crtc_id);
    } else {
        /* Disable: CRTC_ID=0, FB_ID=0 */
        drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                                 out->ovl_prop_crtc_id, 0);
    }
    drmModeAtomicAddProperty(areq, out->ovl_plane_id,
                             out->ovl_prop_fb_id, fb_id);

    int ret = drmModeAtomicCommit(fd, areq, DRM_MODE_ATOMIC_NONBLOCK, NULL);
    if (ret < 0)
        perror("drm: overlay atomic commit");
    drmModeAtomicFree(areq);
    return ret;
}

/*
 * Allocate and mmap the overlay dumb buffer for one output.
 * Called during setup_output if an overlay plane was found.
 */
static void setup_overlay_buffer(int fd, DrmOutput *out)
{
    struct drm_mode_create_dumb creq;
    memset(&creq, 0, sizeof(creq));
    creq.width  = out->mode_w;
    creq.height = out->mode_h;
    creq.bpp    = 32;

    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("drm: overlay create dumb");
        return;
    }

    struct drm_mode_map_dumb mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = creq.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        perror("drm: overlay map dumb");
        goto destroy;
    }

    void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, mreq.offset);
    if (map == MAP_FAILED) {
        perror("drm: overlay mmap");
        goto destroy;
    }

    memset(map, 0, creq.size);   /* fully transparent */

    uint32_t handles[4] = { creq.handle, 0, 0, 0 };
    uint32_t pitches[4] = { creq.pitch,  0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t fb_id = 0;

    if (drmModeAddFB2(fd, out->mode_w, out->mode_h,
                       DRM_FORMAT_ARGB8888,
                       handles, pitches, offsets, &fb_id, 0) < 0) {
        perror("drm: overlay register FB");
        munmap(map, creq.size);
        goto destroy;
    }

    out->ovl_gem      = creq.handle;
    out->ovl_fb_id    = fb_id;
    out->ovl_pitch    = creq.pitch;
    out->ovl_buf_size = creq.size;
    out->ovl_map      = map;

    vlog("drm: subtitle overlay plane %u allocated "
            "(%ux%u ARGB8888)\n",
            out->ovl_plane_id, out->mode_w, out->mode_h);
    return;

destroy: {
        struct drm_mode_destroy_dumb d = { .handle = creq.handle };
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
        out->ovl_plane_id = 0;   /* mark as unavailable */
    }
}

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

    out->prev_crtc = drmModeGetCrtc(fd, crtc_id);

    vlog("drm: connector %u -> crtc %u (idx %d)\n",
            out->connector_id, out->crtc_id, crtc_idx);

    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    if (!pr) {
        fprintf(stderr, "drm: no plane resources\n");
        drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
        return -1;
    }

    /* Pass 1: find NV12-capable video plane (hardware-scaled path) */
    for (uint32_t i = 0; i < pr->count_planes && !out->plane_id; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
        if (!p) continue;
        if ((p->possible_crtcs & (1u << crtc_idx)) &&
            plane_supports_nv12(fd, p->plane_id)) {
            out->plane_id     = p->plane_id;
            out->plane_format = DRM_FORMAT_NV12;
        }
        drmModeFreePlane(p);
    }

    /* Pass 1b: no NV12 plane (e.g. an SPI/DBI panel) — fall back to an
     * RGB565 plane and drive it with a centre-crop instead of scaling. */
    for (uint32_t i = 0; i < pr->count_planes && !out->plane_id; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
        if (!p) continue;
        if ((p->possible_crtcs & (1u << crtc_idx)) &&
            plane_supports_rgb565(fd, p->plane_id)) {
            out->plane_id     = p->plane_id;
            out->plane_format = DRM_FORMAT_RGB565;
        }
        drmModeFreePlane(p);
    }

    /* Pass 2: find ARGB8888-capable overlay plane (different from video plane) */
    for (uint32_t i = 0; i < pr->count_planes && !out->ovl_plane_id; i++) {
        drmModePlane *p = drmModeGetPlane(fd, pr->planes[i]);
        if (!p) continue;
        if ((p->possible_crtcs & (1u << crtc_idx)) &&
            p->plane_id != out->plane_id &&
            plane_supports_argb8888(fd, p->plane_id))
            out->ovl_plane_id = p->plane_id;
        drmModeFreePlane(p);
    }

    drmModeFreePlaneResources(pr);

    if (!out->plane_id) {
        fprintf(stderr, "drm: no NV12 or RGB565 plane for crtc %u\n", out->crtc_id);
        drmModeDestroyPropertyBlob(fd, out->mode_blob_id);
        return -1;
    }
    vlog("drm: connector %u video plane %u format %.4s\n",
         out->connector_id, out->plane_id, (char *)&out->plane_format);

    /* Video plane properties */
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

    /* Overlay plane properties (only if we found one) */
    if (out->ovl_plane_id) {
        out->ovl_prop_fb_id   = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "FB_ID");
        out->ovl_prop_crtc_id = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        out->ovl_prop_crtc_x  = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "CRTC_X");
        out->ovl_prop_crtc_y  = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        out->ovl_prop_crtc_w  = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "CRTC_W");
        out->ovl_prop_crtc_h  = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "CRTC_H");
        out->ovl_prop_src_x   = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "SRC_X");
        out->ovl_prop_src_y   = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "SRC_Y");
        out->ovl_prop_src_w   = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "SRC_W");
        out->ovl_prop_src_h   = get_property_id(fd, out->ovl_plane_id,
                                                  DRM_MODE_OBJECT_PLANE, "SRC_H");

        if (out->ovl_prop_fb_id && out->ovl_prop_crtc_id &&
            out->ovl_prop_crtc_w && out->ovl_prop_src_w)
            setup_overlay_buffer(fd, out);
        else
            out->ovl_plane_id = 0;   /* properties missing, disable */
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

        if(ctx->fd < 0){
            vlog("drm: failed to open %s : %s\n", devices[i]->nodes[DRM_NODE_PRIMARY], strerror(errno));
            continue;
        }

        if (drmSetClientCap(ctx->fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0)
            break;
        else{
            close(ctx->fd);
            ctx->fd = -1;
        }
    }
    drmFreeDevices(devices, device_count);

    if (ctx->fd < 0) {
        fprintf(stderr, "drm: no DRM device with atomic mode setting found\n");
        return -1;
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

    /* If the primary output is an RGB565 SPI/DBI panel, tell vdec to decode
     * straight to RGB565 and use the centre-crop present path. */
    g_drm_spi_panel = (ctx->outputs[0].plane_format == DRM_FORMAT_RGB565);
    if (g_drm_spi_panel)
        vlog("drm: SPI/DBI panel detected — RGB565 decode + centre-crop\n");

#ifdef HAVE_FREETYPE
    ft_init();
#endif

    return 0;
}

/* ------------------------------------------------------------------ */

/*
 * Present an RGB565 decoded frame on an SPI/DBI panel.
 * The panel plane cannot scale or be repositioned, so the frame must be at
 * least panel-sized and we display a panel-sized window centred in it
 * (requires the drm/mipi-dbi source-offset support).
 */
static int drm_present_rgb565(DrmContext *ctx, DrmOutput *out,
                               DecodedFrame *frame)
{
    uint32_t vis_w = frame->width;
    uint32_t vis_h = frame->src_height;

    if (vis_w < out->mode_w || vis_h < out->mode_h) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr,
                "drm: frame %ux%u smaller than panel %ux%u — SPI crop path "
                "needs a source at least panel-sized\n",
                vis_w, vis_h, out->mode_w, out->mode_h);
            warned = 1;
        }
        return -1;
    }

    uint32_t gem_handle = 0;
    if (drmPrimeFDToHandle(ctx->fd, frame->dmabuf_fd, &gem_handle) < 0) {
        perror("drm: drmPrimeFDToHandle (rgb565)");
        return -1;
    }

    uint32_t fb_w = frame->stride / 2;          /* padded width, pixels */
    uint32_t handles[4] = { gem_handle, 0, 0, 0 };
    uint32_t pitches[4] = { frame->stride, 0, 0, 0 };
    uint32_t offsets[4] = { 0, 0, 0, 0 };
    uint32_t fb_id = 0;

    if (drmModeAddFB2(ctx->fd, fb_w, frame->height, DRM_FORMAT_RGB565,
                       handles, pitches, offsets, &fb_id, 0) < 0) {
        perror("drm: drmModeAddFB2 (rgb565)");
        drmCloseBufferHandle(ctx->fd, gem_handle);
        return -1;
    }

    /* Centre-crop: src == panel size, positioned in the larger frame. */
    uint32_t sw = out->mode_w;
    uint32_t sh = out->mode_h;
    uint32_t sx = ((vis_w - sw) / 2) & ~1u;
    uint32_t sy = ((vis_h - sh) / 2) & ~1u;

    /* Plane fills the whole CRTC (no positioning allowed). */
    out->dest_x = 0;
    out->dest_y = 0;
    out->dest_w = sw;
    out->dest_h = sh;

    int need_modeset = out->first_frame ||
                       (out->current_format != DRM_FORMAT_RGB565);

    if (do_atomic_commit(ctx->fd, out, fb_id, sx, sy, sw, sh,
                          need_modeset) < 0) {
        perror("drm: atomic commit (rgb565)");
        drmModeRmFB(ctx->fd, fb_id);
        drmCloseBufferHandle(ctx->fd, gem_handle);
        return -1;
    }

    out->first_frame    = 0;
    out->current_format = DRM_FORMAT_RGB565;

    release_prev(ctx->fd, out);
    out->prev_fb_id      = fb_id;
    out->prev_gem_handle = gem_handle;
    out->prev_is_dumb    = 0;
    return 0;
}

int drm_present(DrmContext *ctx, int output_idx, DecodedFrame *frame)
{
    if (output_idx < 0 || output_idx >= ctx->output_count) {
        fprintf(stderr, "drm: invalid output index %d\n", output_idx);
        return -1;
    }
    DrmOutput *out = &ctx->outputs[output_idx];

    if (out->plane_format == DRM_FORMAT_RGB565)
        return drm_present_rgb565(ctx, out, frame);

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

    int need_modeset = out->first_frame ||
                       (out->current_format != DRM_FORMAT_NV12);

    ret = do_atomic_commit(ctx->fd, out, fb_id, 0, 0,
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

    for (int y = 0; y < height; y++) {
        memcpy((uint8_t *)map + (size_t)y * creq.pitch,
               pixels          + (size_t)y * stride,
               (size_t)width * 4);
    }
    munmap(map, creq.size);

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

    calc_dest_rect(out->mode_w, out->mode_h,
                   (uint32_t)width, (uint32_t)height, 1, 1,
                   &out->dest_x, &out->dest_y,
                   &out->dest_w, &out->dest_h);

    int need_modeset = out->first_frame ||
                       (out->current_format != DRM_FORMAT_XRGB8888);

    if (do_atomic_commit(ctx->fd, out, fb_id, 0, 0,
                          (uint32_t)width, (uint32_t)height,
                          need_modeset) < 0) {
        perror("drm: atomic commit image");
        drmModeRmFB(ctx->fd, fb_id);
        goto destroy_dumb;
    }

    out->first_frame    = 0;
    out->current_format = DRM_FORMAT_XRGB8888;
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

int drm_subtitle_update(DrmContext *ctx, int output_idx, const char *text)
{
    if (output_idx < 0 || output_idx >= ctx->output_count) return 0;
    DrmOutput *out = &ctx->outputs[output_idx];

    if (!out->ovl_plane_id || !out->ovl_map) return 0;

    if (!text || !text[0]) {
        /* Clear overlay */
        if (!out->ovl_showing) return 0;
        do_overlay_commit(ctx->fd, out, 0);
        out->ovl_showing = 0;
        return 0;
    }

    /* Render text into dumb buffer then present */
    render_subtitle_frame(out, text);
    do_overlay_commit(ctx->fd, out, out->ovl_fb_id);
    out->ovl_showing = 1;
    return 0;
}

/* ------------------------------------------------------------------ */

void drm_close(DrmContext *ctx)
{
    for (int i = 0; i < ctx->output_count; i++) {
        DrmOutput *out = &ctx->outputs[i];

        /* Disable and free overlay plane */
        if (out->ovl_plane_id && out->ovl_showing)
            do_overlay_commit(ctx->fd, out, 0);
        if (out->ovl_map) {
            munmap(out->ovl_map, out->ovl_buf_size);
            out->ovl_map = NULL;
        }
        if (out->ovl_fb_id) {
            drmModeRmFB(ctx->fd, out->ovl_fb_id);
            out->ovl_fb_id = 0;
        }
        if (out->ovl_gem) {
            struct drm_mode_destroy_dumb d = { .handle = out->ovl_gem };
            drmIoctl(ctx->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
            out->ovl_gem = 0;
        }

        release_prev(ctx->fd, out);

        if (out->mode_blob_id) {
            drmModeDestroyPropertyBlob(ctx->fd, out->mode_blob_id);
            out->mode_blob_id = 0;
        }

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
#ifdef HAVE_FREETYPE
    ft_close();
#endif

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}
