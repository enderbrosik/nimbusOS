#ifndef NIMBUS_CLIPBOARD_H
#define NIMBUS_CLIPBOARD_H
#include "kernel/types.h"
void clipboard_set(const char *text, u32 len);
const char *clipboard_get(u32 *len);      /* NULL if empty */
#endif
