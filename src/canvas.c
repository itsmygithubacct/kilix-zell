/* The pixel canvas of the Neuronte build: Zell's names and soft-raster's
 * struct over the canvas of neuronte-sdk. See canvas.h.
 *
 * The buffer, the clear, the flat fill, the RGBA bytes and the PPM writer
 * are neuronte-sdk's. This file only carries a canvas across: the shared
 * sources hold an sr_canvas, and the SDK takes its own struct with the same
 * fields, so each call copies the fields over, makes the SDK's call and
 * copies back what it may have changed. The pixels are never copied. */
#include "canvas.h"

#include <errno.h>

#include "neuronte_sdk_canvas.h"

static nsdk_canvas to_sdk(const sr_canvas *c)
{
    return (nsdk_canvas){
        .px = c->px,
        .w = c->w,
        .h = c->h,
        .clip_x0 = c->clip_x0,
        .clip_y0 = c->clip_y0,
        .clip_x1 = c->clip_x1,
        .clip_y1 = c->clip_y1,
        .owns_px = c->owns_px,
    };
}

static void from_sdk(sr_canvas *c, const nsdk_canvas *n)
{
    *c = (sr_canvas){
        .px = n->px,
        .w = n->w,
        .h = n->h,
        .clip_x0 = n->clip_x0,
        .clip_y0 = n->clip_y0,
        .clip_x1 = n->clip_x1,
        .clip_y1 = n->clip_y1,
        .owns_px = n->owns_px,
    };
}

bool zell_canvas_init(sr_canvas *c, int w, int h)
{
    nsdk_canvas n;
    bool made;

    if (c == NULL) {
        return false;
    }
    made = nsdk_canvas_init(&n, w, h);
    from_sdk(c, &n);
    return made;
}

void zell_canvas_wrap(sr_canvas *c, uint32_t *mem, int w, int h)
{
    nsdk_canvas n;

    if (c == NULL) {
        return;
    }
    nsdk_canvas_wrap(&n, mem, w, h);
    from_sdk(c, &n);
}

void zell_canvas_free(sr_canvas *c)
{
    nsdk_canvas n;

    if (c == NULL) {
        return;
    }
    n = to_sdk(c);
    nsdk_canvas_free(&n);
    from_sdk(c, &n);
}

void zell_canvas_set_clip(sr_canvas *c, int x, int y, int w, int h)
{
    nsdk_canvas n;

    if (c == NULL) {
        return;
    }
    n = to_sdk(c);
    nsdk_canvas_set_clip(&n, x, y, w, h);
    from_sdk(c, &n);
}

void zell_canvas_reset_clip(sr_canvas *c)
{
    nsdk_canvas n;

    if (c == NULL) {
        return;
    }
    n = to_sdk(c);
    nsdk_canvas_reset_clip(&n);
    from_sdk(c, &n);
}

void zell_canvas_clear(sr_canvas *c, uint32_t rgb)
{
    nsdk_canvas n;

    if (c == NULL) {
        return;
    }
    n = to_sdk(c);
    nsdk_canvas_clear(&n, rgb);
}

void zell_canvas_fill_rect(sr_canvas *c, float x, float y, float w, float h,
                           uint32_t rgb, float alpha)
{
    nsdk_canvas n;

    if (c == NULL) {
        return;
    }
    n = to_sdk(c);
    nsdk_canvas_fill_rect(&n, x, y, w, h, rgb, alpha);
}

bool zell_canvas_pack_rgba(const sr_canvas *c, uint8_t *rgba, size_t byte_count)
{
    nsdk_canvas n;

    if (c == NULL) {
        return false;
    }
    n = to_sdk(c);
    return nsdk_canvas_pack_rgba(&n, rgba, byte_count);
}

bool zell_canvas_write_ppm(const sr_canvas *c, const char *path)
{
    nsdk_canvas n;

    if (c == NULL) {
        errno = EINVAL;
        return false;
    }
    n = to_sdk(c);
    return nsdk_canvas_write_ppm(&n, path);
}
