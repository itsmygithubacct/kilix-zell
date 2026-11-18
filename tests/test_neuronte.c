/* Checks of the Neuronte renderer that need the weight file. They are
 * skipped, with a message and a zero exit, when it cannot be loaded. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desktop.h"
#include "draw_neuronte.h"
#include "room.h"
#include "scene.h"
#include "soft_raster.h"

static int failures;

static void expect(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static int differs(const sr_canvas *a, const sr_canvas *b, int x0, int y0, int x1, int y1)
{
    int x;
    int y;
    int count = 0;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            count += a->px[y * a->w + x] != b->px[y * b->w + x];
        }
    }
    return count;
}

/* The answer's text is drawn by a net and stays inside the lines the
 * layout gives it: a longer answer changes pixels, and none of them lie
 * above the first text line. */
static void test_answer_text_is_drawn(void)
{
    zell_scene scene;
    sr_canvas blank;
    sr_canvas text;

    expect(sr_canvas_init(&blank, 320, 240) && sr_canvas_init(&text, 320, 240),
           "canvases allocate");
    zell_scene_init(&scene, 320, 240);
    scene.mode = ZELL_MODE_ANSWER;
    snprintf(scene.answerer, sizeof scene.answerer, "Kilix Ask");
    scene.answer[0] = '\0';
    zell_draw(&scene, &blank);
    snprintf(scene.answer, sizeof scene.answer, "Hello from the wizard");
    zell_draw(&scene, &text);

    expect(differs(&blank, &text, 0, 0, 320, 240) > 100, "the answer puts ink on the frame");
    expect(differs(&blank, &text, 0, 0, 320, 40) == 0, "no ink above the first answer line");
    expect(differs(&blank, &text, 0, 80, 320, 240) == 0, "a one-line answer stays on its line");
    sr_canvas_free(&blank);
    sr_canvas_free(&text);
}

/* A character the nets were not trained on still draws something, and
 * a reply in UTF-8 wraps by characters, not bytes. */
static void test_untrained_and_utf8(void)
{
    zell_scene scene;
    sr_canvas blank;
    sr_canvas text;
    const char *accents = "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";

    expect(sr_canvas_init(&blank, 320, 240) && sr_canvas_init(&text, 320, 240),
           "canvases allocate");
    zell_scene_init(&scene, 320, 240);
    scene.mode = ZELL_MODE_ANSWER;
    scene.answer[0] = '\0';
    zell_draw(&scene, &blank);
    snprintf(scene.answer, sizeof scene.answer, "\xe4\xb8\xad");
    zell_draw(&scene, &text);
    expect(differs(&blank, &text, 0, 0, 320, 240) > 0, "an untrained character leaves a mark");

    expect(zell_answer_scroll_limit(320, 240, accents) == 0, "eight accented letters fit one box");
    sr_canvas_free(&blank);
    sr_canvas_free(&text);
}

/* The sign's word is ink on the board: pixels between the board's
 * colours and the ink colour, darker than any board colour. */
static void test_sign_reads_voice(void)
{
    zell_scene scene;
    sr_canvas canvas;
    float sx;
    float sy;
    int x;
    int y;
    int dark = 0;

    expect(sr_canvas_init(&canvas, 320, 240), "sign canvas allocates");
    zell_scene_init(&scene, 320, 240);
    zell_draw(&scene, &canvas);
    zell_switch_point(scene.width, scene.height, &sx, &sy);
    for (y = 0; y < (int)sy - 11; y++) {
        for (x = (int)sx - 40; x < (int)sx + 40; x++) {
            uint32_t p = x >= 0 && x < canvas.w ? canvas.px[y * canvas.w + x] : 0xffffffu;
            unsigned r = (p >> 16) & 0xffu;
            unsigned g = (p >> 8) & 0xffu;
            unsigned b = p & 0xffu;

            /* On the line from board brown 8d5a32 to ink 24160c, at least
             * half way to the ink. No cave tile or board band is there. */
            dark += r >= 0x24u && r <= 0x5cu && g >= 0x16u && g <= 0x3au &&
                    b >= 0x0cu && b <= 0x22u && r > g && g > b;
        }
    }
    expect(dark > 20, "the sign reads voice");
    sr_canvas_free(&canvas);
}

/* A wall tile comes through the octant cells pixel for pixel: each pixel
 * of the frame is the tile pixel that nearest-neighbour scaling puts
 * there. This holds only if the pixel net's three masks at 2 x 4 name the
 * right colour for all eight sub-cells. Odd sizes leave part cells at the
 * tile's right and bottom edges, which are checked too. */
static void test_tiles_are_pixel_exact(void)
{
    static const int sizes[][2] = {{320, 240}, {163, 121}, {1001, 613}};
    const uint32_t *tile = zell_tile_wall();
    int edge = zell_tile_edge();
    size_t i;

    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        int w = sizes[i][0];
        int h = sizes[i][1];
        int col = ZELL_ROOM_COLS - 1;
        int row = 4;
        int x0 = col * w / ZELL_ROOM_COLS;
        int x1 = (col + 1) * w / ZELL_ROOM_COLS;
        int y0 = row * h / ZELL_ROOM_ROWS;
        int y1 = (row + 1) * h / ZELL_ROOM_ROWS;
        zell_scene scene;
        sr_canvas canvas;
        int wrong = 0;
        int x;
        int y;

        expect(zell_room_mark(col, row) == 'x', "the checked cell is rock wall");
        expect(sr_canvas_init(&canvas, w, h), "tile canvas allocates");
        zell_scene_init(&scene, w, h);
        zell_draw(&scene, &canvas);
        for (y = y0; y < y1; y++) {
            for (x = x0; x < x1; x++) {
                int sx = (x - x0) * edge / (x1 - x0);
                int sy = (y - y0) * edge / (y1 - y0);

                wrong += (canvas.px[y * w + x] & 0xffffffu) !=
                         (tile[sy * edge + sx] & 0xffffffu);
            }
        }
        expect(wrong == 0, "a wall tile is exact through the octant cells");
        sr_canvas_free(&canvas);
    }
}

/* The Neuronte build paints the measured frame rate in the top right
 * and leaves the rest of the picture alone. */
static void test_framerate_is_shown(void)
{
    zell_scene scene;
    sr_canvas off;
    sr_canvas on;

    expect(sr_canvas_init(&off, 320, 240) && sr_canvas_init(&on, 320, 240),
           "framerate canvases allocate");
    zell_scene_init(&scene, 320, 240);
    zell_draw(&scene, &off);
    scene.frame_rate = 60;
    zell_draw(&scene, &on);
    expect(differs(&off, &on, 240, 0, 320, 28) > 20,
           "the frame rate is in the top right");
    expect(differs(&off, &on, 0, 40, 320, 240) == 0,
           "the frame rate stays in the corner");
    sr_canvas_free(&off);
    sr_canvas_free(&on);
}

/* Every screen draws, and the same scene gives the same frame twice. */
static void test_screens_repeat(void)
{
    static const int modes[] = {ZELL_MODE_WALK, ZELL_MODE_QUESTION, ZELL_MODE_TYPE,
                                ZELL_MODE_WAIT, ZELL_MODE_ANSWER};
    zell_scene scene;
    sr_canvas a;
    sr_canvas b;
    size_t i;

    expect(sr_canvas_init(&a, 480, 300) && sr_canvas_init(&b, 480, 300), "canvases allocate");
    zell_scene_init(&scene, 480, 300);
    snprintf(scene.question, sizeof scene.question, "why");
    scene.question_len = 3;
    snprintf(scene.answer, sizeof scene.answer, "because");
    for (i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        scene.mode = modes[i];
        zell_draw(&scene, &a);
        zell_draw(&scene, &b);
        expect(differs(&a, &b, 0, 0, 480, 300) == 0, "a screen draws the same twice");
    }
    sr_canvas_free(&a);
    sr_canvas_free(&b);
}

static int like(uint32_t p, uint32_t rgb)
{
    int dr = (int)((p >> 16) & 255u) - (int)((rgb >> 16) & 255u);
    int dg = (int)((p >> 8) & 255u) - (int)((rgb >> 8) & 255u);
    int db = (int)(p & 255u) - (int)(rgb & 255u);

    return abs(dr) + abs(dg) + abs(db);
}

/* The stair marker downstairs is the net's down-pointing triangle, fitted
 * to the box the raster build's triangle has: 28 wide at a level top two
 * pixels under the band's top edge, 20 high, coming to a point below the
 * middle of the stairs. At the world's own size the room is not moved. The
 * Command Center's bottom edge still leads out but carries no marker. */
static void test_stair_marker_is_a_triangle(void)
{
    enum { W = ZELL_WORLD_W, H = ZELL_WORLD_H, ROWS = 32, REACH = 24 };
    const uint32_t gold = 0xd7b56au;
    const int top = (int)(0.93f * (float)H) + 2;
    const int mid = (int)((ZELL_STAIRS_LEFT + ZELL_STAIRS_RIGHT) * 0.5f * (float)W);
    int left[ROWS];
    int right[ROWS];
    zell_scene scene;
    sr_canvas c;
    int room;

    expect(sr_canvas_init(&c, W, H), "marker canvas allocates");
    expect(mid == 768 && top == 954, "the stairs are in the middle, the band near the foot");
    for (room = 1; room <= 2; room++) {
        int first = -1;
        int last = -1;
        int total = 0;
        int grows = 0;
        int off_centre = 0;
        int row;
        int x;

        zell_scene_init(&scene, W, H);
        zell_desktop_init(&scene);
        scene.room = room;
        scene.disk_count = 3;
        scene.disks_ready = true;
        scene.x = (float)W * 0.25f;
        scene.y = (float)H * 0.50f;
        zell_draw(&scene, &c);
        for (row = 0; row < ROWS; row++) {
            int y = top - 6 + row;

            left[row] = -1;
            right[row] = -2;
            for (x = mid - REACH; x < mid + REACH; x++) {
                if (like(c.px[y * W + x], gold) < 30) {
                    left[row] = left[row] < 0 ? x : left[row];
                    right[row] = x;
                    total++;
                }
            }
            if (left[row] >= 0) {
                first = first < 0 ? row : first;
                last = row;
            }
        }
        if (room == 1) {
            expect(first < 0 && total == 0, "the Command Center's way out is unmarked");
            continue;
        }
        expect(first >= 0, "the marker is drawn");
        if (first < 0) {
            continue;
        }
        for (row = first; row <= last; row++) {
            int width = right[row] - left[row] + 1;

            if (row > first && width > right[row - 1] - left[row - 1] + 1) {
                grows++;
            }
            /* Ink from left to right is the same distance either side of mid. */
            if (width < 1 || abs(left[row] + right[row] + 1 - 2 * mid) > 2) {
                off_centre++;
            }
        }
        expect(abs(top - 6 + first - top) <= 1, "its top is two pixels under the band's edge");
        expect(last - first + 1 >= 18 && last - first + 1 <= 21, "it is 20 rows high, less a faint one");
        expect(right[first] - left[first] + 1 >= 24 && right[first] - left[first] + 1 <= 28,
               "its top row is the widest, 28 less the soft ends");
        expect(right[last] - left[last] + 1 <= 3, "it comes to a point at the foot");
        expect(grows == 0, "no row is wider than the one above");
        expect(off_centre == 0, "every row is centred on the stairs");
        expect(total >= 230 && total <= 290, "its area is half its box");
    }
    sr_canvas_free(&c);
}

/* The floating window's close button is the net's multiplication sign in
 * the square whose corners the raster build's two strokes join, five
 * pixels out from the button's middle: ink along both diagonals and
 * through the middle, none at the middle of a side, nothing past the
 * square but a soft edge. */
static void test_close_button_is_a_cross(void)
{
    enum { REACH = 5 };
    const uint32_t ink = 0xabc8c0u;
    const uint32_t ground = 0x1b3a42u;
    zell_scene scene;
    zell_terminal_layout layout;
    sr_canvas c;
    int cols;
    int rows;
    int cx;
    int cy;
    int dx;
    int dy;
    int i;
    int inked = 0;
    int on_diagonals = 0;
    int at_sides = 0;
    int outside = 0;
    int quarter[4] = {0, 0, 0, 0};

    expect(sr_canvas_init(&c, 1000, 700), "button canvas allocates");
    zell_scene_init(&scene, 1000, 700);
    zell_desktop_init(&scene);
    zell_terminal_cells(&scene, &cols, &rows);
    scene.terminal_width = cols * 8;
    scene.terminal_height = rows * 16;
    scene.terminal_pixels = calloc((size_t)scene.terminal_width * (size_t)scene.terminal_height,
                                   sizeof(uint32_t));
    expect(scene.terminal_pixels != NULL, "window pixels allocate");
    if (scene.terminal_pixels == NULL) {
        sr_canvas_free(&c);
        return;
    }
    for (i = 0; i < scene.terminal_width * scene.terminal_height; i++) {
        scene.terminal_pixels[i] = 0xff0b1418u;
    }
    scene.mode = ZELL_MODE_SERVICE;
    scene.service_phase = 5;
    zell_draw(&scene, &c);
    zell_terminal_layout_for(&scene, c.w, c.h, &layout);
    cx = layout.close.x + layout.close.w / 2;
    cy = layout.close.y + layout.close.h / 2;
    expect(layout.close.w >= 2 * REACH + 6 && layout.close.h >= 2 * REACH + 6, "the button holds the cross");
    /* Pixel cx + d covers d to d + 1 from the middle, so the square is d = -REACH .. REACH - 1. */
    for (dy = -REACH - 3; dy <= REACH + 3; dy++) {
        for (dx = -REACH - 3; dx <= REACH + 3; dx++) {
            uint32_t p = c.px[(cy + dy) * c.w + cx + dx];
            int in_square = dx >= -REACH && dx < REACH && dy >= -REACH && dy < REACH;
            int near_square = dx >= -REACH - 1 && dx <= REACH + 1 && dy >= -REACH - 1 && dy <= REACH + 1;

            if (in_square && like(p, ink) < like(p, ground)) {
                inked++;
                quarter[(dy >= 0) * 2 + (dx >= 0)]++;
            }
            if (!near_square && like(p, ground) > 12) {
                outside++;
            }
        }
    }
    for (i = 1; i <= REACH - 1; i++) {
        on_diagonals += like(c.px[(cy - i) * c.w + cx - i], ink) < 60;
        on_diagonals += like(c.px[(cy - i) * c.w + cx + i - 1], ink) < 60;
        on_diagonals += like(c.px[(cy + i - 1) * c.w + cx - i], ink) < 60;
        on_diagonals += like(c.px[(cy + i - 1) * c.w + cx + i - 1], ink) < 60;
    }
    for (i = -1; i <= 0; i++) {
        at_sides += like(c.px[(cy - REACH) * c.w + cx + i], ground) > 40;
        at_sides += like(c.px[(cy + REACH - 1) * c.w + cx + i], ground) > 40;
        at_sides += like(c.px[(cy + i) * c.w + cx - REACH], ground) > 40;
        at_sides += like(c.px[(cy + i) * c.w + cx + REACH - 1], ground) > 40;
    }
    expect(on_diagonals == 4 * (REACH - 1), "ink runs along both diagonals from the middle");
    expect(at_sides == 0, "the middle of each side is clear");
    expect(inked >= 60 && inked <= 88, "the strokes are about three pixels thick");
    expect(abs(quarter[0] - quarter[1]) <= 4 && abs(quarter[2] - quarter[3]) <= 4 &&
           abs(quarter[0] - quarter[2]) <= 4 && quarter[0] >= 8, "the four arms are alike");
    expect(outside == 0, "past its square the cross has a soft edge of one pixel at most");
    {
        /* Where the ink's weight lies. The cell is placed on whole pixels,
         * and here that puts the sign half a pixel left of the button's
         * middle and level with it. The sums are twice the offset, so a
         * cross one pixel away from there is off by two. */
        long weight = 0;
        long across = 0;
        long down = 0;

        for (dy = -REACH - 2; dy <= REACH + 2; dy++) {
            for (dx = -REACH - 2; dx <= REACH + 2; dx++) {
                int w = like(c.px[(cy + dy) * c.w + cx + dx], ground);

                weight += w;
                across += (long)w * (2 * dx + 1);
                down += (long)w * (2 * dy + 1);
            }
        }
        expect(weight > 0 && labs(across + weight) * 5 <= weight * 2, "the cross is half a pixel left of the middle");
        expect(weight > 0 && down * 10 >= -3 * weight && down * 10 <= 7 * weight, "the cross is level with the middle");
    }
    free(scene.terminal_pixels);
    sr_canvas_free(&c);
}

/* The disc and the ellipse are the black circle fitted to the shape's box.
 * The counts are what the reviewed weights give; a cell one pixel off in
 * size or place changes them. */
static void test_ellipse_fills_its_box(void)
{
    static const struct { float cx, cy, rx, ry; int x0, x1, y0, y1, inked; } want[] = {
        {60.0f, 40.0f, 40.0f, 20.0f, 20, 100, 20, 59, 2581},
        {60.0f, 40.0f, 17.0f, 5.0f, 43, 76, 35, 44, 272},
        {60.5f, 40.5f, 23.0f, 7.0f, 37, 83, 33, 47, 533},
    };
    sr_canvas c;
    size_t i;

    expect(sr_canvas_init(&c, 120, 80), "ellipse canvas allocates");
    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        int x0 = c.w;
        int y0 = c.h;
        int x1 = -1;
        int y1 = -1;
        int inked = 0;
        int x;
        int y;

        sr_clear(&c, 0x000000u);
        zell_neuronte_ellipse(&c, want[i].cx, want[i].cy, want[i].rx, want[i].ry, 0xffffffu, 1.0f);
        for (y = 0; y < c.h; y++) {
            for (x = 0; x < c.w; x++) {
                if ((c.px[y * c.w + x] & 255u) >= 128u) {
                    x0 = x < x0 ? x : x0;
                    y0 = y < y0 ? y : y0;
                    x1 = x > x1 ? x : x1;
                    y1 = y > y1 ? y : y1;
                    inked++;
                }
            }
        }
        if (x0 != want[i].x0 || x1 != want[i].x1 || y0 != want[i].y0 || y1 != want[i].y1 ||
            inked != want[i].inked) {
            fprintf(stderr, "ellipse %d: x %d..%d y %d..%d, %d pixels\n", (int)i, x0, x1, y0, y1, inked);
        }
        expect(x0 == want[i].x0 && x1 == want[i].x1 && y0 == want[i].y0 && y1 == want[i].y1,
               "the ellipse's ink is in its box");
        expect(inked == want[i].inked, "the ellipse has its area");
        /* The pinned numbers themselves stay within a pixel of the box asked for. */
        expect(abs(want[i].x0 - (int)(want[i].cx - want[i].rx)) <= 1 &&
               abs(want[i].x1 + 1 - (int)(want[i].cx + want[i].rx)) <= 1 &&
               abs(want[i].y0 - (int)(want[i].cy - want[i].ry)) <= 1 &&
               abs(want[i].y1 + 1 - (int)(want[i].cy + want[i].ry)) <= 1,
               "the pinned box is the box asked for, to a pixel");
    }
    sr_canvas_free(&c);
}

/* A shape's place in its cell is measured from the open weights, and
 * closing them forgets it. A weight file that could not be read must not
 * leave the shape undrawn once a good file is open. This has to run before
 * anything else draws an ellipse: a shape already measured is not measured
 * again while the weights stay open. */
static int ellipse_pixels(sr_canvas *c)
{
    int inked = 0;
    int i;

    sr_clear(c, 0x000000u);
    zell_neuronte_ellipse(c, 60.0f, 40.0f, 40.0f, 20.0f, 0xffffffu, 1.0f);
    for (i = 0; i < c->w * c->h; i++) {
        inked += (c->px[i] & 255u) >= 128u;
    }
    return inked;
}

static void test_shapes_are_measured_again_after_close(void)
{
    static char saved[4096];
    const char *was = getenv("ZELL_NEURONTE_WEIGHTS");
    bool had = was != NULL && strlen(was) < sizeof saved;
    sr_canvas c;

    if (was != NULL && !had) {
        return;
    }
    if (had) {
        strcpy(saved, was);
    }
    expect(sr_canvas_init(&c, 120, 80), "canvas for the reopened weights allocates");
    zell_neuronte_close();
    setenv("ZELL_NEURONTE_WEIGHTS", "/nonexistent/zell-test.nrt", 1);
    expect(ellipse_pixels(&c) == 0, "with no weights no ellipse is drawn");
    zell_neuronte_close();
    if (had) {
        setenv("ZELL_NEURONTE_WEIGHTS", saved, 1);
    } else {
        unsetenv("ZELL_NEURONTE_WEIGHTS");
    }
    expect(zell_neuronte_open(), "the weights open again");
    expect(ellipse_pixels(&c) == 2581, "the ellipse is measured again after a failed open");
    sr_canvas_free(&c);
}

int main(void)
{
    if (!zell_neuronte_open()) {
        printf("test-neuronte: skipped, cannot load %s\n", zell_neuronte_weights());
        return 0;
    }
    /* First, while no shape has been measured yet. */
    test_shapes_are_measured_again_after_close();
    test_answer_text_is_drawn();
    test_untrained_and_utf8();
    test_sign_reads_voice();
    test_tiles_are_pixel_exact();
    test_framerate_is_shown();
    test_stair_marker_is_a_triangle();
    test_close_button_is_a_cross();
    test_ellipse_fills_its_box();
    test_screens_repeat();
    zell_neuronte_close();
    if (failures) {
        fprintf(stderr, "test-neuronte: %d failed\n", failures);
        return 1;
    }
    printf("test-neuronte: ok\n");
    return 0;
}
