#ifndef ZELL_SCENE_H
#define ZELL_SCENE_H

#include <stdbool.h>
#include <stddef.h>

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
    unsigned answer_serial;
    int answer_scroll;
} zell_scene;

typedef struct zell_request {
    int route;
    char model[40];
    char question[240];
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

/* argv is a NULL-terminated list of string literals plus the question.
 * Returns the argc, or -1 when the route or question is unusable. */
int zell_request_argv(const zell_request *request, const char **argv, int capacity);

#endif
