#ifndef ZELL_CANVAS_H
#define ZELL_CANVAS_H

/* The pixel canvas of the Neuronte build.
 *
 * The Neuronte programs link this file in place of soft-raster. It holds
 * what plain code may still do there: make and free the pixel buffer, set
 * the clip, clear the frame, fill a rectangle with one colour, and hand
 * the pixels on as RGBA bytes or a PPM file. It draws no other shape.
 *
 * The struct is soft-raster's sr_canvas, so the objects the two renderers
 * share see one type. Each function gives the same result as the
 * soft-raster function named beside it, byte for byte; tests/test_canvas.c
 * runs the two side by side. Only the header of soft-raster is used here.
 * This file includes nothing else of Zell. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "soft_raster.h"

bool zell_canvas_init(sr_canvas *c, int w, int h);                    /* sr_canvas_init */
void zell_canvas_wrap(sr_canvas *c, uint32_t *mem, int w, int h);     /* sr_canvas_wrap */
void zell_canvas_free(sr_canvas *c);                                  /* sr_canvas_free */
void zell_canvas_set_clip(sr_canvas *c, int x, int y, int w, int h);  /* sr_canvas_set_clip */
void zell_canvas_reset_clip(sr_canvas *c);                            /* sr_canvas_reset_clip */
void zell_canvas_clear(sr_canvas *c, uint32_t rgb);                   /* sr_clear */
void zell_canvas_fill_rect(sr_canvas *c, float x, float y, float w, float h,
                           uint32_t rgb, float alpha);                /* sr_fill_rect */
bool zell_canvas_pack_rgba(const sr_canvas *c, uint8_t *rgba, size_t byte_count); /* sr_pack_rgba */
bool zell_canvas_write_ppm(const sr_canvas *c, const char *path);     /* sr_write_ppm */

#endif
