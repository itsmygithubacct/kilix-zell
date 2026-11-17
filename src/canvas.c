/* The pixel canvas of the Neuronte build. See canvas.h.
 *
 * The arithmetic is soft-raster's, kept step for step so that a frame is
 * the same bytes whichever of the two is linked: alpha is rounded to
 * 1/256 steps and each channel moves toward the colour with a shifted
 * integer mix.
 *
 * Adapted from soft-raster (MIT):
 *   Copyright (c) 2026 soft-raster contributors
 *   Copyright (c) 2026 terminal-lander contributors
 * That notice and the licence text are in the soft-raster checkout this
 * builds against, and in licenses/soft-raster of the runtime archive. */
#include "canvas.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

enum { PPM_CHUNK_PIXELS = 4096 };

static bool dimensions_ok(int w, int h, size_t *pixel_count)
{
    size_t pixels;

    if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / (size_t)h) {
        return false;
    }
    pixels = (size_t)w * (size_t)h;
    if (pixels > (size_t)INT_MAX || pixels > SIZE_MAX / sizeof(uint32_t)) {
        return false;
    }
    if (pixel_count != NULL) {
        *pixel_count = pixels;
    }
    return true;
}

static bool canvas_ok(const sr_canvas *c)
{
    return c != NULL && c->px != NULL && dimensions_ok(c->w, c->h, NULL);
}

static void whole_clip(sr_canvas *c)
{
    c->clip_x0 = 0;
    c->clip_y0 = 0;
    c->clip_x1 = c->w;
    c->clip_y1 = c->h;
}

/* ---- the buffer ---- */

bool zell_canvas_init(sr_canvas *c, int w, int h)
{
    size_t pixels;
    uint32_t *px;

    if (c == NULL) {
        return false;
    }
    *c = (sr_canvas){0};
    if (!dimensions_ok(w, h, &pixels)) {
        return false;
    }
    px = calloc(pixels, sizeof(uint32_t));
    if (px == NULL) {
        return false;
    }
    c->px = px;
    c->w = w;
    c->h = h;
    c->owns_px = true;
    whole_clip(c);
    return true;
}

void zell_canvas_wrap(sr_canvas *c, uint32_t *mem, int w, int h)
{
    if (c == NULL) {
        return;
    }
    *c = (sr_canvas){0};
    if (mem == NULL || !dimensions_ok(w, h, NULL)) {
        return;
    }
    c->px = mem;
    c->w = w;
    c->h = h;
    c->owns_px = false;
    whole_clip(c);
}

void zell_canvas_free(sr_canvas *c)
{
    if (c == NULL) {
        return;
    }
    if (c->owns_px) {
        free(c->px);
    }
    *c = (sr_canvas){0};
}

void zell_canvas_set_clip(sr_canvas *c, int x, int y, int w, int h)
{
    int64_t right;
    int64_t bottom;

    if (c == NULL) {
        return;
    }
    right = (int64_t)x + w;
    bottom = (int64_t)y + h;
    c->clip_x0 = x < 0 ? 0 : x > c->w ? c->w : x;
    c->clip_y0 = y < 0 ? 0 : y > c->h ? c->h : y;
    c->clip_x1 = right < 0 ? 0 : right > c->w ? c->w : (int)right;
    c->clip_y1 = bottom < 0 ? 0 : bottom > c->h ? c->h : (int)bottom;
    if (w <= 0 || c->clip_x1 < c->clip_x0) {
        c->clip_x1 = c->clip_x0;
    }
    if (h <= 0 || c->clip_y1 < c->clip_y0) {
        c->clip_y1 = c->clip_y0;
    }
}

void zell_canvas_reset_clip(sr_canvas *c)
{
    if (c != NULL) {
        whole_clip(c);
    }
}

/* ---- the clear and the flat fill ---- */

/* The clear covers the whole buffer; the clip does not bound it. */
void zell_canvas_clear(sr_canvas *c, uint32_t rgb)
{
    uint32_t value = 0xff000000u | (rgb & 0x00ffffffu);
    size_t n;
    size_t i;

    if (!canvas_ok(c)) {
        return;
    }
    n = (size_t)c->w * (size_t)c->h;
    for (i = 0u; i < n; i++) {
        c->px[i] = value;
    }
}

/* Move the pixel toward the colour by ai / 256. */
static void mix_px(sr_canvas *c, int x, int y, uint32_t rgb, int ai)
{
    uint32_t *p = &c->px[(size_t)y * (size_t)c->w + (size_t)x];
    uint32_t d = *p;
    int dr = (int)((d >> 16) & 255u);
    int dg = (int)((d >> 8) & 255u);
    int db = (int)(d & 255u);
    int da = (int)(d >> 24);
    int r = (int)((rgb >> 16) & 255u);
    int g = (int)((rgb >> 8) & 255u);
    int b = (int)(rgb & 255u);

    dr += ((r - dr) * ai) >> 8;
    dg += ((g - dg) * ai) >> 8;
    db += ((b - db) * ai) >> 8;
    da += ((255 - da) * ai) >> 8;
    *p = ((uint32_t)da << 24) | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
}

static int coverage_steps(float a)
{
    return a >= 1.0f ? 256 : (int)(a * 256.0f + 0.5f);
}

static void mix_px_by(sr_canvas *c, int x, int y, uint32_t rgb, float a)
{
    int ai;

    if (!(a > 0.0f)) {
        return;
    }
    ai = coverage_steps(a);
    if (ai <= 0) {
        return;
    }
    mix_px(c, x, y, rgb, ai);
}

/* A coordinate brought inside [lower, upper] and made an int, so the loops
 * are bounded and the casts hold. */
static int clip_lo(double v, int lower, int upper)
{
    const double rounded = floor(v);

    if (isnan(v) || rounded <= (double)lower) {
        return lower;
    }
    if (rounded >= (double)upper) {
        return upper;
    }
    return (int)rounded;
}

static int clip_hi(double v, int lower, int upper)
{
    const double rounded = ceil(v);

    if (isnan(v) || rounded <= (double)lower) {
        return lower;
    }
    if (rounded >= (double)upper) {
        return upper;
    }
    return (int)rounded;
}

/* Pixels the rectangle's side crosses: each takes the part of it covered. */
static void edge_span(sr_canvas *c, int px0, int px1, int py, float x, float xe,
                      uint32_t rgb, float alpha, float cy)
{
    int px;

    for (px = px0; px < px1; px++) {
        float cx = fminf((float)(px + 1), xe) - fmaxf((float)px, x);

        if (cx <= 0.0f) {
            continue;
        }
        if (cx > 1.0f) {
            cx = 1.0f;
        }
        mix_px_by(c, px, py, rgb, alpha * cx * cy);
    }
}

/* Pixels wholly inside: one alpha for all, and a plain store when opaque. */
static void inner_span(sr_canvas *c, int px0, int px1, int py, uint32_t rgb, float alpha)
{
    int px;

    if (px0 >= px1) {
        return;
    }
    if (alpha >= 1.0f) {
        const uint32_t value = 0xff000000u | (rgb & 0x00ffffffu);
        uint32_t *row = &c->px[(size_t)py * (size_t)c->w];

        for (px = px0; px < px1; px++) {
            row[px] = value;
        }
        return;
    }
    {
        const int ai = coverage_steps(alpha);

        if (ai <= 0) {
            return;
        }
        for (px = px0; px < px1; px++) {
            mix_px(c, px, py, rgb, ai);
        }
    }
}

void zell_canvas_fill_rect(sr_canvas *c, float x, float y, float w, float h,
                           uint32_t rgb, float alpha)
{
    int x0;
    int x1;
    int y0;
    int y1;
    int ix0;
    int ix1;
    int iy0;
    int iy1;
    int py;

    if (!canvas_ok(c) || !isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) ||
        w <= 0.0f || h <= 0.0f || !(alpha > 0.0f)) {
        return;
    }
    x0 = clip_lo(x, c->clip_x0, c->clip_x1);
    x1 = clip_hi(x + w, c->clip_x0, c->clip_x1);
    y0 = clip_lo(y, c->clip_y0, c->clip_y1);
    y1 = clip_hi(y + h, c->clip_y0, c->clip_y1);
    /* Columns and rows the rectangle covers from side to side. The cap
     * keeps the float casts of pixel coordinates exact. */
    ix0 = clip_hi((double)x, x0, x1);
    ix1 = clip_lo((double)(x + w), x0, x1);
    iy0 = clip_hi((double)y, y0, y1);
    iy1 = clip_lo((double)(y + h), y0, y1);
    if (ix1 > (1 << 24)) {
        ix1 = 1 << 24;
    }
    if (iy1 > (1 << 24)) {
        iy1 = 1 << 24;
    }
    if (ix0 > ix1) {
        ix0 = ix1;
    }
    if (iy0 > iy1) {
        iy0 = iy1;
    }
    for (py = y0; py < y1; py++) {
        float cy = fminf((float)(py + 1), y + h) - fmaxf((float)py, y);

        if (cy <= 0.0f) {
            continue;
        }
        if (cy > 1.0f) {
            cy = 1.0f;
        }
        if (py >= iy0 && py < iy1 && ix0 < ix1) {
            edge_span(c, x0, ix0, py, x, x + w, rgb, alpha, cy);
            inner_span(c, ix0, ix1, py, rgb, alpha);
            edge_span(c, ix1, x1, py, x, x + w, rgb, alpha, cy);
        } else {
            edge_span(c, x0, x1, py, x, x + w, rgb, alpha, cy);
        }
    }
}

/* ---- the pixels as bytes ---- */

bool zell_canvas_pack_rgba(const sr_canvas *c, uint8_t *rgba, size_t byte_count)
{
    size_t pixels;
    size_t i;

    if (!canvas_ok(c) || rgba == NULL || !dimensions_ok(c->w, c->h, &pixels)) {
        return false;
    }
    if (pixels > SIZE_MAX / 4u || byte_count < pixels * 4u) {
        return false;
    }
    for (i = 0u; i < pixels; i++) {
        uint32_t pixel = c->px[i];

        rgba[i * 4u] = (uint8_t)(pixel >> 16);
        rgba[i * 4u + 1u] = (uint8_t)(pixel >> 8);
        rgba[i * 4u + 2u] = (uint8_t)pixel;
        rgba[i * 4u + 3u] = (uint8_t)(pixel >> 24);
    }
    return true;
}

/* Binary PPM, three bytes a pixel; the alpha is not written. On failure
 * errno says why and a part file may be left. */
bool zell_canvas_write_ppm(const sr_canvas *c, const char *path)
{
    unsigned char bytes[PPM_CHUNK_PIXELS * 3];
    int failure = 0;
    bool ok = true;
    FILE *file;
    size_t n;
    size_t offset;

    if (!canvas_ok(c) || path == NULL) {
        errno = EINVAL;
        return false;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    errno = 0;
    if (fprintf(file, "P6\n%d %d\n255\n", c->w, c->h) < 0) {
        failure = errno != 0 ? errno : EIO;
        ok = false;
    }
    n = (size_t)c->w * (size_t)c->h;
    for (offset = 0u; ok && offset < n;) {
        const size_t remaining = n - offset;
        const size_t chunk = remaining < (size_t)PPM_CHUNK_PIXELS ? remaining : (size_t)PPM_CHUNK_PIXELS;
        size_t i;

        for (i = 0u; i < chunk; i++) {
            const uint32_t pixel = c->px[offset + i];

            bytes[i * 3u] = (unsigned char)(pixel >> 16);
            bytes[i * 3u + 1u] = (unsigned char)(pixel >> 8);
            bytes[i * 3u + 2u] = (unsigned char)pixel;
        }
        if (fwrite(bytes, 3u, chunk, file) != chunk) {
            failure = errno != 0 ? errno : EIO;
            ok = false;
        }
        offset += chunk;
    }
    if (ferror(file)) {
        if (failure == 0) {
            failure = errno != 0 ? errno : EIO;
        }
        ok = false;
    }
    if (fclose(file) != 0) {
        if (failure == 0) {
            failure = errno != 0 ? errno : EIO;
        }
        ok = false;
    }
    errno = ok ? 0 : failure != 0 ? failure : EIO;
    return ok;
}
