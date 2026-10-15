/* NimbusOS - system-wide text clipboard shared by all applications. */
#include "gui/clipboard.h"
#include "kernel/heap.h"
#include "kernel/string.h"

static char *clip;
static u32 clip_len;

void clipboard_set(const char *text, u32 len)
{
    kfree(clip);
    clip = kmalloc(len + 1);
    if (!clip) { clip_len = 0; return; }
    memcpy(clip, text, len);
    clip[len] = 0;
    clip_len = len;
}

const char *clipboard_get(u32 *len)
{
    if (len) *len = clip_len;
    return clip;
}
