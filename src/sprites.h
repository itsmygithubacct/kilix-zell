#ifndef ZELL_SPRITES_H
#define ZELL_SPRITES_H

#include <stdint.h>

/* Straight-alpha 0xAARRGGBB pixels, row major. */

int zell_kilix_width(void);
int zell_kilix_height(void);
int zell_kilix_foot(void);
int zell_wizard_width(void);
int zell_wizard_height(void);

/* direction: 0 down, 1 up, 2 left, 3 right. step is 0 or 1. */
const uint32_t *zell_kilix_sprite(int direction, int step);
const uint32_t *zell_wizard_sprite(void);

#endif
