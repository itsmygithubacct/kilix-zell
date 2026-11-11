#ifndef ZELL_ROUTER_H
#define ZELL_ROUTER_H

/* A larger model has this long to write its first answer byte. */
enum { ZELL_MODEL_START_MS = 10000, ZELL_ROUTER_SLOTS = 4 };

typedef struct zell_router_slot {
    char id[40];
    int superior;
} zell_router_slot;

const char *zell_models_dir(void);
const char *zell_model_label(const char *model_id);

/* ready[] follows the fixed order: 27B, 4B, stock 0.8B, no-loop 0.8B. */
int zell_router_fill(const int ready[ZELL_ROUTER_SLOTS],
                     zell_router_slot *out, int capacity);
int zell_router_prepare(const char *models_dir,
                        zell_router_slot *out, int capacity);

int zell_router_should_switch(int superior, int stdout_bytes,
                              long elapsed_ms, int more);
int zell_ask_succeeded(int status, const char *text);

#endif
