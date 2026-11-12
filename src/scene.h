#ifndef ZELL_SCENE_H
#define ZELL_SCENE_H

#include "history.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ZELL_DIR_DOWN = 0,
    ZELL_DIR_UP = 1,
    ZELL_DIR_LEFT = 2,
    ZELL_DIR_RIGHT = 3
};

enum {
    ZELL_MODE_WALK = 0,
    ZELL_MODE_ROUTE = 1,
    ZELL_MODE_QUESTION = 2,
    ZELL_MODE_TYPE = 3,
    ZELL_MODE_WAIT = 4,
    ZELL_MODE_ANSWER = 5
};

enum {
    ZELL_ROUTE_ASK = 0,
    ZELL_ROUTE_LLM = 1
};

enum {
    ZELL_PRESET_COUNT = 5,
    ZELL_QUESTION_CHOICES = ZELL_PRESET_COUNT + 1
};

typedef struct zell_controls {
    int move_x;
    int move_y;
    bool run;
    bool launcher;
    bool confirm;
    bool cancel;
    bool backspace;
    unsigned character;
} zell_controls;

typedef struct zell_scene {
    int width;
    int height;
    int scale;
    float x;
    float y;
    float wizard_x;
    float wizard_y;
    int direction;
    int step;
    int step_ticks;
    int run_charge, run_direction, run_bump;
    bool running, run_blocked;
    int flame_tick;
    int mode;
    int menu_index;
    int menu_hold;
    int route;
    char question[240];
    int question_len;
    char answer[6000];
    bool request_ready;
    bool cancel_request;
    bool leave;
    int hp;
    int max_hp;
    int burn;
    int burn_gap;
    int grace;
    char answerer[48];
    bool voice_on;
    bool voice_changed;
    bool history_on;
    bool history_changed;
    int call_count;
    zell_history_call calls[ZELL_HISTORY_CAP];
    unsigned answer_serial;
    int answer_scroll;
    /* Presented frames per second. Zero until a sample exists. The
     * Neuronte build draws it; the other build leaves it unused. */
    int frame_rate;
    bool desktop;
    bool desktop_launch;
    int station;
    int mouse_x, mouse_y;
    bool mouse_valid;
    int room; /* cave, command center, laundry */
    int fatigue, mana, update_age_days, mana_age;
    int os_health, dirty_files;
    bool disks_ready;
    int disk_count, disk_used[8];
    char disk_labels[8][96];
    char screen_status[5][96], screen_preview[5][1200];
    bool screens_ready;
    uint32_t *terminal_pixels;
    int terminal_width, terminal_height;
    /* Floating terminal window state. Minimized, its command keeps running
     * and Kilix walks freely; a button on the bottom bar reopens it. */
    bool terminal_maximized, terminal_minimized, terminal_private;
    bool mouse_click; /* an unconsumed left press at mouse_x, mouse_y */
    /* Native station dialogue. Helpers exchange bounded frames over pipes. */
    int service_phase; /* 0 working, 1 text, 2 choices, 3 report, 4 finished */
    char service_title[512];
    char service_text[6000];
    char service_input[1024];
    char service_choices[36][320];
    int service_count, service_index, service_scroll;
    bool service_send, service_cancel;
    char service_reply[1026];
} zell_scene;

typedef struct zell_request {
    int route;
    char model[40];
    char question[240];
    char device[32];
} zell_request;

void zell_scene_init(zell_scene *scene, int width, int height);
void zell_scene_resize(zell_scene *scene, int width, int height);
int zell_scale_for(int height);
bool zell_near_wizard(const zell_scene *scene);
void zell_switch_point(int width, int height, float *x, float *y);
bool zell_near_switch(const zell_scene *scene);

/* One simulation step. True when Kilix has walked off the bottom. */
bool zell_scene_tick(zell_scene *scene, const zell_controls *controls);

bool zell_scene_take_request(zell_scene *scene, zell_request *request);
void zell_scene_finish_request(zell_scene *scene, const char *answer);
void zell_scene_set_answerer(zell_scene *scene, const char *label);

const char *zell_route_label(int route);
const char *zell_question_label(int index);

/* Rows under the Ask field. History off keeps the preset order. History
 * on lists remembered calls, most used first. The next row is the switch. */
int zell_scene_choice_rows(const zell_scene *scene);
const char *zell_scene_choice(const zell_scene *scene, int index);
void zell_history_switch_point(int width, int height, int rows,
                               float *x, float *y);

/* argv is a NULL-terminated list of string literals plus the question.
 * An LLM request with device set also passes --device. Returns the argc,
 * or -1 when the route or question is unusable or argv cannot hold it. */
int zell_request_argv(const zell_request *request, const char **argv, int capacity);

/* Pick one Vulkan device name from `kilix-llm devices` text.
 * A Quadro, GeForce, NVIDIA, or RTX line wins. One listed device is used
 * as-is. Several unmatched devices leave name empty. Returns 1 when set. */
int zell_llm_device_from_list(const char *text, char *name, size_t name_len);

/* Copy the llama-cli reply out of a transcript, after the echoed prompt
 * and without the banner or the timings line. Returns the copied length,
 * 0 when the transcript has no reply text yet, or -1 when text is not
 * that transcript. out may be NULL to measure only. */
int zell_llm_reply(const char *text, char *out, size_t out_len);
int zell_llm_reply_started(const char *text);

#endif
