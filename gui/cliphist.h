#ifndef NIMBUS_CLIPHIST_H
#define NIMBUS_CLIPHIST_H
#include "gui/wm.h"

/* Ctrl+Shift+C: list the last copies; picking one pastes it into `target`
 * (may be NULL: the entry is then only made the current clipboard). */
void cliphist_open(window_t *target);
#endif
