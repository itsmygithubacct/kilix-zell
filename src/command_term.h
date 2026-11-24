#ifndef ZELL_COMMAND_TERM_H
#define ZELL_COMMAND_TERM_H
#include "scene.h"
#include "neuroterm_terminal.h"

enum { ZELL_TERM_CELL_W=8, ZELL_TERM_CELL_H=16 };
typedef struct {
    neuroterm terminal;
    unsigned long revision;
    const char *message;
} zell_command_screen;

const zell_command_screen *zell_command_term_screen(int index);
void zell_command_term_pump(const zell_scene *scene);
void zell_command_term_retry(int index);
bool zell_command_term_write(int index,const void *bytes,size_t length);
bool zell_command_term_paste(int index,const char *bytes,size_t length);
void zell_command_term_key(int index,VTermKey key,VTermModifier modifiers);
void zell_command_term_shutdown(void);
#endif
