#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Best model first. Slot 2 recognizes retired stock weights but is never routed.
 * The supported no-loop 0.8B entry is the fallback, not an upgrade. */
static const struct zell_weight {
    const char *id;
    const char *file;
    long long size;
    int superior;
} WEIGHTS[ZELL_ROUTER_SLOTS] = {
    {"qwen3.8-27b", "Qwen3.8-27B-UD-Q4_K_M.gguf", 16464440224LL, 1},
    {"qwen3.5-4b", "Qwen3.5-4B-Q6_K.gguf", 3525956768LL, 1},
    {"qwen3.5-0.8b", "Qwen3.5-0.8B-Q8_0.gguf", 833592096LL, 0},
    {"qwen-0.8b-no-loop-02", "qwen-0.8b-no-loop-02.gguf", 833592416LL, 0},
};

const char *zell_models_dir(void)
{
    const char *env = getenv("KILIX_LLM_MODELS_DIR");
    const char *home;
    static char path[400];
    int wrote;

    if (env != NULL && env[0] != '\0') {
        return env;
    }
    home = getenv("GPU_TERMINAL_HOME");
    if (home != NULL && home[0] != '\0') {
        wrote = snprintf(path, sizeof path, "%s/kilix-llm/models", home);
    } else {
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            home = ".";
        }
        wrote = snprintf(path, sizeof path,
                         "%s/.local/gpu_terminal/kilix-llm/models", home);
    }
    if (wrote < 0 || (size_t)wrote >= sizeof path) {
        return ".";
    }
    return path;
}

const char *zell_model_label(const char *model_id)
{
    if (model_id == NULL) {
        return "Qwen3.5 0.8B";
    }
    if (strcmp(model_id, "qwen3.8-27b") == 0) {
        return "Qwen3.8 27B";
    }
    if (strcmp(model_id, "qwen3.5-4b") == 0) {
        return "Qwen3.5 4B";
    }
    return "Qwen3.5 0.8B";
}

static int weight_ready(const char *dir, const struct zell_weight *weight)
{
    char path[512];
    struct stat info;
    int wrote;

    if (dir == NULL || dir[0] == '\0') {
        return 0;
    }
    wrote = snprintf(path, sizeof path, "%s/%s", dir, weight->file);
    if (wrote < 0 || (size_t)wrote >= sizeof path) {
        return 0;
    }
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        return 0;
    }
    return (long long)info.st_size == weight->size;
}

static void add_slot(zell_router_slot *out, int capacity, int *count,
                     const char *id, int superior)
{
    size_t length;

    if (*count >= capacity || *count < 0) {
        return;
    }
    length = strlen(id);
    if (length >= sizeof out[*count].id) {
        length = sizeof out[*count].id - 1;
    }
    memcpy(out[*count].id, id, length);
    out[*count].id[length] = '\0';
    out[*count].superior = superior;
    *count += 1;
}

int zell_router_fill(const int ready[ZELL_ROUTER_SLOTS],
                     zell_router_slot *out, int capacity)
{
    int index;
    int count = 0;

    if (ready == NULL || out == NULL || capacity < 1) {
        return 0;
    }
    for (index = 0; index < ZELL_ROUTER_SLOTS; index++) {
        if (ready[index] && WEIGHTS[index].superior) {
            add_slot(out, capacity, &count, WEIGHTS[index].id, 1);
        }
    }
    /* The stock 0.8B catalog alias was retired by the installed runtime.
     * Never abandon an installed model for a missing or retired fallback. */
    if (ready[3] || count == 0) {
        add_slot(out, capacity, &count, WEIGHTS[3].id, 0);
    }
    return count;
}

int zell_router_prepare(const char *models_dir,
                        zell_router_slot *out, int capacity)
{
    int ready[ZELL_ROUTER_SLOTS];
    int index;

    for (index = 0; index < ZELL_ROUTER_SLOTS; index++) {
        ready[index] = weight_ready(models_dir, &WEIGHTS[index]);
    }
    return zell_router_fill(ready, out, capacity);
}

int zell_router_should_switch(int superior, int stdout_bytes,
                              long elapsed_ms, int more)
{
    return superior && stdout_bytes == 0 &&
           elapsed_ms > ZELL_MODEL_START_MS && more;
}

int zell_router_pin(zell_router_slot *slots, int count, const char *model_id)
{
    int index;

    if (slots == NULL || count < 1 || model_id == NULL || model_id[0] == '\0' ||
        strcmp(model_id, "auto") == 0) {
        return count;
    }
    for (index = 0; index < count; index++) {
        if (strcmp(slots[index].id, model_id) != 0) {
            continue;
        }
        if (index != 0) {
            slots[0] = slots[index];
        }
        slots[0].superior = 0;
        return 1;
    }
    return count;
}

int zell_ask_succeeded(int status, const char *text)
{
    if (status != 0 || text == NULL) {
        return 0;
    }
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text += 1;
    }
    return *text != '\0';
}
