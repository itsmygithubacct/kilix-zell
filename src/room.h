#ifndef ZELL_ROOM_H
#define ZELL_ROOM_H

#include <stdint.h>

/* Shellda cave tiles and the Legend of Kilix campfire.
 * Straight alpha, 0xAARRGGBB, row major. */

int zell_tile_edge(void);
const uint32_t *zell_tile_stone(void);
const uint32_t *zell_tile_wall(void);
const uint32_t *zell_tile_dark(void);
const uint32_t *zell_tile_stairs(void);

int zell_heart_edge(void);
const uint32_t *zell_heart(void);

int zell_campfire_edge(void);
int zell_campfire_frames(void);
const uint32_t *zell_campfire(void);
const uint32_t *zell_campfire_frame(int frame);

/* Shellda's starting cave, 10 by 8.
 * x rock wall, d dark alcove, s stone, u stairs. */
enum { ZELL_ROOM_COLS = 10, ZELL_ROOM_ROWS = 8 };

static inline char zell_room_mark(int col, int row)
{
    static const char room[ZELL_ROOM_ROWS][ZELL_ROOM_COLS + 1] = {
        "xxxxxxxxxx",
        "xxddddddxx",
        "xssssssssx",
        "xssssssssx",
        "xssssssssx",
        "xssssssssx",
        "xssssssssx",
        "xxxxuuxxxx",
    };

    if (col < 0 || row < 0 || col >= ZELL_ROOM_COLS || row >= ZELL_ROOM_ROWS) {
        return 'x';
    }
    return room[row][col];
}

/* Rock and the dark alcove stop the walk. Stone and the stairs do not. */
static inline int zell_room_solid(int col, int row)
{
    char mark = zell_room_mark(col, row);
    return mark == 'x' || mark == 'd';
}

#endif
