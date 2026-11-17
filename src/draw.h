#ifndef ZELL_DRAW_H
#define ZELL_DRAW_H

#include "scene.h"
#include "soft_raster.h"

#ifdef ZELL_RENDER_NEURONTE
/* The Neuronte programs do not link soft-raster. The canvas calls the
 * shared sources make by its names go to Zell's own canvas. A call with no
 * line here has no body in that build, and the link says so. */
#include "canvas.h"
#define sr_canvas_init zell_canvas_init
#define sr_canvas_wrap zell_canvas_wrap
#define sr_canvas_free zell_canvas_free
#define sr_canvas_set_clip zell_canvas_set_clip
#define sr_canvas_reset_clip zell_canvas_reset_clip
#define sr_clear zell_canvas_clear
#define sr_pack_rgba zell_canvas_pack_rgba
#define sr_write_ppm zell_canvas_write_ppm
#endif

void zell_draw(const zell_scene *scene, sr_canvas *canvas);

/* How many answer lines sit below the box. Zero when the reply fits. */
int zell_answer_scroll_limit(int width, int height, const char *text);

/* Campfire index 0 is the left fire and 1 is the right. The box is a
 * room cell, so it does not follow Kilix. */
void zell_campfire_rect(int canvas_w, int canvas_h, int index,
                        int *x, int *y, int *width, int *height);

int zell_service_scroll_limit(const zell_scene *scene);

#endif
