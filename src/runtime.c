#define _POSIX_C_SOURCE 200809L
#include "runtime.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

const char *zell_runtime_directory(void)
{
    /* An install can replace the executable while this game is open. Keep
     * the original directory name so new helpers come from the active install. */
    static char directory[PATH_MAX];
    if (!directory[0]) {
        char binary[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", binary, sizeof binary);
        if (n <= 0 || (size_t)n >= sizeof binary) return NULL;
        binary[n] = '\0';
        char *slash = strrchr(binary, '/');
        if (!slash) return NULL;
        if (slash == binary) slash++;
        *slash = '\0';
        memcpy(directory, binary, strlen(binary) + 1);
    }
    return directory;
}

bool zell_runtime_tool_path(char *path, size_t capacity, const char *name)
{
    const char *directory = zell_runtime_directory();
    if (!directory) return false;
    int n = snprintf(path, capacity, "%s/tools/%s", directory, name);
    return n > 0 && (size_t)n < capacity;
}
