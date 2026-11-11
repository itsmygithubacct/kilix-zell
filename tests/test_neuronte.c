/* Checks of the Neuronte renderer that need the weight file. They are
 * skipped, with a message and a zero exit, when it cannot be loaded. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    if (!zell_neuronte_open()) {
        printf("test-neuronte: skipped, cannot load %s\n", zell_neuronte_weights());
        return 0;
    }
    test_answer_text_is_drawn();
    test_untrained_and_utf8();
    test_sign_reads_voice();
    test_tiles_are_pixel_exact();
    test_screens_repeat();
    zell_neuronte_close();
    if (failures) {
        fprintf(stderr, "test-neuronte: %d failed\n", failures);
        return 1;
    }
    printf("test-neuronte: ok\n");
    return 0;
}
