#ifndef NIMBUS_CLIPBOARD_H
#define NIMBUS_CLIPBOARD_H
#include "kernel/types.h"

#define CLIP_HISTORY 5          /* entries kept by Ctrl+Shift+C */

void clipboard_set(const char *text, u32 len);
const char *clipboard_get(u32 *len);      /* NULL if empty */
/* Recent copies, newest first; i = 0 is the current clipboard.  Memory only. */
int         clipboard_history_count(void);
const char *clipboard_history_get(int i, u32 *len);
#endif
