#include "draw.h"
#include "room.h"
#include "sprites.h"

#include <stdio.h>
#include <string.h>

static void sprite_at(sr_canvas *canvas, const uint32_t *pixels,
                      int width, int height, int foot,
                      float feet_x, float feet_y, int scale)
{
    uint32_t storage[64 * 64];
    sr_canvas sprite;
    int draw_w;
    int draw_h;
    int dest_foot;

    if (width < 1 || height < 1 || width > 64 || height > 64 || scale < 1) {
        return;
    }
    memcpy(storage, pixels, (size_t)width * (size_t)height * sizeof storage[0]);
    sr_canvas_wrap(&sprite, storage, width, height);
    draw_w = width * scale;
    draw_h = height * scale;
    dest_foot = foot * scale;
    if (dest_foot > draw_h) {
        dest_foot = draw_h;
    }
    sr_fill_ellipse(canvas, feet_x, feet_y - 2.0f,
                    (float)draw_w * 0.28f, (float)draw_w * 0.08f,
                    0x000000u, 0.35f);
    sr_blit_scaled(canvas, &sprite,
                   (int)feet_x - draw_w / 2,
                   (int)feet_y - dest_foot,
                   draw_w, draw_h, 1.0f);
}

static int pull_line(const char **cursor, int max_width, char *line, int cap)
{
    const char *start;
    int length = 0;

    if (cursor == NULL || *cursor == NULL || **cursor == '\0' || cap < 2) {
        return -1;
    }
    start = *cursor;
    if (max_width < 8) {
        max_width = 8;
    }
    while (**cursor != '\0') {
        unsigned char ch = (unsigned char)**cursor;
        int split;

        if (ch == '\n' || length >= cap - 1) {
            line[length] = '\0';
            if (ch == '\n') {
                *cursor += 1;
            }
            return length;
        }
        line[length] = (char)ch;
        line[length + 1] = '\0';
        if (length > 0 &&
            sr_text_width_in(SR_FONT_FIXED_8X16, line, 1) > max_width) {
            split = length;
            while (split > 0 && line[split - 1] != ' ') {
                split -= 1;
            }
            if (split > 0) {
                *cursor -= length - split;
                while (split > 0 && line[split - 1] == ' ') {
                    split -= 1;
                }
                if (*cursor <= start) {
                    *cursor = start + 1;
                    line[0] = *start;
                    line[1] = '\0';
                    return 1;
                }
                line[split] = '\0';
                return split;
            }
            line[length] = '\0';
            return length;
        }
        length += 1;
        *cursor += 1;
    }
    if (length == 0) {
        return -1;
    }
    line[length] = '\0';
    return length;
}

static void answer_layout(int width, int height, int *text_y, int *rows,
                          int *text_width)
{
    int line = sr_font_height(SR_FONT_FIXED_8X16) + 2;
    int y = 50;
    int footer = height - 28;
    int fit = line > 0 ? (footer - 4 - y) / line : 1;
    int span = width - 40;

    if (fit < 1) {
        fit = 1;
    }
    if (span < 8) {
        span = 8;
    }
    if (text_y != NULL) {
        *text_y = y;
    }
    if (rows != NULL) {
        *rows = fit;
    }
    if (text_width != NULL) {
        *text_width = span;
    }
}

static int wrap_count(const char *text, int max_width)
{
    char line[180];
    const char *cursor = text == NULL ? "" : text;
    int rows = 0;

    while (pull_line(&cursor, max_width, line, (int)sizeof line) >= 0) {
        rows += 1;
    }
    return rows;
}

int zell_answer_scroll_limit(int width, int height, const char *text)
{
    int rows = 1;
    int span = 8;
    int total;

    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }
    answer_layout(width, height, NULL, &rows, &span);
    total = wrap_count(text, span);
    if (total <= rows) {
        return 0;
    }
    return total - rows;
}

int zell_service_scroll_limit(const zell_scene *s)
{
    int width=s->width-32;if(width>900)width=900;if(width<64)width=64;
    int reserved=s->service_phase==1?100:32;
    int rows=(s->height-44-44-28-reserved)/18;if(rows<1)rows=1;
    int total=wrap_count(s->service_text,width-24);
    return total>rows?total-rows:0;
}

static void draw_wrapped(sr_canvas *canvas, int x, int y, int max_width,
                         const char *text, uint32_t color, int skip, int max_rows)
{
    char line[180];
    const char *cursor = text == NULL ? "" : text;
    int line_height = sr_font_height(SR_FONT_FIXED_8X16) + 2;
    int index = 0;
    int drawn = 0;
    int length;

    if (skip < 0) {
        skip = 0;
    }
    if (max_rows < 1) {
        return;
    }
    while ((length = pull_line(&cursor, max_width, line, (int)sizeof line)) >= 0) {
        if (index >= skip && drawn < max_rows) {
            sr_text(canvas, (float)x, (float)(y + drawn * line_height),
                    line, color, 1.0f, 1);
            drawn += 1;
        }
        index += 1;
        (void)length;
    }
}

static void panel(sr_canvas *canvas, int y, int height)
{
    sr_fill_rect(canvas, 8.0f, (float)y, (float)canvas->w - 16.0f,
                 (float)height, 0x14161cu, 0.92f);
}

/* Keep the last row of a menu above the footer on a short pane. */
static int menu_top(int canvas_h, int span)
{
    int top = canvas_h / 2;
    int fitted = canvas_h - span;

    if (top > fitted) {
        top = fitted;
    }
    if (top < 8) {
        top = 8;
    }
    return top;
}

static const uint32_t *tile_for(char mark)
{
    switch (mark) {
    case 'd': return zell_tile_dark();
    case 'u': return zell_tile_stairs();
    case 'x': return zell_tile_wall();
    default: return zell_tile_stone();
    }
}

static void blit_at(sr_canvas *canvas, const uint32_t *pixels,
                    int width, int height, int x, int y,
                    int dest_w, int dest_h, float alpha)
{
    uint32_t storage[64 * 64];
    sr_canvas sprite;

    if (pixels == NULL || width < 1 || height < 1 || width > 64 || height > 64 ||
        dest_w < 1 || dest_h < 1) {
        return;
    }
    memcpy(storage, pixels, (size_t)width * (size_t)height * sizeof storage[0]);
    sr_canvas_wrap(&sprite, storage, width, height);
    sr_blit_scaled(canvas, &sprite, x, y, dest_w, dest_h, alpha);
}

static int voice_sign_scale(int canvas_w, float plate_top)
{
    int cell_w;
    int room;
    int scale = 1;

    if (canvas_w < 1) {
        canvas_w = 1;
    }
    cell_w = canvas_w / ZELL_ROOM_COLS;
    room = cell_w - 4;
    if (room < 16) {
        room = 16;
    }
    while (scale < 4) {
        int next = scale + 1;
        int pad = 4 * next;
        int text_w = sr_text_width_in(SR_FONT_FIXED_8X16, "voice", next);
        int text_h = sr_font_height(SR_FONT_FIXED_8X16) * next;
        int board_w = text_w + pad * 2;
        int board_h = text_h + pad * 2;

        if (board_w > room) {
            break;
        }
        if ((float)board_h + 3.0f > plate_top - 1.0f) {
            break;
        }
        scale = next;
    }
    return scale;
}

static void draw_voice_sign(sr_canvas *canvas)
{
    float sx;
    float sy;
    float plate_top;
    float x;
    float y;
    float board_w;
    float board_h;
    int scale;
    int pad;
    int text_w;
    int text_h;

    zell_switch_point(canvas->w, canvas->h, &sx, &sy);
    plate_top = sy - 11.0f;
    scale = voice_sign_scale(canvas->w, plate_top);
    pad = 4 * scale;
    text_w = sr_text_width_in(SR_FONT_FIXED_8X16, "voice", scale);
    text_h = sr_font_height(SR_FONT_FIXED_8X16) * scale;
    board_w = (float)(text_w + pad * 2);
    board_h = (float)(text_h + pad * 2);
    x = sx - board_w * 0.5f;
    y = plate_top - 3.0f - board_h;
    if (y < 1.0f) {
        y = 1.0f;
    }
    sr_fill_rect(canvas, x, y, board_w, board_h, 0x8d5a32u, 1.0f);
    sr_fill_rect(canvas, x, y, board_w, board_h * 0.30f, 0xc4956au, 1.0f);
    sr_fill_rect(canvas, x, y + board_h * 0.70f, board_w, board_h * 0.30f,
                 0x6b4124u, 1.0f);
    sr_stroke_rect(canvas, x, y, board_w, board_h, (float)scale,
                   0x3a2414u, 1.0f);
    sr_text(canvas, x + (float)pad, y + (float)pad, "voice",
            0x24160cu, 1.0f, scale);
}

static void draw_switch(const zell_scene *scene, sr_canvas *canvas)
{
    float x;
    float y;
    uint32_t lever = scene->voice_on ? 0xf2c14eu : 0x6a6458u;

    zell_switch_point(canvas->w, canvas->h, &x, &y);
    sr_fill_rect(canvas, x - 7.0f, y - 11.0f, 14.0f, 22.0f, 0x1c1814u, 1.0f);
    sr_stroke_rect(canvas, x - 7.0f, y - 11.0f, 14.0f, 22.0f, 1.0f,
                   0xc8c2b4u, 1.0f);
    if (scene->voice_on) {
        sr_fill_rect(canvas, x - 1.0f, y - 9.0f, 3.0f, 10.0f, lever, 1.0f);
    } else {
        sr_fill_rect(canvas, x - 1.0f, y + 1.0f, 3.0f, 10.0f, lever, 1.0f);
    }
    sr_fill_circle(canvas, x, y, 2.5f, lever, 1.0f);
}

static void draw_room(sr_canvas *canvas)
{
    int row;
    int col;

    for (row = 0; row < ZELL_ROOM_ROWS; row++) {
        int y0 = row * canvas->h / ZELL_ROOM_ROWS;
        int y1 = (row + 1) * canvas->h / ZELL_ROOM_ROWS;

        for (col = 0; col < ZELL_ROOM_COLS; col++) {
            int x0 = col * canvas->w / ZELL_ROOM_COLS;
            int x1 = (col + 1) * canvas->w / ZELL_ROOM_COLS;

            blit_at(canvas, tile_for(zell_room_mark(col, row)),
                    zell_tile_edge(), zell_tile_edge(),
                    x0, y0, x1 - x0, y1 - y0, 1.0f);
        }
    }
}

void zell_campfire_rect(int canvas_w, int canvas_h, int index,
                        int *x, int *y, int *width, int *height)
{
    int col = index == 0 ? 2 : 7;
    int row = 4;
    int x0;
    int x1;
    int y1;
    int size;
    int left;
    int top;

    if (canvas_w < 1) {
        canvas_w = 1;
    }
    if (canvas_h < 1) {
        canvas_h = 1;
    }
    x0 = col * canvas_w / ZELL_ROOM_COLS;
    x1 = (col + 1) * canvas_w / ZELL_ROOM_COLS;
    y1 = (row + 1) * canvas_h / ZELL_ROOM_ROWS;
    size = (x1 - x0) * 2;
    if (size < 8) {
        size = 8;
    }
    left = (x0 + x1) / 2 - size / 2;
    top = y1 - size;
    if (left < 0) {
        left = 0;
    }
    if (top < 0) {
        top = 0;
    }
    if (left + size > canvas_w) {
        size = canvas_w - left;
    }
    if (top + size > canvas_h) {
        size = canvas_h - top;
    }
    if (size < 1) {
        size = 1;
    }
    if (x != NULL) {
        *x = left;
    }
    if (y != NULL) {
        *y = top;
    }
    if (width != NULL) {
        *width = size;
    }
    if (height != NULL) {
        *height = size;
    }
}

static void draw_campfires(const zell_scene *scene, sr_canvas *canvas)
{
    int index;
    int frames = zell_campfire_frames();
    int phase = 0;

    if (frames < 1) {
        frames = 1;
    }
    if (scene != NULL && scene->flame_tick > 0) {
        phase = scene->flame_tick / 5;
    }
    for (index = 0; index < 2; index++) {
        int x;
        int y;
        int width;
        int height;
        int frame = (phase + index * 2) % frames;

        zell_campfire_rect(canvas->w, canvas->h, index, &x, &y, &width, &height);
        blit_at(canvas, zell_campfire_frame(frame),
                zell_campfire_edge(), zell_campfire_edge(),
                x, y, width, height, 1.0f);
    }
}

static void blit_crop(sr_canvas *canvas, const uint32_t *src, int src_w,
                      int src_x, int src_y, int width, int height,
                      int dest_x, int dest_y, int dest_w, int dest_h,
                      float alpha)
{
    uint32_t packed[24 * 28];
    int row;
    int col;

    if (src == NULL || width < 1 || height < 1 || width > 24 || height > 28 ||
        src_x < 0 || src_y < 0 || src_x + width > src_w || src_y + height > src_w) {
        return;
    }
    for (row = 0; row < height; row++) {
        for (col = 0; col < width; col++) {
            packed[row * width + col] = src[(src_y + row) * src_w + src_x + col];
        }
    }
    blit_at(canvas, packed, width, height, dest_x, dest_y, dest_w, dest_h, alpha);
}

static int sprite_opaque(const uint32_t *pixels, int width, int height,
                         int x, int y)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return 0;
    }
    return (pixels[(y * width) + x] >> 24) != 0;
}

/* Pointed campfire tongues. The base of each crop is the hot part of the
 * flame and the top is the tip, still above the log ring. */
static const int BURN_CROPS[][4] = {
    {24, 8, 16, 28},
    {20, 14, 14, 22},
    {30, 12, 14, 24},
};

/* Tongues grow out of the fur, then break off, rise, and thin out. */
static void draw_burn(const zell_scene *scene, sr_canvas *canvas, int kilix_scale)
{
    enum { RAW = 128, EDGES = 48, WISPS = 9, CYCLE = 16 };
    const uint32_t *body;
    int width;
    int height;
    int x;
    int y;
    int raw = 0;
    int count = 0;
    int raw_x[RAW];
    int raw_y[RAW];
    int edges_x[EDGES];
    int edges_y[EDGES];
    int frames;
    int index;
    int pass;
    int sprite_left;
    int sprite_top;
    int foot_limit;

    if (scene->burn <= 0 || kilix_scale < 1) {
        return;
    }
    body = zell_kilix_sprite(scene->direction, scene->step);
    width = zell_kilix_width();
    height = zell_kilix_height();
    if (body == NULL || width < 1 || height < 1) {
        return;
    }
    foot_limit = zell_kilix_foot() - 10;
    if (foot_limit < 1) {
        foot_limit = 1;
    }
    for (y = 0; y < foot_limit && y < height && raw < RAW; y++) {
        for (x = 0; x < width && raw < RAW; x++) {
            if (!sprite_opaque(body, width, height, x, y)) {
                continue;
            }
            if (sprite_opaque(body, width, height, x, y - 1)) {
                continue;
            }
            raw_x[raw] = x;
            raw_y[raw] = y;
            raw += 1;
        }
    }
    for (index = 0; index < raw && count < EDGES; index++) {
        int kept;
        int edge;

        kept = 1;
        for (edge = 0; edge < count; edge++) {
            int dx = raw_x[index] - edges_x[edge];
            int dy = raw_y[index] - edges_y[edge];

            if (dx < 0) {
                dx = -dx;
            }
            if (dy < 0) {
                dy = -dy;
            }
            if (dx < 4 && dy < 4) {
                kept = 0;
                break;
            }
        }
        if (!kept) {
            continue;
        }
        edges_x[count] = raw_x[index];
        edges_y[count] = raw_y[index];
        count += 1;
    }
    if (count < 1) {
        return;
    }
    frames = zell_campfire_frames();
    if (frames < 1) {
        frames = 1;
    }
    sprite_left = (int)scene->x - (width * kilix_scale) / 2;
    sprite_top = (int)scene->y - zell_kilix_foot() * kilix_scale;
    /* Departing tongues first, so a flame still rooted in the fur stays bright. */
    for (pass = 0; pass < 2; pass++) {
        for (index = 0; index < WISPS; index++) {
            int pick = (index * count) / WISPS;
            int crop = index % 3;
            int phase = (scene->flame_tick + index * 4) % CYCLE;
            int full_h = 28 * kilix_scale;
            int rise = 0;
            int lift = 0;
            int dest_w;
            int dest_h;
            int side;
            int plant = 0;
            int drift = 0;
            int ax;
            int ay;
            float alpha = 1.0f;
            const uint32_t *flame;

            if ((pass == 0) != (phase >= 8)) {
                continue;
            }
            if (phase < 8) {
                dest_h = (10 + (phase * 18) / 7) * kilix_scale;
                dest_w = (6 + phase) * kilix_scale;
            } else {
                /* The tip stays put and the base lifts off the fur. */
                rise = phase - 7;
                lift = rise * 3 * kilix_scale;
                dest_h = full_h - lift;
                dest_w = (13 - rise) * kilix_scale;
                alpha = 1.0f - (float)rise * 0.05f;
            }
            if (dest_w < 3) {
                dest_w = 3;
            }
            if (dest_h < 4) {
                dest_h = 4;
            }
            if (alpha < 0.4f) {
                alpha = 0.4f;
            }
            side = edges_x[pick] - width / 2;
            if (side > 6) {
                plant = -2 * kilix_scale;
                drift = lift / 4;
            } else if (side < -6) {
                plant = 2 * kilix_scale;
                drift = -(lift / 4);
            }
            flame = zell_campfire_frame((scene->flame_tick / 5 + index) % frames);
            ax = sprite_left + edges_x[pick] * kilix_scale + plant + drift;
            ay = sprite_top + edges_y[pick] * kilix_scale + 3 * kilix_scale;
            blit_crop(canvas, flame, zell_campfire_edge(),
                      BURN_CROPS[crop][0], BURN_CROPS[crop][1],
                      BURN_CROPS[crop][2], BURN_CROPS[crop][3],
                      ax - dest_w / 2, ay - dest_h - lift,
                      dest_w, dest_h, alpha);
        }
    }
}

static void draw_framerate(const zell_scene *scene, sr_canvas *canvas)
{
#ifdef ZELL_RENDER_NEURONTE
    char label[16];
    int width;
    int wrote;

    if (scene->frame_rate < 1) {
        return;
    }
    wrote = snprintf(label, sizeof label, "%d fps", scene->frame_rate);
    if (wrote < 1 || (size_t)wrote >= sizeof label) {
        return;
    }
    width = sr_text_width_in(SR_FONT_FIXED_8X16, label, 1);
    if (width > canvas->w - 8) {
        return;
    }
    sr_text_outlined(canvas, (float)(canvas->w - width - 6), 6.0f,
                     label, 0xf4f1e8u, 1.0f, 1);
#else
    (void)scene;
    (void)canvas;
#endif
}

static void draw_hearts(const zell_scene *scene, sr_canvas *canvas)
{
    int count = scene->max_hp;
    int size = 12;
    int step;
    int index;
    int edge = zell_heart_edge();

    if (count < 1) {
        return;
    }
    if (count > 8) {
        count = 8;
    }
    step = size + 2;
    if (count * step > canvas->w - 8) {
        size = 8;
        step = size + 1;
    }
    for (index = 0; index < count; index++) {
        float alpha = index < scene->hp ? 1.0f : 0.38f;

        blit_at(canvas, zell_heart(), edge, edge,
                6 + index * step, 4, size, size, alpha);
    }
}

static void draw_history_switch(const zell_scene *scene, sr_canvas *canvas, int rows)
{
    float x;
    float y;
    uint32_t lever = scene->history_on ? 0xf2c14eu : 0x6a6458u;
    uint32_t edge = scene->menu_index == rows ? 0xf2c14eu : 0xc8c2b4u;

    zell_history_switch_point(canvas->w, canvas->h, rows, &x, &y);
    sr_fill_rect(canvas, x - 7.0f, y - 11.0f, 14.0f, 22.0f, 0x1c1814u, 1.0f);
    sr_stroke_rect(canvas, x - 7.0f, y - 11.0f, 14.0f, 22.0f, 1.0f,
                   edge, 1.0f);
    if (scene->history_on) {
        sr_fill_rect(canvas, x - 1.0f, y - 9.0f, 3.0f, 10.0f, lever, 1.0f);
    } else {
        sr_fill_rect(canvas, x - 1.0f, y + 1.0f, 3.0f, 10.0f, lever, 1.0f);
    }
    sr_fill_circle(canvas, x, y, 2.5f, lever, 1.0f);
    sr_text(canvas, x + 12.0f, y - 8.0f, "history", 0xf4f1e8u, 1.0f, 1);
}

static void choice(sr_canvas *canvas, int x, int y, const char *label, bool selected)
{
    char row[200];
    snprintf(row, sizeof row, "%s %s", selected ? ">" : " ", label);
    sr_text(canvas, (float)x, (float)y, row,
            selected ? 0xf2c14eu : 0xf4f1e8u, 1.0f, 1);
}

static void input_field(sr_canvas *canvas, int x, int y, int width,
                        const char *text, int text_len)
{
    int inner;
    int max_chars;
    int shown_len;
    int text_w;
    const char *shown;

    if (width < 16) {
        width = 16;
    }
    sr_fill_rect(canvas, (float)x, (float)y, (float)width, 22.0f,
                 0x0c0e12u, 1.0f);
    sr_stroke_rect(canvas, (float)x, (float)y, (float)width, 22.0f,
                   1.0f, 0xc8c2b4u, 1.0f);
    inner = width - 12;
    if (inner < 8) {
        inner = 8;
    }
    max_chars = inner / 8;
    if (max_chars < 1) {
        max_chars = 1;
    }
    shown = text != NULL ? text : "";
    shown_len = text_len;
    if (shown_len < 0) {
        shown_len = 0;
    }
    if (shown_len > max_chars) {
        shown += shown_len - max_chars;
        shown_len = max_chars;
    }
    text_w = 0;
    if (shown_len > 0) {
        sr_text(canvas, (float)(x + 6), (float)(y + 3), shown,
                0xf4f1e8u, 1.0f, 1);
        text_w = sr_text_width_in(SR_FONT_FIXED_8X16, shown, 1);
    }
    sr_fill_rect(canvas, (float)(x + 6 + text_w), (float)(y + 3),
                 1.0f, 16.0f, 0xf2c14eu, 1.0f);
}


#include "desktop_draw.inc"

void zell_draw(const zell_scene *scene, sr_canvas *canvas)
{
    int index;
    int top;
    int title_w;
    int kilix_scale;
    float title_x;
    const char *hint;
    int hint_w;

    if (scene->desktop) {
        desktop_base(scene, canvas);
        if (scene->mode == ZELL_MODE_SERVICE) {desktop_service(scene, canvas);return;}
        if (scene->zoom_ticks > 0) desktop_zoom(scene, canvas);
        else if (scene->mode == ZELL_MODE_DESK || scene->mode == ZELL_MODE_LAUNCHER) desktop_panel(scene, canvas);
        else if (scene->mode != ZELL_MODE_WALK) desktop_dialogue(scene, canvas);
        desktop_terminal_tab(scene, canvas);
        return;
    } else {
    sr_clear(canvas, 0x100e14u);
    draw_room(canvas);
    draw_voice_sign(canvas);
    draw_switch(scene, canvas);
    sprite_at(canvas, zell_wizard_sprite(),
              zell_wizard_width(), zell_wizard_height(), zell_wizard_height(),
              scene->wizard_x, scene->wizard_y, scene->scale);
    draw_campfires(scene, canvas);
    kilix_scale = scene->scale >= 4 ? 2 : 1;
    sprite_at(canvas, zell_kilix_sprite(scene->direction, scene->step),
              zell_kilix_width(), zell_kilix_height(), zell_kilix_foot(),
              scene->x, scene->y, kilix_scale);
    draw_burn(scene, canvas, kilix_scale);
    draw_hearts(scene, canvas);
    title_w = sr_text_width_in(SR_FONT_FIXED_8X16, "Zell", 1);
    title_x = (float)(canvas->w - title_w) / 2.0f;
    sr_text_outlined(canvas, title_x, 6.0f, "Zell", 0xf4f1e8u, 1.0f, 1);
    }
    if (scene->mode == ZELL_MODE_WALK) {
        if (zell_near_switch(scene)) {
            hint = scene->voice_on ? "Enter  turn voice off"
                                   : "Enter  turn voice on";
        } else if (zell_near_wizard(scene)) {
            hint = "Enter  talk to the Old Wizard";
        } else {
            hint = "Walk off the bottom to leave";
        }
        hint_w = sr_text_width_in(SR_FONT_FIXED_8X16, hint, 1);
        sr_text_outlined(canvas, (float)(canvas->w - hint_w) / 2.0f,
                         (float)(canvas->h - 20),
                         hint, 0xd8d4c8u, 1.0f, 1);
        draw_framerate(scene, canvas);
        return;
    }

    if (scene->mode == ZELL_MODE_ANSWER) {
        int text_y = 50;
        int rows = 1;
        int span = 8;
        int skip = scene->answer_scroll;
        int limit = zell_answer_scroll_limit(canvas->w, canvas->h, scene->answer);

        hint = limit > 0 ? "Up Down scroll    Enter closes" : "Enter closes";

        answer_layout(canvas->w, canvas->h, &text_y, &rows, &span);
        if (skip > limit) {
            skip = limit;
        }
        if (skip < 0) {
            skip = 0;
        }
        panel(canvas, 20, canvas->h - 28);
        sr_text(canvas, 20.0f, 28.0f,
                scene->answerer[0] != '\0' ? scene->answerer : "Kilix Ask",
                0xf2c14eu, 1.0f, 1);
        draw_wrapped(canvas, 20, text_y, span, scene->answer, 0xf4f1e8u,
                     skip, rows);
        hint_w = sr_text_width_in(SR_FONT_FIXED_8X16, hint, 1);
        sr_text(canvas, (float)(canvas->w - hint_w) / 2.0f,
                (float)(canvas->h - 28), hint, 0xa8b0bdu, 1.0f, 1);
        draw_framerate(scene, canvas);
        return;
    }
    if (scene->mode == ZELL_MODE_QUESTION) {
        top = menu_top(canvas->h, 220);
    } else {
        top = menu_top(canvas->h, 80);
    }
    panel(canvas, top - 8, canvas->h - top - 8);
    if (scene->mode == ZELL_MODE_QUESTION || scene->mode == ZELL_MODE_TYPE) {
        sr_text(canvas, 20.0f, (float)top, "Old Wizard", 0xf2c14eu, 1.0f, 1);
        sr_text(canvas, 20.0f, (float)(top + 18), "Ask", 0xf4f1e8u, 1.0f, 1);
        input_field(canvas, 20, top + 40, canvas->w - 40,
                    scene->question, scene->question_len);
        if (scene->mode == ZELL_MODE_QUESTION) {
            int rows = zell_scene_choice_rows(scene);

            for (index = 0; index < rows; index++) {
                choice(canvas, 20, top + 70 + index * 18,
                       zell_scene_choice(scene, index),
                       scene->menu_index == index);
            }
            draw_history_switch(scene, canvas, rows);
            if (scene->menu_index == rows) {
                hint = scene->history_on ? "Enter  turn history off"
                                         : "Enter  turn history on";
            } else {
                hint = "Type in the field    Enter asks    Esc back";
            }
            hint_w = sr_text_width_in(SR_FONT_FIXED_8X16, hint, 1);
            sr_text(canvas, (float)(canvas->w - hint_w) / 2.0f,
                    (float)(canvas->h - 28), hint, 0xa8b0bdu, 1.0f, 1);
        } else {
            sr_text(canvas, 20.0f, (float)(canvas->h - 28),
                    "Enter asks    Esc back", 0xa8b0bdu, 1.0f, 1);
        }
        draw_framerate(scene, canvas);
        return;
    }
    if (scene->mode == ZELL_MODE_WAIT) {
        char line[80];
        const char *who = scene->answerer[0] != '\0' ? scene->answerer : "Kilix Ask";
        snprintf(line, sizeof line, "Asking %s...", who);
        sr_text(canvas, 20.0f, (float)top, line, 0xf4f1e8u, 1.0f, 1);
        sr_text(canvas, 20.0f, (float)(top + 22), "Esc cancels",
                0xa8b0bdu, 1.0f, 1);
        draw_framerate(scene, canvas);
        return;
    }
}
