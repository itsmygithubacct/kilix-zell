#include "voice.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int regular_file(const char *path)
{
    struct stat info;

    return path != NULL && path[0] != '\0' &&
           stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

static int can_run(const char *path)
{
    return regular_file(path) && access(path, X_OK) == 0;
}

const char *zell_voice_state_path(void)
{
    const char *env = getenv("KILIX_ZELL_STATE");
    const char *home;
    static char path[400];
    int wrote;

    if (env != NULL && env[0] != '\0') {
        wrote = snprintf(path, sizeof path, "%s/voice.json", env);
    } else {
        home = getenv("GPU_TERMINAL_HOME");
        if (home != NULL && home[0] != '\0') {
            wrote = snprintf(path, sizeof path,
                             "%s/kilix-zell/state/voice.json", home);
        } else {
            home = getenv("HOME");
            if (home == NULL || home[0] == '\0') {
                home = ".";
            }
            wrote = snprintf(path, sizeof path,
                             "%s/.local/gpu_terminal/kilix-zell/state/voice.json",
                             home);
        }
    }
    if (wrote < 0 || (size_t)wrote >= sizeof path) {
        return "voice.json";
    }
    return path;
}

int zell_voice_parse(const char *text, int *enabled)
{
    const char *schema;
    const char *flag;

    if (text == NULL || enabled == NULL) {
        return -1;
    }
    schema = strstr(text, ZELL_VOICE_SCHEMA);
    flag = strstr(text, "\"enabled\"");
    if (schema == NULL || flag == NULL) {
        return -1;
    }
    flag += sizeof "\"enabled\"" - 1;
    while (*flag == ' ' || *flag == '\t' || *flag == '\n' || *flag == ':') {
        flag += 1;
    }
    if (strncmp(flag, "true", 4) == 0) {
        *enabled = 1;
        return 1;
    }
    if (strncmp(flag, "false", 5) == 0) {
        *enabled = 0;
        return 1;
    }
    return -1;
}

int zell_voice_load(const char *path, int *enabled)
{
    FILE *file;
    char text[1024];
    size_t count;

    if (enabled == NULL) {
        return -1;
    }
    *enabled = 1;
    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    count = fread(text, 1, sizeof text - 1, file);
    fclose(file);
    text[count] = '\0';
    if (zell_voice_parse(text, enabled) < 0) {
        *enabled = 1;
        return -1;
    }
    return 1;
}

static int mkdir_one(const char *path)
{
    if (mkdir(path, (mode_t)0700) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int mkdir_parent(const char *file_path)
{
    char path[400];
    size_t length;
    size_t index;

    length = strlen(file_path);
    if (length == 0 || length >= sizeof path) {
        return -1;
    }
    memcpy(path, file_path, length + 1);
    for (index = 1; index < length; index++) {
        if (path[index] != '/') {
            continue;
        }
        path[index] = '\0';
        if (path[0] != '\0' && mkdir_one(path) != 0) {
            return -1;
        }
        path[index] = '/';
    }
    return 0;
}

int zell_voice_save(const char *path, int enabled)
{
    char temporary[420];
    FILE *file;
    int wrote;
    int descriptor;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    if (mkdir_parent(path) != 0) {
        return -1;
    }
    wrote = snprintf(temporary, sizeof temporary, "%s.tmp", path);
    if (wrote < 0 || (size_t)wrote >= sizeof temporary) {
        return -1;
    }
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                      (mode_t)0600);
    if (descriptor < 0) {
        return -1;
    }
    file = fdopen(descriptor, "w");
    if (file == NULL) {
        close(descriptor);
        return -1;
    }
    if (fprintf(file,
                "{\n"
                "  \"schema\": \"%s\",\n"
                "  \"enabled\": %s,\n"
                "  \"engine\": \"paradee\",\n"
                "  \"model\": \"paradee-8m-v1.2\",\n"
                "  \"voice\": \"%s\"\n"
                "}\n",
                ZELL_VOICE_SCHEMA,
                enabled ? "true" : "false",
                ZELL_VOICE_ID) < 0) {
        fclose(file);
        unlink(temporary);
        return -1;
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        fclose(file);
        unlink(temporary);
        return -1;
    }
    if (fclose(file) != 0) {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, path) != 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

int zell_voice_should_speak(int enabled, const char *text)
{
    if (!enabled || text == NULL) {
        return 0;
    }
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text += 1;
    }
    if (*text == '\0') {
        return 0;
    }
    if (strcmp(text, "No answer.") == 0 ||
        strcmp(text, "No question.") == 0 ||
        strcmp(text, "Could not start the question.") == 0 ||
        strcmp(text, "The question took too long.") == 0 ||
        strncmp(text, "The question failed", 19) == 0) {
        return 0;
    }
    return 1;
}

static int join_path(char *destination, size_t capacity,
                     const char *left, const char *right)
{
    int wrote = snprintf(destination, capacity, "%s/%s", left, right);
    return wrote > 0 && (size_t)wrote < capacity;
}

static int copy_path(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (source == NULL || source[0] == '\0') {
        return 0;
    }
    length = strlen(source);
    if (length >= capacity) {
        return 0;
    }
    memcpy(destination, source, length + 1);
    return 1;
}

static int find_on_path(char *destination, size_t capacity, const char *name)
{
    const char *path = getenv("PATH");
    char piece[PATH_MAX];
    size_t index = 0;

    if (path == NULL) {
        return 0;
    }
    while (path[index] != '\0') {
        size_t start = index;
        size_t length;
        while (path[index] != '\0' && path[index] != ':') {
            index += 1;
        }
        length = index - start;
        if (length > 0 && length + 1 + strlen(name) < sizeof piece) {
            memcpy(piece, path + start, length);
            piece[length] = '/';
            memcpy(piece + length + 1, name, strlen(name) + 1);
            if (can_run(piece) && copy_path(destination, capacity, piece)) {
                return 1;
            }
        }
        if (path[index] == ':') {
            index += 1;
        }
    }
    return 0;
}

static int synth_beside_exe(char *destination, size_t capacity)
{
    char exe[PATH_MAX];
    ssize_t count;
    char *slash;

    count = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (count <= 0 || (size_t)count >= sizeof exe) {
        return 0;
    }
    exe[count] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL) {
        return 0;
    }
    *slash = '\0';
    return join_path(destination, capacity, exe, "tools/paradee_speak.py") &&
           regular_file(destination);
}

int zell_voice_prepare(zell_voice_runtime *runtime)
{
    const char *python = getenv("KILIX_PARADEE_PYTHON");
    const char *model_dir = getenv("KILIX_PARADEE_MODEL");
    const char *synth = getenv("KILIX_PARADEE_SYNTH");

    if (runtime == NULL) {
        return 0;
    }
    memset(runtime, 0, sizeof *runtime);
    if (python == NULL || python[0] == '\0') {
        python = "/usr/lib/kilix/pd/bin/python";
    }
    if (model_dir == NULL || model_dir[0] == '\0') {
        model_dir = "/usr/lib/kilix/paradee";
    }
    if (!copy_path(runtime->python, sizeof runtime->python, python) ||
        !can_run(runtime->python)) {
        return 0;
    }
    if (!join_path(runtime->model, sizeof runtime->model, model_dir,
                   "onnx/paradee_int8.onnx") ||
        !regular_file(runtime->model)) {
        return 0;
    }
    if (!join_path(runtime->config, sizeof runtime->config, model_dir,
                   "config.json") ||
        !regular_file(runtime->config)) {
        return 0;
    }
    if (synth != NULL && synth[0] != '\0') {
        if (!copy_path(runtime->synth, sizeof runtime->synth, synth) ||
            !regular_file(runtime->synth)) {
            return 0;
        }
    } else if (!synth_beside_exe(runtime->synth, sizeof runtime->synth)) {
        return 0;
    }
    if (!find_on_path(runtime->player, sizeof runtime->player, "paplay") &&
        !find_on_path(runtime->player, sizeof runtime->player, "aplay")) {
        return 0;
    }
    return 1;
}

int zell_voice_synth_argv(const zell_voice_runtime *runtime, const char *text,
                          const char *wav, const char **argv, int capacity)
{
    if (runtime == NULL || text == NULL || text[0] == '\0' ||
        wav == NULL || wav[0] == '\0' || argv == NULL || capacity < 14 ||
        runtime->python[0] == '\0' || runtime->synth[0] == '\0' ||
        runtime->model[0] == '\0' || runtime->config[0] == '\0') {
        return -1;
    }
    argv[0] = runtime->python;
    argv[1] = "-I";
    argv[2] = runtime->synth;
    argv[3] = "--model";
    argv[4] = runtime->model;
    argv[5] = "--config";
    argv[6] = runtime->config;
    argv[7] = "--voice";
    argv[8] = ZELL_VOICE_ID;
    argv[9] = "--text";
    argv[10] = text;
    argv[11] = "--wav";
    argv[12] = wav;
    argv[13] = NULL;
    return 13;
}
