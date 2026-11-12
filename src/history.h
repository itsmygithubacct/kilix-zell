#ifndef ZELL_HISTORY_H
#define ZELL_HISTORY_H

#include <stddef.h>

/* Questions the wizard has been asked. The switch only changes whether
 * Ask lists them with the most used first. */
#define ZELL_HISTORY_SCHEMA "kilix.zell.history/v1"
#define ZELL_HISTORY_CAP 12

typedef struct zell_history_call {
    char text[240];
    int count;
} zell_history_call;

const char *zell_history_state_path(void);

/* 1 when the file was read, 0 when it is missing, -1 when it is unusable.
 * Missing and unusable leave enabled off and count at 0. */
int zell_history_parse(const char *text, int *enabled,
                       zell_history_call *calls, int cap, int *count);
int zell_history_load(const char *path, int *enabled,
                      zell_history_call *calls, int cap, int *count);
int zell_history_save(const char *path, int enabled,
                      const zell_history_call *calls, int count);

/* 1 when text is already stored. */
int zell_history_find(const zell_history_call *calls, int count,
                      const char *text);

/* Add amount to a stored call. A new call is stored with that amount.
 * Amount 0 only makes sure the text is present. Returns the slot, or -1. */
int zell_history_remember(zell_history_call *calls, int *count, int cap,
                          const char *text, int amount);

/* Most used first. Equal counts keep the earlier slot. */
void zell_history_order(const zell_history_call *calls, int count,
                        int *order);

#endif
