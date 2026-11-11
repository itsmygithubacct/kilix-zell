#include "scene.h"
#include "room.h"
#include "draw.h"
#include "sprites.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int prompt_for(char *destination, size_t capacity, const char *question)
{
    int wrote = snprintf(destination, capacity,
                         "Answer in a few sentences: %s", question);
    if (wrote < 0 || (size_t)wrote >= capacity) {
        return -1;
    }
    return wrote;
}

static const char *const PRESETS[ZELL_PRESET_COUNT] = {
    "What time is it?",
    "How much disk is free?",
    "What is three times five?",
    "How do I list panes?",
    "What's the weather in Tokyo?",
};

int zell_scale_for(int height)
{
    int scale;

    if (height < 160) {
        return 2;
    }
    scale = height / 90;
    if (scale < 3) {
        scale = 3;
    }
    if (scale > 6) {
        scale = 6;
    }
    return scale;
}

static float distance_between(float ax, float ay, float bx, float by)
{
    float dx = ax - bx;
    float dy = ay - by;
    return sqrtf(dx * dx + dy * dy);
}

static void place_wizard(zell_scene *scene)
{
    scene->wizard_x = (float)scene->width * 0.5f;
    scene->wizard_y = (float)scene->height * 0.34f;
    if (scene->wizard_y < (float)(scene->scale * 22)) {
        scene->wizard_y = (float)(scene->scale * 22);
    }
}

static void clamp_actor(zell_scene *scene)
{
    float min_y = (float)(scene->scale * 18);
    float max_x = (float)scene->width - 8.0f;

    if (scene->x < 8.0f) {
        scene->x = 8.0f;
    }
    if (scene->x > max_x) {
        scene->x = max_x;
    }
    if (scene->y < min_y) {
        scene->y = min_y;
    }
    if (scene->y > (float)scene->height) {
        scene->y = (float)scene->height;
    }
}

void zell_scene_init(zell_scene *scene, int width, int height)
{
    memset(scene, 0, sizeof *scene);
    if (width < 32) {
        width = 32;
    }
    if (height < 32) {
        height = 32;
    }
    scene->width = width;
    scene->height = height;
    scene->scale = zell_scale_for(height);
    scene->x = (float)width * 0.5f;
    scene->y = (float)height * 0.72f;
    scene->max_hp = 6;
    scene->hp = scene->max_hp;
    scene->route = ZELL_ROUTE_ASK;
    scene->voice_on = true;
    memcpy(scene->answerer, "Kilix Ask", sizeof "Kilix Ask");
    place_wizard(scene);
    clamp_actor(scene);
}

void zell_scene_resize(zell_scene *scene, int width, int height)
{
    float rx = scene->width > 0 ? scene->x / (float)scene->width : 0.5f;
    float ry = scene->height > 0 ? scene->y / (float)scene->height : 0.72f;

    if (width < 32) {
        width = 32;
    }
    if (height < 32) {
        height = 32;
    }
    scene->width = width;
    scene->height = height;
    scene->scale = zell_scale_for(height);
    scene->x = rx * (float)width;
    scene->y = ry * (float)height;
    place_wizard(scene);
    clamp_actor(scene);
}

bool zell_near_wizard(const zell_scene *scene)
{
    float reach = (float)(scene->scale * 22);
    return distance_between(scene->x, scene->y, scene->wizard_x,
                            scene->wizard_y) <= reach;
}

void zell_switch_point(int width, int height, float *x, float *y)
{
    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }
    if (x != NULL) {
        *x = (1.5f * (float)width) / (float)ZELL_ROOM_COLS;
    }
    if (y != NULL) {
        *y = (1.5f * (float)height) / (float)ZELL_ROOM_ROWS;
    }
}

bool zell_near_switch(const zell_scene *scene)
{
    float x;
    float y;
    float reach = (float)(scene->scale * 18);

    zell_switch_point(scene->width, scene->height, &x, &y);
    return distance_between(scene->x, scene->y, x, y) <= reach;
}

static bool blocked(const zell_scene *scene, float x, float y)
{
    float limit = (float)(scene->scale * 10);
    float next = distance_between(x, y, scene->wizard_x, scene->wizard_y);
    float current = distance_between(scene->x, scene->y, scene->wizard_x,
                                     scene->wizard_y);
    return next < limit && next < current;
}

static int tile_index(int span, int cells, float position)
{
    int px;
    int index;

    if (span < 1) {
        return 0;
    }
    px = (int)position;
    if (px < 0) {
        return -1;
    }
    if (px >= span) {
        return cells;
    }
    index = px * cells / span;
    if (index >= cells) {
        index = cells - 1;
    }
    while (index < cells - 1 && (index + 1) * span / cells <= px) {
        index += 1;
    }
    while (index > 0 && index * span / cells > px) {
        index -= 1;
    }
    return index;
}

static int on_stairs(const zell_scene *scene, float x)
{
    int col = tile_index(scene->width, ZELL_ROOM_COLS, x);
    return zell_room_mark(col, ZELL_ROOM_ROWS - 1) == 'u';
}

static int hits_wall(const zell_scene *scene, float x, float y)
{
    float hx = (float)scene->width / (float)ZELL_ROOM_COLS * 0.28f;
    float hy = (float)scene->height / (float)ZELL_ROOM_ROWS * 0.20f;
    float probes_x[3];
    float probes_y[3];
    int index;

    probes_x[0] = x - hx;
    probes_y[0] = y;
    probes_x[1] = x + hx;
    probes_y[1] = y;
    probes_x[2] = x;
    probes_y[2] = y - hy;
    for (index = 0; index < 3; index++) {
        int col = tile_index(scene->width, ZELL_ROOM_COLS, probes_x[index]);
        int row = tile_index(scene->height, ZELL_ROOM_ROWS, probes_y[index]);

        if (probes_y[index] >= (float)scene->height) {
            if (!on_stairs(scene, probes_x[index])) {
                return 1;
            }
            continue;
        }
        if (zell_room_solid(col, row)) {
            return 1;
        }
    }
    return 0;
}

static void copy_text(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (capacity == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void begin_request(zell_scene *scene, const char *question)
{
    copy_text(scene->question, sizeof scene->question, question);
    scene->question_len = (int)strlen(scene->question);
    scene->answer[0] = '\0';
    scene->route = ZELL_ROUTE_ASK;
    copy_text(scene->answerer, sizeof scene->answerer, "Kilix Ask");
    scene->request_ready = true;
    scene->cancel_request = false;
    scene->mode = ZELL_MODE_WAIT;
}

static void walk(zell_scene *scene, const zell_controls *controls)
{
    float speed = (float)(scene->scale + 1);
    float next_x = scene->x;
    float next_y = scene->y;
    bool moved = false;

    if (controls->move_x != 0) {
        next_x += (float)controls->move_x * speed;
        scene->direction = controls->move_x < 0 ? ZELL_DIR_LEFT : ZELL_DIR_RIGHT;
        moved = true;
    }
    if (controls->move_y > 0 && on_stairs(scene, scene->x) &&
        scene->y + (float)controls->move_y * speed > (float)scene->height) {
        scene->leave = true;
        return;
    }
    if (controls->move_y != 0) {
        next_y += (float)controls->move_y * speed;
        if (controls->move_x == 0) {
            scene->direction = controls->move_y < 0 ? ZELL_DIR_UP : ZELL_DIR_DOWN;
        }
        moved = true;
    }
    if (blocked(scene, next_x, scene->y) || hits_wall(scene, next_x, scene->y)) {
        next_x = scene->x;
    }
    if (blocked(scene, next_x, next_y) || hits_wall(scene, next_x, next_y)) {
        next_y = scene->y;
    }
    scene->x = next_x;
    scene->y = next_y;
    clamp_actor(scene);
    if (!moved) {
        scene->step_ticks = 0;
        return;
    }
    scene->step_ticks += 1;
    if (scene->step_ticks >= 6) {
        scene->step_ticks = 0;
        scene->step ^= 1;
    }
}

static int menu_limit(const zell_scene *scene)
{
    if (scene->mode == ZELL_MODE_QUESTION) {
        return ZELL_PRESET_COUNT;
    }
    return 1;
}

static void move_menu(zell_scene *scene, int delta)
{
    int limit = menu_limit(scene);
    scene->menu_index += delta;
    if (scene->menu_index < 0) {
        scene->menu_index = limit - 1;
    }
    if (scene->menu_index >= limit) {
        scene->menu_index = 0;
    }
}

static void type_character(zell_scene *scene, unsigned character)
{
    if (character < 32 || character > 126) {
        return;
    }
    if (scene->question_len >= (int)sizeof scene->question - 1) {
        return;
    }
    scene->question[scene->question_len++] = (char)character;
    scene->question[scene->question_len] = '\0';
}

/* One crossing burns for a little over a second and costs a heart now,
 * then another heart about every 24 ticks until it goes out. */
enum { ZELL_BURN_TIME = 72, ZELL_BURN_GAP = 24, ZELL_RESPAWN_GRACE = 40 };

static int overlaps_fire(const zell_scene *scene)
{
    int index;
    int kilix_scale = scene->scale >= 4 ? 2 : 1;
    float body_top = scene->y - (float)(zell_kilix_foot() * kilix_scale);
    float half = 18.0f * (float)kilix_scale;
    float body_left = scene->x - half;
    float body_right = scene->x + half;

    for (index = 0; index < 2; index++) {
        int x;
        int y;
        int width;
        int height;
        int inset;

        zell_campfire_rect(scene->width, scene->height, index,
                           &x, &y, &width, &height);
        inset = width / 5;
        if (inset < 1) {
            inset = 1;
        }
        x += inset;
        y += inset;
        width -= inset * 2;
        height -= inset * 2;
        if (width < 1 || height < 1) {
            continue;
        }
        if (body_right > (float)x && body_left < (float)(x + width) &&
            scene->y > (float)y && body_top < (float)(y + height)) {
            return 1;
        }
    }
    return 0;
}

static void respawn(zell_scene *scene)
{
    scene->hp = scene->max_hp;
    scene->burn = 0;
    scene->burn_gap = 0;
    scene->grace = ZELL_RESPAWN_GRACE;
    scene->x = (float)scene->width * 0.5f;
    scene->y = (float)scene->height * 0.72f;
    scene->direction = ZELL_DIR_DOWN;
    scene->step = 0;
    scene->step_ticks = 0;
    scene->mode = ZELL_MODE_WALK;
    scene->menu_index = 0;
    scene->menu_hold = 0;
    scene->request_ready = false;
    scene->question_len = 0;
    scene->question[0] = '\0';
    clamp_actor(scene);
}

static void apply_burn(zell_scene *scene)
{
    if (scene->max_hp < 1) {
        scene->max_hp = 6;
        scene->hp = scene->max_hp;
    }
    if (scene->grace > 0) {
        scene->grace -= 1;
        scene->burn = 0;
        return;
    }
    if (scene->mode == ZELL_MODE_WALK && overlaps_fire(scene)) {
        if (scene->burn <= 0) {
            scene->hp -= 1;
            scene->burn_gap = ZELL_BURN_GAP;
        }
        scene->burn = ZELL_BURN_TIME;
    }
    if (scene->burn > 0) {
        scene->burn -= 1;
        if (scene->burn_gap > 0) {
            scene->burn_gap -= 1;
        } else {
            scene->hp -= 1;
            scene->burn_gap = ZELL_BURN_GAP;
        }
    }
    if (scene->hp <= 0) {
        respawn(scene);
    }
}

static bool end_tick(zell_scene *scene)
{
    apply_burn(scene);
    return scene->leave;
}

bool zell_scene_tick(zell_scene *scene, const zell_controls *controls)
{
    scene->flame_tick += 1;
    if (scene->flame_tick >= 600000) {
        scene->flame_tick = 0;
    }
    if (scene->leave) {
        return true;
    }
    if (scene->mode == ZELL_MODE_WALK) {
        walk(scene, controls);
        if (scene->leave) {
            return true;
        }
        if (controls->confirm && zell_near_switch(scene)) {
            scene->voice_on = !scene->voice_on;
            scene->voice_changed = true;
            return end_tick(scene);
        }
        if (controls->confirm && zell_near_wizard(scene)) {
            scene->question[0] = '\0';
            scene->question_len = 0;
            scene->mode = ZELL_MODE_QUESTION;
            scene->menu_index = 0;
        }
        return end_tick(scene);
    }
    if (scene->mode == ZELL_MODE_WAIT) {
        if (controls->cancel) {
            scene->cancel_request = true;
            scene->request_ready = false;
            scene->mode = ZELL_MODE_WALK;
        }
        return end_tick(scene);
    }
    if (controls->cancel) {
        if (scene->mode == ZELL_MODE_TYPE) {
            scene->mode = ZELL_MODE_QUESTION;
        } else {
            scene->mode = ZELL_MODE_WALK;
        }
        return end_tick(scene);
    }
    if (scene->mode == ZELL_MODE_ANSWER) {
        int limit = zell_answer_scroll_limit(scene->width, scene->height,
                                             scene->answer);

        if (scene->answer_scroll > limit) {
            scene->answer_scroll = limit;
        }
        if (scene->answer_scroll < 0) {
            scene->answer_scroll = 0;
        }
        if (controls->move_y != 0) {
            if (scene->menu_hold == 0) {
                scene->answer_scroll += controls->move_y;
                if (scene->answer_scroll > limit) {
                    scene->answer_scroll = limit;
                }
                if (scene->answer_scroll < 0) {
                    scene->answer_scroll = 0;
                }
                scene->menu_hold = 8;
            } else {
                scene->menu_hold -= 1;
            }
        } else {
            scene->menu_hold = 0;
        }
        if (controls->confirm) {
            scene->mode = ZELL_MODE_WALK;
        }
        return end_tick(scene);
    }
    if (scene->mode == ZELL_MODE_TYPE || scene->mode == ZELL_MODE_QUESTION) {
        if (controls->backspace && scene->question_len > 0) {
            scene->question[--scene->question_len] = '\0';
        }
        if (controls->character != 0) {
            type_character(scene, controls->character);
        }
    }
    if (scene->mode == ZELL_MODE_TYPE) {
        if (controls->confirm && scene->question_len > 0) {
            begin_request(scene, scene->question);
        }
        return end_tick(scene);
    }
    if (controls->move_y != 0) {
        if (scene->menu_hold == 0) {
            move_menu(scene, controls->move_y);
            scene->menu_hold = 8;
        } else {
            scene->menu_hold -= 1;
        }
    } else {
        scene->menu_hold = 0;
    }
    if (!controls->confirm) {
        return end_tick(scene);
    }
    if (scene->mode == ZELL_MODE_QUESTION) {
        if (scene->question_len > 0) {
            begin_request(scene, scene->question);
        } else if (scene->menu_index >= 0 &&
                   scene->menu_index < ZELL_PRESET_COUNT) {
            begin_request(scene, zell_question_label(scene->menu_index));
        }
        return end_tick(scene);
    }
    return end_tick(scene);
}

bool zell_scene_take_request(zell_scene *scene, zell_request *request)
{
    if (!scene->request_ready) {
        return false;
    }
    scene->request_ready = false;
    request->route = scene->route;
    request->model[0] = '\0';
    copy_text(request->question, sizeof request->question, scene->question);
    return request->question[0] != '\0';
}

void zell_scene_finish_request(zell_scene *scene, const char *answer)
{
    copy_text(scene->answer, sizeof scene->answer,
              answer != NULL && answer[0] != '\0' ? answer : "No answer.");
    scene->answer_scroll = 0;
    scene->menu_hold = 0;
    scene->answer_serial += 1;
    scene->mode = ZELL_MODE_ANSWER;
    scene->cancel_request = false;
}

void zell_scene_set_answerer(zell_scene *scene, const char *label)
{
    copy_text(scene->answerer, sizeof scene->answerer,
              label != NULL && label[0] != '\0' ? label : "Kilix Ask");
}

const char *zell_route_label(int route)
{
    return route == ZELL_ROUTE_LLM ? "An LLM" : "Kilix Ask";
}

const char *zell_question_label(int index)
{
    if (index >= 0 && index < ZELL_PRESET_COUNT) {
        return PRESETS[index];
    }
    return "Type your own";
}

int zell_request_argv(const zell_request *request, const char **argv, int capacity)
{
    static char prompt[320];
    int count;

    if (request == NULL || argv == NULL || capacity < 8 ||
        request->question[0] == '\0') {
        return -1;
    }
    if (request->route == ZELL_ROUTE_LLM) {
        if (request->model[0] == '\0') {
            return -1;
        }
        count = prompt_for(prompt, sizeof prompt, request->question);
        if (count < 0) {
            return -1;
        }
        argv[0] = "kilix-llm";
        argv[1] = "run";
        argv[2] = "--model-id";
        argv[3] = request->model;
        argv[4] = "-n";
        argv[5] = "160";
        argv[6] = prompt;
        argv[7] = NULL;
        return 7;
    }
    argv[0] = "kilix";
    argv[1] = "ask";
    argv[2] = "--";
    argv[3] = request->question;
    argv[4] = NULL;
    return 4;
}
