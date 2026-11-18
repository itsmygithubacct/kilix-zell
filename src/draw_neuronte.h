#ifndef ZELL_DRAW_NEURONTE_H
#define ZELL_DRAW_NEURONTE_H

#include <stdbool.h>
#include <stdint.h>

#include "draw.h"

/* The Neuronte renderer gives the functions in draw.h. These three are
 * its own. Opening is also done on the first string drawn; a program
 * calls zell_neuronte_open() first to find out whether the weights load. */
const char *zell_neuronte_weights(void);
bool zell_neuronte_open(void);
void zell_neuronte_close(void);

/* The ellipse as this renderer draws it for draw.c, by itself: the black
 * circle fitted to the ellipse's box. The tests measure it. */
/* The file that draws this character as text, by its name: the weight
 * file, a listed bundle beside it, or "mark" for the untrained mark. NULL
 * when the weights do not open. */
const char *zell_neuronte_text_net(uint32_t scalar);
/* Text as the scene draws it, SCALE times the 8x16 cell. */
void zell_neuronte_text(sr_canvas *canvas, float x, float y, const char *text,
                        uint32_t rgb, float alpha, int scale);

void zell_neuronte_ellipse(sr_canvas *canvas, float cx, float cy, float rx, float ry,
                           uint32_t rgb, float alpha);

#endif
