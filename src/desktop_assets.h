#ifndef ZELL_DESKTOP_ASSETS_H
#define ZELL_DESKTOP_ASSETS_H
#include <stdint.h>
#define ZELL_ART_COUNT 33
/* Premultiplied ARGB, already filtered to the fixed world display size. */
typedef struct { int width, height; uint32_t *pixels; } zell_art;
const zell_art *zell_art_get(int id);
#endif
