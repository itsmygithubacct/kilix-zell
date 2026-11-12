#include "scene.h"
#include "desktop.h"
#include "room.h"
#include "draw.h"
#include "history.h"
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
    int index;

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
    scene->frame_rate = 0;
    memcpy(scene->answerer, "Kilix Ask", sizeof "Kilix Ask");
    for (index = 0; index < ZELL_PRESET_COUNT; index++) {
        zell_history_remember(scene->calls, &scene->call_count,
                              ZELL_HISTORY_CAP, PRESETS[index], 0);
    }
    place_wizard(scene);
    clamp_actor(scene);
}

void zell_scene_resize(zell_scene *scene, int width, int height)
{
    if (scene->desktop) { scene->width=width<32?32:width;scene->height=height<32?32:height;return; }
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

static int is_preset_text(const char *text)
{
    int index;

    for (index = 0; index < ZELL_PRESET_COUNT; index++) {
        if (strcmp(PRESETS[index], text) == 0) {
            return 1;
        }
    }
    return 0;
}

static int lowest_custom(const zell_scene *scene)
{
    int index;
    int lowest = -1;

    for (index = 0; index < scene->call_count; index++) {
        if (is_preset_text(scene->calls[index].text)) {
            continue;
        }
        if (lowest < 0 ||
            scene->calls[index].count < scene->calls[lowest].count) {
            lowest = index;
        }
    }
    return lowest;
}

static void note_call(zell_scene *scene, const char *question)
{
    int slot;
    int lowest;

    slot = zell_history_remember(scene->calls, &scene->call_count,
                                 ZELL_HISTORY_CAP, question, 1);
    if (slot >= 0) {
        scene->history_changed = true;
        return;
    }
    lowest = lowest_custom(scene);
    if (lowest < 0) {
        return;
    }
    memset(scene->calls[lowest].text, 0, sizeof scene->calls[lowest].text);
    copy_text(scene->calls[lowest].text, sizeof scene->calls[lowest].text,
              question);
    scene->calls[lowest].count = 1;
    scene->history_changed = true;
}

static void begin_request(zell_scene *scene, const char *question)
{
    note_call(scene, question);
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

static int ask_top(int height)
{
    int top = height / 2;
    int fitted = height - 220;

    if (top > fitted) {
        top = fitted;
    }
    if (top < 8) {
        top = 8;
    }
    return top;
}

int zell_scene_choice_rows(const zell_scene *scene)
{
    int wanted;
    int room;
    int rows;

    if (scene == NULL) {
        return 0;
    }
    wanted = scene->history_on ? scene->call_count : ZELL_PRESET_COUNT;
    if (wanted < 0) {
        wanted = 0;
    }
    room = (scene->height - 28) - (ask_top(scene->height) + 70) - 30;
    rows = room / 18;
    if (rows < 1) {
        rows = 1;
    }
    if (rows > wanted) {
        rows = wanted;
    }
    if (rows > ZELL_HISTORY_CAP) {
        rows = ZELL_HISTORY_CAP;
    }
    return rows;
}

const char *zell_scene_choice(const zell_scene *scene, int index)
{
    int order[ZELL_HISTORY_CAP];
    int slot;

    if (scene == NULL || index < 0 || index >= zell_scene_choice_rows(scene)) {
        return "";
    }
    if (!scene->history_on) {
        return zell_question_label(index);
    }
    zell_history_order(scene->calls, scene->call_count, order);
    slot = order[index];
    if (slot < 0 || slot >= scene->call_count) {
        return "";
    }
    return scene->calls[slot].text;
}

void zell_history_switch_point(int width, int height, int rows,
                               float *x, float *y)
{
    int list_bottom;
    int gap_top;
    int gap_bottom;
    int center_y;

    if (width < 1) {
        width = 1;
    }
    if (height < 1) {
        height = 1;
    }
    if (rows < 0) {
        rows = 0;
    }
    list_bottom = ask_top(height) + 70 + rows * 18;
    gap_top = list_bottom + 6;
    gap_bottom = height - 32;
    center_y = gap_top + 11;
    if (gap_bottom - gap_top >= 22) {
        center_y = (gap_top + gap_bottom) / 2;
    }
    if (center_y < list_bottom + 11) {
        center_y = list_bottom + 11;
    }
    if (x != NULL) {
        *x = (float)width * 0.5f;
    }
    if (y != NULL) {
        *y = (float)center_y;
    }
}

static int menu_limit(const zell_scene *scene)
{
    if (scene->mode == ZELL_MODE_QUESTION) {
        return zell_scene_choice_rows(scene) + 1;
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
    if (!scene->desktop) apply_burn(scene);
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
    if (zell_desktop_tick(scene, controls)) return scene->leave;
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
        int limit = zell_answer_scroll_limit(scene->width, scene->height - (scene->desktop ? 42 : 0),
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
        int rows = zell_scene_choice_rows(scene);

        if (scene->question_len > 0) {
            begin_request(scene, scene->question);
        } else if (scene->menu_index == rows) {
            scene->history_on = !scene->history_on;
            scene->history_changed = true;
            if (scene->menu_index > zell_scene_choice_rows(scene)) {
                scene->menu_index = zell_scene_choice_rows(scene);
            }
        } else if (scene->menu_index >= 0 && scene->menu_index < rows) {
            begin_request(scene, zell_scene_choice(scene, scene->menu_index));
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
    request->device[0] = '\0';
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
        if (request->device[0] != '\0') {
            if (capacity < 10) {
                return -1;
            }
            argv[6] = "--device";
            argv[7] = request->device;
            argv[8] = prompt;
            argv[9] = NULL;
            return 9;
        }
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

static int preferred_gpu(const char *description)
{
    return strstr(description, "Quadro") != NULL
        || strstr(description, "GeForce") != NULL
        || strstr(description, "NVIDIA") != NULL
        || strstr(description, "RTX") != NULL;
}

static int copy_vulkan_name(const char *line, char *name, size_t name_len)
{
    size_t i = 6;

    if (strncmp(line, "Vulkan", 6) != 0 || name_len < 8) {
        return 0;
    }
    if (line[i] < '0' || line[i] > '9') {
        return 0;
    }
    while (line[i] >= '0' && line[i] <= '9') {
        i++;
        if (i + 1 >= name_len || i > 16) {
            return 0;
        }
    }
    if (line[i] != ':') {
        return 0;
    }
    memcpy(name, line, i);
    name[i] = '\0';
    return 1;
}

int zell_llm_device_from_list(const char *text, char *name, size_t name_len)
{
    char best[32];
    char only[32];
    int count = 0;
    int have_best = 0;
    const char *cursor;

    if (name == NULL || name_len == 0) {
        return 0;
    }
    name[0] = '\0';
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    best[0] = '\0';
    only[0] = '\0';
    cursor = text;
    while (*cursor != '\0') {
        char line[256];
        char parsed[32];
        size_t n = 0;
        const char *desc;

        while (*cursor != '\0' && *cursor != '\n' && n + 1 < sizeof line) {
            line[n] = *cursor;
            n++;
            cursor++;
        }
        line[n] = '\0';
        if (*cursor == '\n') {
            cursor++;
        }
        if (!copy_vulkan_name(line, parsed, sizeof parsed)) {
            continue;
        }
        count++;
        copy_text(only, sizeof only, parsed);
        desc = strchr(line, ':');
        if (desc != NULL && !have_best && preferred_gpu(desc + 1)) {
            copy_text(best, sizeof best, parsed);
            have_best = 1;
        }
    }
    if (have_best) {
        copy_text(name, name_len, best);
        return name[0] != '\0';
    }
    if (count == 1) {
        copy_text(name, name_len, only);
        return name[0] != '\0';
    }
    return 0;
}

static const char *transcript_body(const char *text)
{
    const char *mark = NULL;
    const char *p;

    if (text == NULL || text[0] == '\0') {
        return NULL;
    }
    if (text[0] == '>') {
        mark = text;
    }
    p = text;
    while ((p = strstr(p, "\n> ")) != NULL) {
        mark = p + 1;
        p += 3;
    }
    if (mark == NULL) {
        return NULL;
    }
    p = strchr(mark, '\n');
    if (p == NULL) {
        return NULL;
    }
    return p + 1;
}

static int is_space_byte(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

int zell_llm_reply(const char *text, char *out, size_t out_len)
{
    const char *body;
    const char *reply;
    const char *cut = NULL;
    const char *prompt_line;
    const char *exiting;
    size_t length;
    size_t skip;

    if (out != NULL && out_len > 0) {
        out[0] = '\0';
    }
    body = transcript_body(text);
    if (body == NULL) {
        return -1;
    }
    prompt_line = strstr(body, "\n[ Prompt:");
    exiting = strstr(body, "\nExiting...");
    if (prompt_line != NULL) {
        cut = prompt_line;
    }
    if (exiting != NULL && (cut == NULL || exiting < cut)) {
        cut = exiting;
    }
    length = cut != NULL ? (size_t)(cut - body) : strlen(body);
    while (length > 0 && is_space_byte(body[length - 1])) {
        length--;
    }
    skip = 0;
    while (skip < length && is_space_byte(body[skip])) {
        skip++;
    }
    reply = body + skip;
    length -= skip;
    if (length == 0) {
        return 0;
    }
    if (length > 1000000u) {
        length = 1000000u;
    }
    if (out == NULL || out_len == 0) {
        return (int)length;
    }
    if (length >= out_len) {
        length = out_len - 1;
    }
    memcpy(out, reply, length);
    out[length] = '\0';
    return (int)length;
}

int zell_llm_reply_started(const char *text)
{
    return zell_llm_reply(text, NULL, 0) > 0;
}
