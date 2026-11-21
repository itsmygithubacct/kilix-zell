#ifndef ZELL_RUNTIME_H
#define ZELL_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

const char *zell_runtime_directory(void);
bool zell_runtime_tool_path(char *path, size_t capacity, const char *name);

#endif
