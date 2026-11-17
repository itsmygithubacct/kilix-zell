/* Zell's canvas beside soft-raster's. The Neuronte programs link canvas.c
 * where the raster programs link soft-raster, and a frame must be the same
 * bytes either way, so every call here is made on both and compared. A few
 * results are also written out as numbers, so the pair cannot drift
 * together. */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "canvas.h"

static int failures;

static void expect(int ok, const char *what)
{
    if (!ok) {
        if (failures < 20) {
            fprintf(stderr, "FAIL: %s\n", what);
        }
        failures++;
    }
}

static uint32_t state = 0x2545f491u;

static uint32_t next(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static int same_struct(const sr_canvas *a, const sr_canvas *b)
{
    return a->w == b->w && a->h == b->h && a->clip_x0 == b->clip_x0 &&
           a->clip_y0 == b->clip_y0 && a->clip_x1 == b->clip_x1 &&
           a->clip_y1 == b->clip_y1 && a->owns_px == b->owns_px &&
           (a->px == NULL) == (b->px == NULL);
}

static int same_pixels(const sr_canvas *a, const sr_canvas *b)
{
    return a->w == b->w && a->h == b->h &&
           memcmp(a->px, b->px, (size_t)a->w * (size_t)a->h * sizeof(uint32_t)) == 0;
}

static uint32_t sum(const sr_canvas *c)
{
    size_t n = (size_t)c->w * (size_t)c->h;
    uint32_t total = 0u;
    size_t i;

    for (i = 0; i < n; i++) {
        total = total * 31u + c->px[i];
    }
    return total;
}

/* The same arbitrary pixels in both, alpha included. */
static void scribble(sr_canvas *a, sr_canvas *b)
{
    size_t n = (size_t)a->w * (size_t)a->h;
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t v = next();

        if ((v & 7u) == 0u) {
            v |= 0xff000000u;
        }
        a->px[i] = v;
        b->px[i] = v;
    }
}

static void test_buffer(void)
{
    static const int sizes[][2] = {
        {1, 1}, {7, 5}, {64, 48}, {0, 4}, {4, 0}, {-3, 4}, {4, -3},
        {65536, 65536}, {INT_MAX, 2}, {INT_MAX, INT_MAX},
    };
    uint32_t mem[12];
    sr_canvas a;
    sr_canvas b;
    size_t i;

    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        bool ra = sr_canvas_init(&a, sizes[i][0], sizes[i][1]);
        bool rb = zell_canvas_init(&b, sizes[i][0], sizes[i][1]);

        expect(ra == rb && same_struct(&a, &b), "init agrees");
        if (ra && rb) {
            expect(same_pixels(&a, &b), "a new buffer is the same zeros");
        }
        sr_canvas_free(&a);
        zell_canvas_free(&b);
        expect(same_struct(&a, &b) && b.px == NULL && b.w == 0 && !b.owns_px, "free leaves it empty");
    }
    expect(!zell_canvas_init(NULL, 4, 4), "init of no canvas is refused");
    expect(zell_canvas_init(&b, 7, 5) && b.px != NULL && b.w == 7 && b.h == 5 && b.owns_px &&
           b.clip_x0 == 0 && b.clip_y0 == 0 && b.clip_x1 == 7 && b.clip_y1 == 5 && b.px[34] == 0u,
           "init gives a zeroed 7 x 5 with the whole clip");
    zell_canvas_free(&b);
    expect(!zell_canvas_init(&b, 0, 5) && b.px == NULL && b.w == 0, "a zero width gives nothing");

    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        memset(&a, 0x5a, sizeof a);
        memset(&b, 0x5a, sizeof b);
        sr_canvas_wrap(&a, mem, sizes[i][0], sizes[i][1]);
        zell_canvas_wrap(&b, mem, sizes[i][0], sizes[i][1]);
        expect(same_struct(&a, &b) && a.px == b.px, "wrap agrees");
    }
    sr_canvas_wrap(&a, NULL, 4, 3);
    zell_canvas_wrap(&b, NULL, 4, 3);
    expect(same_struct(&a, &b) && b.px == NULL && b.w == 0, "wrap of no memory gives nothing");
    zell_canvas_wrap(&b, mem, 4, 3);
    expect(b.px == mem && b.w == 4 && b.h == 3 && !b.owns_px && b.clip_x1 == 4 && b.clip_y1 == 3,
           "wrap takes the memory and does not own it");
    mem[0] = 0x12345678u;
    zell_canvas_free(&b);
    expect(b.px == NULL && mem[0] == 0x12345678u, "free leaves wrapped memory alone");
    zell_canvas_free(NULL);
    zell_canvas_wrap(NULL, mem, 4, 3);
}

static int clip_number(void)
{
    static const int edge[] = {0, 1, -1, 5, 31, 32, 33, 47, 48, 49, 1000, -1000,
                               INT_MAX, INT_MIN, INT_MAX - 1, INT_MIN + 1};
    uint32_t v = next();

    return (v & 3u) == 0u ? edge[(v >> 8) % (sizeof edge / sizeof edge[0])]
                          : (int)((v >> 8) % 80u) - 16;
}

static void test_clip(void)
{
    sr_canvas a;
    sr_canvas b;
    int i;

    expect(sr_canvas_init(&a, 48, 32) && zell_canvas_init(&b, 48, 32), "clip canvases");
    for (i = 0; i < 20000; i++) {
        int x = clip_number();
        int y = clip_number();
        int w = clip_number();
        int h = clip_number();

        sr_canvas_set_clip(&a, x, y, w, h);
        zell_canvas_set_clip(&b, x, y, w, h);
        expect(same_struct(&a, &b), "set_clip agrees");
        expect(b.clip_x0 >= 0 && b.clip_x0 <= b.clip_x1 && b.clip_x1 <= 48 &&
               b.clip_y0 >= 0 && b.clip_y0 <= b.clip_y1 && b.clip_y1 <= 32, "the clip stays inside");
    }
    zell_canvas_set_clip(&b, 5, 6, 10, 4);
    expect(b.clip_x0 == 5 && b.clip_y0 == 6 && b.clip_x1 == 15 && b.clip_y1 == 10, "clip 5,6 10x4");
    zell_canvas_set_clip(&b, -4, 30, 10, 10);
    expect(b.clip_x0 == 0 && b.clip_y0 == 30 && b.clip_x1 == 6 && b.clip_y1 == 32, "clip cut at the sides");
    zell_canvas_set_clip(&b, 10, 10, 0, 5);
    expect(b.clip_x0 == 10 && b.clip_x1 == 10 && b.clip_y0 == 10 && b.clip_y1 == 15, "an empty clip is empty");
    zell_canvas_reset_clip(&b);
    sr_canvas_reset_clip(&a);
    expect(same_struct(&a, &b) && b.clip_x0 == 0 && b.clip_y0 == 0 && b.clip_x1 == 48 && b.clip_y1 == 32,
           "reset gives the whole canvas");
    zell_canvas_set_clip(NULL, 0, 0, 1, 1);
    zell_canvas_reset_clip(NULL);
    sr_canvas_free(&a);
    zell_canvas_free(&b);
}

static void test_clear(void)
{
    static const int sizes[][2] = {{1, 1}, {7, 1}, {8, 1}, {9, 1}, {5, 3}, {64, 48}};
    sr_canvas a;
    sr_canvas b;
    size_t i;

    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint32_t rgb = next();

        expect(sr_canvas_init(&a, sizes[i][0], sizes[i][1]) &&
               zell_canvas_init(&b, sizes[i][0], sizes[i][1]), "clear canvases");
        scribble(&a, &b);
        /* The clear is of the whole frame, whatever the clip. */
        sr_canvas_set_clip(&a, 1, 0, 2, 1);
        zell_canvas_set_clip(&b, 1, 0, 2, 1);
        sr_clear(&a, rgb);
        zell_canvas_clear(&b, rgb);
        expect(same_pixels(&a, &b), "clear agrees");
        expect(b.px[0] == (0xff000000u | (rgb & 0xffffffu)) &&
               b.px[(size_t)b.w * (size_t)b.h - 1u] == (0xff000000u | (rgb & 0xffffffu)),
               "clear stores the opaque colour from the first pixel to the last");
        sr_canvas_free(&a);
        zell_canvas_free(&b);
    }
    zell_canvas_clear(NULL, 0u);
    memset(&b, 0, sizeof b);
    zell_canvas_clear(&b, 0u);
}

/* A coordinate on or near a canvas side of that many pixels: whole, on a
 * half, or anywhere to a thousandth. */
static float coordinate(int span)
{
    uint32_t v = next();
    int reach = span + span / 4 + 4;
    int at = (int)((v >> 8) % (uint32_t)reach) - span / 8 - 2;

    switch (v & 7u) {
    case 0:
    case 1:
    case 2:
        return (float)at;
    case 3:
        return (float)at + 0.5f;
    default:
        return (float)at + (float)((v >> 20) % 1000u) / 1000.0f;
    }
}

/* Values a fill must refuse or cut down without overflow. */
static float awkward(void)
{
    static const float odd[] = {0.0f, -0.0f, 1e9f, -1e9f, 3.0e38f, -3.0e38f, 16777216.0f,
                                16777217.0f, 33554432.0f, 1e-30f, -7.25f, 2147483648.0f};
    uint32_t v = next();

    switch (v & 7u) {
    case 0:
        return NAN;
    case 1:
        return (v & 256u) ? INFINITY : -INFINITY;
    default:
        return odd[(v >> 8) % (sizeof odd / sizeof odd[0])];
    }
}

static float opacity(void)
{
    static const float odd[] = {1.0f, 1.0f, 1.0f, 1.5f, 0.001f, 0.0019f, 0.002f, 0.5f, 0.999f,
                                0.9999f, 0.88f, 0.95f, 255.0f / 256.0f, 0.0f, -1.0f};
    uint32_t v = next();

    switch (v & 15u) {
    case 0:
        return (v & 256u) ? NAN : INFINITY;
    case 1:
    case 2:
    case 3:
    case 4:
        return odd[(v >> 8) % (sizeof odd / sizeof odd[0])];
    default:
        return (float)((v >> 8) % 1001u) / 1000.0f;
    }
}

static void test_fill(void)
{
    static const int sizes[][2] = {{64, 48}, {33, 17}, {64, 48}, {7, 5}, {3, 80}, {1, 1}};
    enum { EACH = 10000 };
    sr_canvas a;
    sr_canvas b;
    size_t size;
    int i;
    long touched = 0;
    long refused = 0;

    for (size = 0; size < sizeof sizes / sizeof sizes[0]; size++) {
        expect(sr_canvas_init(&a, sizes[size][0], sizes[size][1]) &&
               zell_canvas_init(&b, sizes[size][0], sizes[size][1]), "fill canvases");
        scribble(&a, &b);
        for (i = 0; i < EACH; i++) {
            float v[4];
            float alpha = opacity();
            uint32_t rgb = next();
            uint32_t pick = next();
            uint32_t before;

            v[0] = coordinate(b.w);
            v[1] = coordinate(b.h);
            v[2] = fabsf(coordinate(b.w)) + 0.25f;
            v[3] = fabsf(coordinate(b.h)) + 0.25f;
            if ((pick & 7u) == 0u) {
                v[(pick >> 4) & 3u] = awkward();
            } else if ((pick & 7u) == 1u) {
                v[2 + ((pick >> 4) & 1u)] *= -1.0f;
            }
            if ((i & 63) == 0) {
                scribble(&a, &b);
                sr_canvas_reset_clip(&a);
                zell_canvas_reset_clip(&b);
            } else if ((i & 63) == 24) {
                /* A clip inside the canvas. */
                int cx = (int)(next() % (uint32_t)b.w);
                int cy = (int)(next() % (uint32_t)b.h);
                int cw = 1 + (int)(next() % (uint32_t)b.w);
                int ch = 1 + (int)(next() % (uint32_t)b.h);

                sr_canvas_set_clip(&a, cx, cy, cw, ch);
                zell_canvas_set_clip(&b, cx, cy, cw, ch);
            } else if ((i & 63) == 48) {
                /* Any clip at all, for a few fills. */
                int cx = clip_number();
                int cy = clip_number();
                int cw = clip_number();
                int ch = clip_number();

                sr_canvas_set_clip(&a, cx, cy, cw, ch);
                zell_canvas_set_clip(&b, cx, cy, cw, ch);
            } else if ((i & 63) == 56) {
                sr_canvas_reset_clip(&a);
                zell_canvas_reset_clip(&b);
            }
            before = sum(&b);
            sr_fill_rect(&a, v[0], v[1], v[2], v[3], rgb, alpha);
            zell_canvas_fill_rect(&b, v[0], v[1], v[2], v[3], rgb, alpha);
            if (before != sum(&b)) {
                touched++;
            } else {
                refused++;
            }
            if (!same_pixels(&a, &b)) {
                fprintf(stderr, "fill %a %a %a %a alpha %a on %d x %d\n", (double)v[0], (double)v[1],
                        (double)v[2], (double)v[3], (double)alpha, b.w, b.h);
                expect(0, "fill_rect agrees");
                scribble(&a, &b);
            }
        }
        sr_canvas_free(&a);
        zell_canvas_free(&b);
    }
    /* Many of the fills must reach the pixels, or agreeing means little;
     * and some must be turned away, or the refusals are not compared. */
    if (touched < 15000 || refused < 3000) {
        fprintf(stderr, "%ld fills changed pixels and %ld did not\n", touched, refused);
    }
    expect(touched >= 15000, "at least 15000 of the 60000 fills changed pixels");
    expect(refused >= 3000, "some fills were refused or fell outside");
    zell_canvas_fill_rect(NULL, 0, 0, 1, 1, 0u, 1.0f);
}

/* Results by number, worked out by hand from the mix:
 * channel += ((colour - channel) * steps) >> 8, steps = alpha * 256 rounded. */
static void test_fill_numbers(void)
{
    sr_canvas c;

    expect(zell_canvas_init(&c, 6, 3), "numbers canvas");
    zell_canvas_clear(&c, 0x000000u);
    zell_canvas_fill_rect(&c, 1.0f, 1.0f, 2.0f, 1.0f, 0x336699u, 1.0f);
    expect(c.px[6] == 0xff000000u && c.px[7] == 0xff336699u && c.px[8] == 0xff336699u &&
           c.px[9] == 0xff000000u && c.px[1] == 0xff000000u && c.px[13] == 0xff000000u,
           "an opaque fill on whole pixels stores the colour and nothing beside it");
    zell_canvas_clear(&c, 0x000000u);
    zell_canvas_fill_rect(&c, 0.5f, 0.0f, 1.0f, 1.0f, 0xffffffu, 1.0f);
    expect(c.px[0] == 0xff7f7f7fu && c.px[1] == 0xff7f7f7fu && c.px[2] == 0xff000000u,
           "a fill across two half pixels gives each half the colour");
    zell_canvas_clear(&c, 0x000000u);
    zell_canvas_fill_rect(&c, 0.0f, 0.0f, 1.0f, 1.0f, 0xff8040u, 0.25f);
    expect(c.px[0] == 0xff3f2010u, "a quarter alpha moves each channel a quarter of the way");
    zell_canvas_clear(&c, 0xffffffu);
    zell_canvas_fill_rect(&c, 0.0f, 0.0f, 1.0f, 1.0f, 0x000000u, 0.5f);
    expect(c.px[0] == 0xff7f7f7fu, "half black on white rounds down");
    c.px[0] = 0x00000000u;
    zell_canvas_fill_rect(&c, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffu, 0.5f);
    expect(c.px[0] == 0x7f7f7f7fu, "the alpha channel is mixed toward opaque");
    c.px[0] = 0x00102030u;
    zell_canvas_fill_rect(&c, 0.0f, 0.0f, 1.0f, 1.0f, 0xabcdefu, 1.0f);
    expect(c.px[0] == 0xffabcdefu, "an opaque fill makes the pixel opaque");
    zell_canvas_clear(&c, 0x000000u);
    zell_canvas_fill_rect(&c, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffu, 0.0019f);
    expect(c.px[0] == 0xff000000u, "an alpha under half a step draws nothing");
    zell_canvas_fill_rect(&c, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffu, 0.002f);
    expect(c.px[0] == 0xff000000u, "one step moves a channel by 255 / 256, which is still zero");
    zell_canvas_fill_rect(&c, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffu, 0.008f);
    expect(c.px[0] == 0xff010101u, "two steps move 255 by one");
    zell_canvas_clear(&c, 0x000000u);
    zell_canvas_set_clip(&c, 2, 1, 2, 1);
    zell_canvas_fill_rect(&c, -10.0f, -10.0f, 100.0f, 100.0f, 0xffffffu, 1.0f);
    expect(c.px[7] == 0xff000000u && c.px[8] == 0xffffffffu && c.px[9] == 0xffffffffu &&
           c.px[10] == 0xff000000u && c.px[2] == 0xff000000u && c.px[14] == 0xff000000u,
           "a fill stays inside the clip");
    zell_canvas_free(&c);
}

static void test_pack(void)
{
    uint8_t one[4 * 35 + 4];
    uint8_t two[4 * 35 + 4];
    sr_canvas a;
    sr_canvas b;

    expect(sr_canvas_init(&a, 7, 5) && zell_canvas_init(&b, 7, 5), "pack canvases");
    scribble(&a, &b);
    memset(one, 0xee, sizeof one);
    memset(two, 0xee, sizeof two);
    expect(sr_pack_rgba(&a, one, 140) && zell_canvas_pack_rgba(&b, two, 140) &&
           memcmp(one, two, sizeof one) == 0 && two[140] == 0xee, "pack agrees and stops at the last pixel");
    b.px[0] = 0x11223344u;
    b.px[34] = 0xa1b2c3d4u;
    expect(zell_canvas_pack_rgba(&b, two, sizeof two) && two[0] == 0x22 && two[1] == 0x33 &&
           two[2] == 0x44 && two[3] == 0x11 && two[136] == 0xb2 && two[137] == 0xc3 &&
           two[138] == 0xd4 && two[139] == 0xa1, "bytes are red, green, blue, alpha");
    memset(two, 0xee, sizeof two);
    expect(!sr_pack_rgba(&a, one, 139) && !zell_canvas_pack_rgba(&b, two, 139) && two[0] == 0xee,
           "a short buffer is refused and left alone");
    expect(!zell_canvas_pack_rgba(&b, NULL, 140) && !zell_canvas_pack_rgba(NULL, two, 140),
           "pack needs a canvas and a buffer");
    sr_canvas_free(&a);
    zell_canvas_free(&b);
}

static long slurp(const char *path, unsigned char *into, long room)
{
    FILE *file = fopen(path, "rb");
    long got;

    if (file == NULL) {
        return -1;
    }
    got = (long)fread(into, 1, (size_t)room, file);
    fclose(file);
    return got;
}

static void test_ppm(void)
{
    /* 100 x 50 is 5000 pixels, so the writer's second block is used. */
    enum { W = 100, H = 50, ROOM = 15 + W * H * 3 + 8 };
    static unsigned char one[ROOM];
    static unsigned char two[ROOM];
    const char *tmp = getenv("TMPDIR");
    char dir[200];
    char first[220];
    char second[220];
    char missing[220];
    sr_canvas a;
    sr_canvas b;
    long n1;
    long n2;

    snprintf(dir, sizeof dir, "%.160s/zell-canvas-XXXXXX", tmp != NULL && tmp[0] != '\0' ? tmp : "/tmp");
    expect(mkdtemp(dir) != NULL, "a scratch directory");
    snprintf(first, sizeof first, "%s/a.ppm", dir);
    snprintf(second, sizeof second, "%s/b.ppm", dir);
    snprintf(missing, sizeof missing, "%s/none/c.ppm", dir);
    expect(sr_canvas_init(&a, W, H) && zell_canvas_init(&b, W, H), "ppm canvases");
    scribble(&a, &b);
    b.px[0] = a.px[0] = 0x80112233u;
    b.px[W * H - 1] = a.px[W * H - 1] = 0x00a0b0c0u;
    expect(sr_write_ppm(&a, first) && zell_canvas_write_ppm(&b, second), "both files are written");
    expect(errno == 0, "errno is clear after a write");
    n1 = slurp(first, one, ROOM);
    n2 = slurp(second, two, ROOM);
    expect(n1 == 14 + W * H * 3 && n2 == n1 && memcmp(one, two, (size_t)n1) == 0, "the two files are the same bytes");
    expect(n2 > 14 && memcmp(two, "P6\n100 50\n255\n", 14) == 0 && two[14] == 0x11 && two[15] == 0x22 &&
           two[16] == 0x33 && two[n2 - 3] == 0xa0 && two[n2 - 2] == 0xb0 && two[n2 - 1] == 0xc0,
           "P6 header, then red, green, blue from the first pixel to the last");
    errno = 0;
    expect(!zell_canvas_write_ppm(&b, missing) && errno == ENOENT, "a missing directory fails with its reason");
    {
        /* A device with no room. The big frame fails while its blocks are
         * written; the 1 x 1 frame fits the stream's buffer, so only the
         * close can report it. */
        FILE *full = fopen("/dev/full", "wb");
        sr_canvas dot_a;
        sr_canvas dot_b;

        if (full != NULL) {
            int want;

            fclose(full);
            errno = 0;
            expect(!sr_write_ppm(&a, "/dev/full"), "soft-raster fails on a full device");
            want = errno;
            errno = 0;
            expect(!zell_canvas_write_ppm(&b, "/dev/full") && errno == want && want == ENOSPC,
                   "a full device fails the write with no space");
            expect(sr_canvas_init(&dot_a, 1, 1) && zell_canvas_init(&dot_b, 1, 1), "dot canvases");
            errno = 0;
            expect(!sr_write_ppm(&dot_a, "/dev/full"), "soft-raster fails a small file on a full device");
            want = errno;
            errno = 0;
            expect(!zell_canvas_write_ppm(&dot_b, "/dev/full") && errno == want && want == ENOSPC,
                   "a full device fails the close of a small file");
            sr_canvas_free(&dot_a);
            zell_canvas_free(&dot_b);
        }
    }
    errno = 0;
    expect(!zell_canvas_write_ppm(&b, NULL) && errno == EINVAL, "no path is refused");
    errno = 0;
    expect(!zell_canvas_write_ppm(NULL, second) && errno == EINVAL, "no canvas is refused");
    unlink(first);
    unlink(second);
    rmdir(dir);
    sr_canvas_free(&a);
    zell_canvas_free(&b);
}

/* Past 2^24 a float no longer holds every pixel coordinate, and the fill
 * stops its whole-pixel run there. Zell makes no canvas that long; the two
 * must still agree on one, along a row and down a column. */
static void test_fill_past_float_range(void)
{
    enum { PAST = 8, LONG = (1 << 24) + PAST, FROM = (1 << 24) - 4 };
    uint32_t *mem_a = calloc((size_t)LONG, sizeof(uint32_t));
    uint32_t *mem_b = calloc((size_t)LONG, sizeof(uint32_t));
    int turn;

    if (mem_a == NULL || mem_b == NULL) {
        /* No room for two 64 MB buffers is not a fault of the canvas. */
        free(mem_a);
        free(mem_b);
        return;
    }
    for (turn = 0; turn < 2; turn++) {
        sr_canvas a;
        sr_canvas b;
        int painted = 0;
        int i;

        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        memset(mem_a, 0, (size_t)LONG * sizeof(uint32_t));
        memset(mem_b, 0, (size_t)LONG * sizeof(uint32_t));
        sr_canvas_wrap(&a, mem_a, turn == 0 ? LONG : 1, turn == 0 ? 1 : LONG);
        zell_canvas_wrap(&b, mem_b, turn == 0 ? LONG : 1, turn == 0 ? 1 : LONG);
        expect(a.px == mem_a && b.px == mem_b, "a canvas past 2^24 is wrapped");
        /* Opaque, then a half-covering colour over it from a fractional edge. */
        if (turn == 0) {
            sr_fill_rect(&a, (float)FROM, 0.0f, 12.0f, 1.0f, 0xffffffu, 1.0f);
            zell_canvas_fill_rect(&b, (float)FROM, 0.0f, 12.0f, 1.0f, 0xffffffu, 1.0f);
            sr_fill_rect(&a, (float)FROM - 2.5f, 0.0f, 14.5f, 1.0f, 0x336699u, 0.5f);
            zell_canvas_fill_rect(&b, (float)FROM - 2.5f, 0.0f, 14.5f, 1.0f, 0x336699u, 0.5f);
        } else {
            sr_fill_rect(&a, 0.0f, (float)FROM, 1.0f, 12.0f, 0xffffffu, 1.0f);
            zell_canvas_fill_rect(&b, 0.0f, (float)FROM, 1.0f, 12.0f, 0xffffffu, 1.0f);
            sr_fill_rect(&a, 0.0f, (float)FROM - 2.5f, 1.0f, 14.5f, 0x336699u, 0.5f);
            zell_canvas_fill_rect(&b, 0.0f, (float)FROM - 2.5f, 1.0f, 14.5f, 0x336699u, 0.5f);
        }
        expect(memcmp(mem_a, mem_b, (size_t)LONG * sizeof(uint32_t)) == 0,
               turn == 0 ? "a fill along a row past 2^24 is the same" : "a fill down a column past 2^24 is the same");
        for (i = FROM; i < LONG; i++) {
            painted += mem_b[i] != 0u;
        }
        expect(painted >= 4, "the fill reached 2^24");
    }
    free(mem_a);
    free(mem_b);
}

int main(void)
{
    test_buffer();
    test_clip();
    test_clear();
    test_fill();
    test_fill_numbers();
    test_fill_past_float_range();
    test_pack();
    test_ppm();
    if (failures) {
        fprintf(stderr, "test-canvas: %d failed\n", failures);
        return 1;
    }
    printf("test-canvas: ok\n");
    return 0;
}
