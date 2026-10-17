/* NimbusOS - system-wide text clipboard shared by all applications.
 * The last CLIP_HISTORY copies are kept in memory (never saved to disk). */
#include "gui/clipboard.h"
#include "kernel/heap.h"
#include "kernel/string.h"

static char *hist[CLIP_HISTORY];      /* hist[0] is the newest copy */
static u32 hist_len[CLIP_HISTORY];
static int nhist;

void clipboard_set(const char *text, u32 len)
{
    char *c = kmalloc(len + 1);
    if (!c) return;
    memcpy(c, text, len);                 /* copy first: text may point into an entry */
    c[len] = 0;

    /* an identical older copy moves to the top; otherwise the oldest is dropped */
    int at = -1;
    for (int i = 0; i < nhist && at < 0; i++)
        if (hist_len[i] == len && memcmp(hist[i], text, len) == 0) at = i;
    if (at < 0) at = nhist < CLIP_HISTORY ? nhist++ : CLIP_HISTORY - 1;

    kfree(hist[at]);
    for (int i = at; i > 0; i--) {
        hist[i] = hist[i - 1];
        hist_len[i] = hist_len[i - 1];
    }
    hist[0] = c;
    hist_len[0] = len;
}

const char *clipboard_get(u32 *len)
{
    if (!nhist) return NULL;
    if (len) *len = hist_len[0];
    return hist[0];
}

int clipboard_history_count(void) { return nhist; }

const char *clipboard_history_get(int i, u32 *len)
{
    if (i < 0 || i >= nhist) return NULL;
    if (len) *len = hist_len[i];
    return hist[i];
}
