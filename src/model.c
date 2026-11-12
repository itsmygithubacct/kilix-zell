#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_token(char *destination, size_t capacity, const char *source)
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

static int usable_token(const char *text)
{
    size_t index;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    for (index = 0; text[index] != '\0'; index++) {
        char byte = text[index];
        int ok = (byte >= 'a' && byte <= 'z') ||
                 (byte >= 'A' && byte <= 'Z') ||
                 (byte >= '0' && byte <= '9') ||
                 byte == '.' || byte == '-' || byte == '_';
        if (!ok || index > 30) {
            return 0;
        }
    }
    return 1;
}

static int json_string(const char *text, const char *key,
                       char *out, size_t out_len)
{
    char pattern[40];
    const char *found;
    size_t length = 0;
    int wrote;

    if (out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    wrote = snprintf(pattern, sizeof pattern, "\"%s\"", key);
    if (wrote < 0 || (size_t)wrote >= sizeof pattern) {
        return 0;
    }
    found = strstr(text, pattern);
    if (found == NULL) {
        return 0;
    }
    found += strlen(pattern);
    while (*found == ' ' || *found == '\t' || *found == '\n' || *found == ':') {
        found += 1;
    }
    if (*found != '"') {
        return 0;
    }
    found += 1;
    while (found[length] != '\0' && found[length] != '"' && length + 1 < out_len) {
        out[length] = found[length];
        length += 1;
    }
    out[length] = '\0';
    return found[length] == '"' && usable_token(out);
}

const char *zell_model_state_path(void)
{
    const char *env = getenv("KILIX_ZELL_STATE");
    const char *home;
    static char path[400];
    int wrote;

    if (env != NULL && env[0] != '\0') {
        wrote = snprintf(path, sizeof path, "%s/model.json", env);
    } else {
        home = getenv("GPU_TERMINAL_HOME");
        if (home != NULL && home[0] != '\0') {
            wrote = snprintf(path, sizeof path,
                             "%s/kilix-zell/state/model.json", home);
        } else {
            home = getenv("HOME");
            if (home == NULL || home[0] == '\0') {
                home = ".";
            }
            wrote = snprintf(path, sizeof path,
                             "%s/.local/gpu_terminal/kilix-zell/state/model.json",
                             home);
        }
    }
    if (wrote < 0 || (size_t)wrote >= sizeof path) {
        return "model.json";
    }
    return path;
}

int zell_model_parse(const char *text, char *device, size_t device_len,
                     char *model, size_t model_len)
{
    char parsed_device[40];
    char parsed_model[40];

    if (device == NULL || model == NULL || device_len == 0 || model_len == 0) {
        return -1;
    }
    copy_token(device, device_len, ZELL_MODEL_AUTO);
    copy_token(model, model_len, ZELL_MODEL_AUTO);
    if (text == NULL || strstr(text, ZELL_MODEL_SCHEMA) == NULL) {
        return -1;
    }
    if (!json_string(text, "device", parsed_device, sizeof parsed_device) ||
        !json_string(text, "model", parsed_model, sizeof parsed_model)) {
        return -1;
    }
    copy_token(device, device_len, parsed_device);
    copy_token(model, model_len, parsed_model);
    return 1;
}

int zell_model_load(char *device, size_t device_len,
                    char *model, size_t model_len)
{
    FILE *file;
    char text[1024];
    size_t count;
    const char *path;

    if (zell_model_parse("", device, device_len, model, model_len) < 0) {
        if (device != NULL && device_len > 0) {
            copy_token(device, device_len, ZELL_MODEL_AUTO);
        }
        if (model != NULL && model_len > 0) {
            copy_token(model, model_len, ZELL_MODEL_AUTO);
        }
    }
    path = zell_model_state_path();
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    count = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[count] = '\0';
    if (zell_model_parse(text, device, device_len, model, model_len) < 0) {
        return -1;
    }
    return 1;
}

int zell_model_device(char *out, size_t out_len)
{
    char device[40];
    char model[40];

    if (out == NULL || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (zell_model_load(device, sizeof device, model, sizeof model) != 1) {
        return 0;
    }
    if (strcmp(device, ZELL_MODEL_AUTO) == 0) {
        return 0;
    }
    copy_token(out, out_len, device);
    return out[0] != '\0';
}

void zell_model_id(char *out, size_t out_len)
{
    char device[40];
    char model[40];

    zell_model_load(device, sizeof device, model, sizeof model);
    copy_token(out, out_len, model);
}
