#ifndef ZELL_DRAW_H
#define ZELL_DRAW_H

#include "scene.h"
#include "soft_raster.h"

void zell_draw(const zell_scene *scene, sr_canvas *canvas);

/* How many answer lines sit below the box. Zero when the reply fits. */
int zell_answer_scroll_limit(int width, int height, const char *text);

/* Campfire index 0 is the left fire and 1 is the right. The box is a
 * room cell, so it does not follow Kilix. */
void zell_campfire_rect(int canvas_w, int canvas_h, int index,
                        int *x, int *y, int *width, int *height);

#endif
