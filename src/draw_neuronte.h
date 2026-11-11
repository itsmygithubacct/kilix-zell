#ifndef ZELL_DRAW_NEURONTE_H
#define ZELL_DRAW_NEURONTE_H

#include <stdbool.h>

#include "draw.h"

/* The Neuronte renderer gives the functions in draw.h. These three are
 * its own. Opening is also done on the first string drawn; a program
 * calls zell_neuronte_open() first to find out whether the weights load. */
const char *zell_neuronte_weights(void);
bool zell_neuronte_open(void);
void zell_neuronte_close(void);

#endif
