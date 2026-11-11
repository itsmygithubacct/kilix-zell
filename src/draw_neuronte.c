/* The Neuronte renderer: the same scene drawn with Neuronte nets.
 *
 * This file gives the soft-raster calls draw.c makes a Neuronte body and
 * then includes draw.c, so the layout code is shared and cannot drift
 * between the two renderers.
 *
 *   text            one cell per character, from the nets
 *   outlines        eighth blocks, placed so their ink is the outline
 *   thin bars       a full block stretched to the bar (cursor, lever)
 *   disc, ellipse   the black circle, fitted to the shape's box
 *   sprites, tiles  every pixel through 2 x 4 octant cells: the cell's
 *                   eight source pixels are its eight colours, and three
 *                   masks from the pixel net say which colour each pixel
 *                   of the cell takes
 *
 * What stays plain code: clearing the frame, filling a rectangle with one
 * colour, choosing which source pixel a sub-cell shows, and mixing colours
 * by coverage.
 *
 * Built by `make RENDER=neuronte`. The weight file is the one named at
 * build time by ZELL_NEURONTE_WEIGHTS, or by the environment variable of
 * the same name. */
#include "draw_neuronte.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "neuronte.h"
#include "soft_raster.h"

#ifndef ZELL_NEURONTE_WEIGHTS
#define ZELL_NEURONTE_WEIGHTS "neuronte-split-v4.nrt"
#endif

enum { ZN_CELL_W = 8, ZN_CELL_H = 16 };

static neuronte *zn_font;
static int zn_state; /* 0 not tried, 1 open, -1 failed */

const char *zell_neuronte_weights(void)
{
    const char *path = getenv("ZELL_NEURONTE_WEIGHTS");

    return path != NULL && path[0] != '\0' ? path : ZELL_NEURONTE_WEIGHTS;
}

bool zell_neuronte_open(void)
{
    if (zn_state == 0) {
        zn_state = neuronte_open_file(zell_neuronte_weights(), &zn_font) == 0 ? 1 : -1;
        if (zn_state < 0) {
            zn_font = NULL;
        }
    }
    return zn_state > 0;
}

void zell_neuronte_close(void)
{
    neuronte_close(zn_font);
    zn_font = NULL;
    zn_state = 0;
}

static int zn_scale(int scale)
{
    return scale < 1 ? 1 : scale;
}

/* One cell per scalar, as Neuronte draws it. */
static int zn_text_width(const char *s, int scale)
{
    size_t len = s != NULL ? strlen(s) : 0;
    size_t at = 0;
    long cells = 0;

    while (at < len) {
        uint32_t scalar;

        at += neuronte_utf8_decode(s + at, len - at, &scalar);
        cells++;
    }
    cells *= ZN_CELL_W * zn_scale(scale);
    return cells > 0x7fffffffL ? 0x7fffffff : (int)cells;
}

static uint32_t zn_color(uint32_t rgb, float alpha)
{
    float a = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;

    return ((uint32_t)(a * 255.0f + 0.5f) << 24) | (rgb & 0xffffffu);
}

static void zn_text(sr_canvas *c, float x, float y, const char *s,
                    uint32_t rgb, float alpha, int scale)
{
    int k = zn_scale(scale);

    if (c == NULL || s == NULL || !zell_neuronte_open()) {
        return;
    }
    neuronte_draw_utf8(zn_font, c->px, c->w, c->w, c->h,
                       (int)(x + 0.5f), (int)(y + 0.5f),
                       ZN_CELL_W * k, ZN_CELL_H * k, s, strlen(s),
                       zn_color(rgb, alpha));
}

/* A dark rim one scaled pixel wide, then the text. Eight more draws of
 * masks the library has cached. */
static void zn_text_outlined(sr_canvas *c, float x, float y, const char *s,
                             uint32_t rgb, float alpha, int scale)
{
    int k = zn_scale(scale);
    int dx;
    int dy;

    for (dy = -1; dy <= 1; dy++) {
        for (dx = -1; dx <= 1; dx++) {
            if (dx != 0 || dy != 0) {
                zn_text(c, x + (float)(dx * k), y + (float)(dy * k), s,
                        0x000000u, alpha, scale);
            }
        }
    }
    zn_text(c, x, y, s, rgb, alpha, scale);
}

/* ---- shapes ---- */

static int zn_round(float v)
{
    return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

static void zn_glyph(sr_canvas *c, uint32_t scalar, int x, int y, int cell_w, int cell_h,
                     uint32_t rgb, float alpha)
{
    if (c == NULL || cell_w < 1 || cell_h < 1 || !zell_neuronte_open()) {
        return;
    }
    neuronte_draw_scalar(zn_font, c->px, c->w, c->w, c->h, x, y, cell_w, cell_h,
                         scalar, zn_color(rgb, alpha));
}

/* A bar no thicker than three pixels is a full block stretched to it.
 * Anything larger is a flat fill, which is a background and stays code. */
static void zn_fill_rect(sr_canvas *c, float x, float y, float w, float h,
                         uint32_t rgb, float alpha)
{
    int iw = zn_round(w);
    int ih = zn_round(h);

    if (iw >= 1 && ih >= 1 && (iw <= 3 || ih <= 3) && iw <= 64 && ih <= 64) {
        zn_glyph(c, 0x2588u, zn_round(x), zn_round(y), iw, ih, rgb, alpha);
        return;
    }
    sr_fill_rect(c, x, y, w, h, rgb, alpha);
}

/* An eighth block's ink is one eighth of its cell. A side t pixels thick
 * is that block in a cell 8t across the side, stretched along it. */
static void zn_stroke_rect(sr_canvas *c, float x, float y, float w, float h,
                           float thickness, uint32_t rgb, float alpha)
{
    int x0 = zn_round(x);
    int y0 = zn_round(y);
    int iw = zn_round(x + w) - x0;
    int ih = zn_round(y + h) - y0;
    int t = zn_round(thickness);

    if (t < 1) {
        t = 1;
    }
    if (iw < 2 * t || ih < 2 * t) {
        zn_fill_rect(c, x, y, w, h, rgb, alpha);
        return;
    }
    zn_glyph(c, 0x2594u, x0, y0, iw, 8 * t, rgb, alpha);               /* top */
    zn_glyph(c, 0x2581u, x0, y0 + ih - 8 * t, iw, 8 * t, rgb, alpha);  /* bottom */
    /* The uprights stop short of the corners the level bars own. */
    zn_glyph(c, 0x258fu, x0, y0 + t, 8 * t, ih - 2 * t, rgb, alpha);
    zn_glyph(c, 0x2595u, x0 + iw - 8 * t, y0 + t, 8 * t, ih - 2 * t, rgb, alpha);
}

/* Where the black circle's ink sits in its cell, as fractions of the
 * cell, measured once from the net's own coverage. */
static bool zn_disc_box(float *fx0, float *fy0, float *fx1, float *fy1)
{
    enum { W = 48, H = 96 };
    static float box[4];
    static int state;

    if (state == 0) {
        static float cov[W * H];
        int x;
        int y;
        int x0 = W;
        int y0 = H;
        int x1 = -1;
        int y1 = -1;

        state = -1;
        if (zell_neuronte_open() &&
            neuronte_coverage(zn_font, 0x25cfu, W, H, 0, cov) == 0) {
            for (y = 0; y < H; y++) {
                for (x = 0; x < W; x++) {
                    if (cov[y * W + x] > 0.5f) {
                        x0 = x < x0 ? x : x0;
                        y0 = y < y0 ? y : y0;
                        x1 = x > x1 ? x : x1;
                        y1 = y > y1 ? y : y1;
                    }
                }
            }
            if (x1 > x0 && y1 > y0) {
                box[0] = (float)x0 / W;
                box[1] = (float)y0 / H;
                box[2] = (float)(x1 + 1) / W;
                box[3] = (float)(y1 + 1) / H;
                state = 1;
            }
        }
    }
    if (state < 0) {
        return false;
    }
    *fx0 = box[0];
    *fy0 = box[1];
    *fx1 = box[2];
    *fy1 = box[3];
    return true;
}

/* The circle character in a cell sized so its ink fills the ellipse's box. */
static void zn_fill_ellipse(sr_canvas *c, float cx, float cy, float rx, float ry,
                            uint32_t rgb, float alpha)
{
    float fx0;
    float fy0;
    float fx1;
    float fy1;
    int cell_w;
    int cell_h;

    if (rx <= 0.0f || ry <= 0.0f || !zn_disc_box(&fx0, &fy0, &fx1, &fy1)) {
        return;
    }
    cell_w = zn_round(2.0f * rx / (fx1 - fx0));
    cell_h = zn_round(2.0f * ry / (fy1 - fy0));
    if (cell_w < 1 || cell_h < 1 || cell_w > 2048 || cell_h > 2048) {
        return;
    }
    zn_glyph(c, 0x25cfu, zn_round(cx - rx - fx0 * (float)cell_w),
             zn_round(cy - ry - fy0 * (float)cell_h), cell_w, cell_h, rgb, alpha);
}

static void zn_fill_circle(sr_canvas *c, float cx, float cy, float r,
                           uint32_t rgb, float alpha)
{
    zn_fill_ellipse(c, cx, cy, r, r, rgb, alpha);
}

/* ---- pictures ---- */

/* Which of a 2 x 4 cell's eight colours each of its pixels takes, from
 * three masks of the pixel net used as bit planes: the right column, the
 * two middle rows, and the lower half. Colour number = column + 2 * middle
 * + 4 * lower, so the sub-cells are numbered 0 1 / 2 3 / 6 7 / 4 5 and
 * neighbours differ in one plane. The masks are rounded: at this size
 * each sub-cell is one pixel. */
static const unsigned char *zn_cell_labels(void)
{
    static const uint32_t planes[3] = {0x2590u, 0x1cd33u, 0x2584u};
    static unsigned char labels[8];
    static int state;

    if (state == 0) {
        float cov[8];
        int plane;
        int i;

        state = -1;
        if (zell_neuronte_open()) {
            memset(labels, 0, sizeof labels);
            state = 1;
            for (plane = 0; plane < 3 && state > 0; plane++) {
                if (neuronte_coverage(zn_font, planes[plane], 2, 4, 0, cov) != 0) {
                    state = -1;
                    break;
                }
                for (i = 0; i < 8; i++) {
                    labels[i] = (unsigned char)(labels[i] | ((cov[i] > 0.5f) << plane));
                }
            }
        }
    }
    return state > 0 ? labels : NULL;
}

/* soft-raster's own mix, so the two renderers agree to the last bit. */
static void zn_composite(uint32_t *to, uint32_t s, int ga)
{
    int sa = ((int)(s >> 24) * ga) / 255;
    int inv;
    uint32_t d;
    int r;
    int g;
    int b;
    int a;

    if (sa <= 0) {
        return;
    }
    if (sa >= 255) {
        *to = s;
        return;
    }
    inv = 255 - sa;
    d = *to;
    r = ((int)((s >> 16) & 255u) * ga) / 255 + ((int)((d >> 16) & 255u) * inv) / 255;
    g = ((int)((s >> 8) & 255u) * ga) / 255 + ((int)((d >> 8) & 255u) * inv) / 255;
    b = ((int)(s & 255u) * ga) / 255 + ((int)(d & 255u) * inv) / 255;
    a = sa + ((int)(d >> 24) * inv) / 255;
    *to = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* A scaled picture, cell by cell. The picture's box is cut into 2 x 4
 * cells from its own corner. Sub-cell j of a cell is given the source
 * pixel that nearest-neighbour scaling puts there, as colour number
 * number[j]; then every pixel of the cell takes the colour whose number
 * the net's three masks give it. So pixel k shows sub-cell from[k], where
 * from[] is the masks' numbering turned back into sub-cells. */
static void zn_blit_scaled(sr_canvas *dst, const sr_canvas *src, int x, int y,
                           int w, int h, float alpha)
{
    static const unsigned char number[8] = {0, 1, 2, 3, 6, 7, 4, 5};
    enum { ZN_MAX_SIDE = 4096 };
    static int col_of[ZN_MAX_SIDE];
    const unsigned char *labels = zn_cell_labels();
    unsigned char from[8];
    int ga;
    int k;
    int j;
    int dx;
    int dy;
    int dx0;
    int dy0;
    int dx1;
    int dy1;

    if (labels == NULL || dst == NULL || src == NULL || dst->px == NULL ||
        src->px == NULL || w <= 0 || h <= 0 || w > ZN_MAX_SIDE || h > ZN_MAX_SIDE ||
        src->w <= 0 || src->h <= 0 || dst->px == src->px) {
        return;
    }
    ga = (int)((alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha) * 255.0f + 0.5f);
    if (ga <= 0) {
        return;
    }
    for (k = 0; k < 8; k++) {
        from[k] = 8;
        for (j = 0; j < 8; j++) {
            if (number[j] == labels[k]) {
                from[k] = (unsigned char)j;
            }
        }
        if (from[k] > 7) {
            return;
        }
    }
    dx0 = x < 0 ? -x : 0;
    dy0 = y < 0 ? -y : 0;
    dx1 = x + w > dst->w ? dst->w - x : w;
    dy1 = y + h > dst->h ? dst->h - y : h;
    /* The source column for each picture column, worked out once. */
    for (dx = dx0 & ~1; dx < w && dx <= (dx1 | 1); dx++) {
        col_of[dx] = (int)(((long long)dx * src->w) / w);
    }
    for (dy = dy0; dy < dy1; dy++) {
        int row_in_cell = dy & 3;
        int cell_y = dy - row_in_cell;
        uint32_t *out = dst->px + (size_t)(y + dy) * (size_t)dst->w + (size_t)x;
        const uint32_t *row_of[4];

        /* The source row behind each of the cell's four rows. A row below
         * the picture, in a part cell at its bottom edge, has none. */
        for (k = 0; k < 4; k++) {
            int py = cell_y + k;

            row_of[k] = py < h
                ? src->px + (size_t)(((long long)py * src->h) / h) * (size_t)src->w
                : NULL;
        }
        for (dx = dx0; dx < dx1; dx++) {
            int sub = from[row_in_cell * 2 + (dx & 1)];
            int px = (dx & ~1) + (sub & 1);
            const uint32_t *row = row_of[sub >> 1];

            if (px < w && row != NULL) {
                zn_composite(&out[dx], row[col_of[px]], ga);
            }
        }
    }
}

#define sr_fill_rect zn_fill_rect
#define sr_stroke_rect zn_stroke_rect
#define sr_fill_circle zn_fill_circle
#define sr_fill_ellipse zn_fill_ellipse
#define sr_blit_scaled zn_blit_scaled
#define sr_text zn_text
#define sr_text_outlined zn_text_outlined
#define sr_text_width_in(font, s, scale) zn_text_width((s), (scale))
#define sr_font_height(font) ((void)(font), (int)ZN_CELL_H)

#include "draw.c"
