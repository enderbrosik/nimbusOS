/*
 * NimbusOS - Media Player: plays WAV files through the sound mixer.
 * Open, Play/Pause, Stop, a seek bar (click to jump) and a volume slider.
 */
#include "apps/apps.h"
#include "gui/wm.h"
#include "gui/widgets.h"
#include "gui/dialog.h"
#include "gui/icons.h"
#include "gui/theme.h"
#include "fs/vfs.h"
#include "kernel/sound.h"
#include "kernel/heap.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"

#define W 440
#define H 190

struct player {
    struct pcm pcm;
    char path[NBFS_PATH_MAX];
    int voice;
    bool paused;
    int vol;                /* 0..256 */
    rect_t b_open, b_play, b_stop, seek, volr;
    bool drag_seek, drag_vol;
};

static void fmt_time(u32 frames, u32 rate, char *out)
{
    u32 s = rate ? frames / rate : 0;
    snprintf(out, 16, "%u:%02u", s / 60, s % 60);
}

static void paint(window_t *w)
{
    struct player *p = w->app;
    surface_t *s = w->surface;
    gfx_gradient_v(s, 0, 0, s->w, s->h, RGB(34, 38, 52), RGB(20, 22, 30));
    icon_draw(s, ICON_AUDIO, 16, 16);
    gfx_ui_text_bold(s, 60, 16, p->path[0] ? nbfs_path_basename(p->path) : "No file loaded", 0xFFFFFF);
    char info[96];
    if (p->pcm.data)
        snprintf(info, sizeof info, "%u Hz, %s, 16-bit", p->pcm.rate, p->pcm.channels == 2 ? "stereo" : "mono");
    else
        snprintf(info, sizeof info, "%s", sound_available() ? "Open a .wav file to play it." : "No sound card (start QEMU with -device AC97).");
    gfx_ui_text(s, 60, 38, info, RGB(170, 178, 196));
    /* seek bar */
    u32 pos = sound_playing(p->voice) ? sound_position(p->voice) : 0;
    p->seek = rect_make(16, 76, s->w - 32, 10);
    gfx_round_rect(s, p->seek.x, p->seek.y, p->seek.w, p->seek.h, 4, RGB(60, 66, 84));
    if (p->pcm.frames) {
        int fw = (int)((u64)p->seek.w * pos / p->pcm.frames);
        gfx_round_rect(s, p->seek.x, p->seek.y, MAX(fw, 8), p->seek.h, 4, C_SELECT);
        gfx_fill_circle(s, p->seek.x + fw, p->seek.y + 5, 7, 0xFFFFFF);
    }
    char t1[16], t2[16], tt[40];
    fmt_time(pos, p->pcm.rate, t1);
    fmt_time(p->pcm.frames, p->pcm.rate, t2);
    snprintf(tt, sizeof tt, "%s / %s", t1, t2);
    gfx_ui_text(s, s->w - 16 - gfx_ui_width(tt), 94, tt, RGB(200, 206, 220));
    /* buttons */
    bool playing = sound_playing(p->voice) && !p->paused;
    p->b_open = rect_make(16, 130, 80, 32);
    p->b_play = rect_make(104, 130, 80, 32);
    p->b_stop = rect_make(192, 130, 80, 32);
    ui_button(s, p->b_open, "Open...", BTN_NORMAL);
    ui_button(s, p->b_play, playing ? "Pause" : "Play", p->pcm.data ? BTN_DEFAULT : BTN_DISABLED);
    ui_button(s, p->b_stop, "Stop", p->pcm.data ? BTN_NORMAL : BTN_DISABLED);
    /* volume */
    p->volr = rect_make(s->w - 116, 142, 96, 8);
    gfx_ui_text(s, p->volr.x - 30, 135, "Vol", RGB(170, 178, 196));
    gfx_round_rect(s, p->volr.x, p->volr.y, p->volr.w, p->volr.h, 3, RGB(60, 66, 84));
    int vx = p->volr.x + p->volr.w * p->vol / 256;
    gfx_round_rect(s, p->volr.x, p->volr.y, vx - p->volr.x, p->volr.h, 3, RGB(110, 200, 120));
    gfx_fill_circle(s, vx, p->volr.y + 4, 6, 0xFFFFFF);
    w->mem_hint = p->pcm.frames * p->pcm.channels * 2;
    wm_invalidate(w);
}

static void stop(struct player *p)
{
    if (p->voice >= 0) sound_stop(p->voice);
    p->voice = -1;
    p->paused = false;
}

static void play(window_t *w)
{
    struct player *p = w->app;
    if (!p->pcm.data) return;
    if (sound_playing(p->voice)) {            /* toggle pause */
        p->paused = !p->paused;
        sound_pause(p->voice, p->paused);
    } else {
        p->voice = sound_play(&p->pcm, p->vol);
        p->paused = false;
        klog("[play] playing %s (voice %d)\n", p->path, p->voice);
    }
    paint(w);
}

static bool load(window_t *w, const char *path)
{
    struct player *p = w->app;
    char *data;
    u32 len;
    struct pcm pcm;
    if (vfs_read_all(path, &data, &len) != 0) return false;
    int r = wav_decode((u8 *)data, len, &pcm);
    kfree(data);
    if (r) {
        msgbox(w, "Media Player", "This file is not a supported WAV file (PCM, 8 or 16-bit).", ICON_ERROR, MB_OK, NULL, NULL);
        return false;
    }
    stop(p);
    kfree(p->pcm.data);
    p->pcm = pcm;
    strlcpy(p->path, path, sizeof p->path);
    char t[96];
    snprintf(t, sizeof t, "%s - Media Player", nbfs_path_basename(path));
    wm_set_title(w, t);
    klog("[play] loaded %s: %u frames, %u Hz, %u ch\n", path, pcm.frames, pcm.rate, pcm.channels);
    return true;
}

static void opened(bool ok, const char *path, void *ctx)
{
    window_t *w = ctx;
    if (ok && wm_window_alive(w) && load(w, path)) play(w);
}

static void set_from_mouse(window_t *w, int x)
{
    struct player *p = w->app;
    if (p->drag_seek && p->pcm.frames && sound_playing(p->voice)) {
        int rel = MAX(0, MIN(p->seek.w, x - p->seek.x));
        sound_seek(p->voice, (u32)((u64)p->pcm.frames * rel / p->seek.w));
    }
    if (p->drag_vol) p->vol = MAX(0, MIN(256, (x - p->volr.x) * 256 / p->volr.w));
}

static void on_event(window_t *w, event_t *e)
{
    struct player *p = w->app;
    switch (e->type) {
    case EV_PAINT: paint(w); break;
    case EV_TIMER: paint(w); break;
    case EV_MOUSE_DOWN:
        if (rect_contains(p->b_open, e->x, e->y)) { filedialog(w, FD_OPEN, "Open Sound", "/Music", "", ".wav", opened, w); break; }
        if (rect_contains(p->b_play, e->x, e->y)) { play(w); break; }
        if (rect_contains(p->b_stop, e->x, e->y)) { stop(p); paint(w); break; }
        if (rect_contains(rect_make(p->seek.x, p->seek.y - 6, p->seek.w, p->seek.h + 12), e->x, e->y)) p->drag_seek = true;
        if (rect_contains(rect_make(p->volr.x - 6, p->volr.y - 8, p->volr.w + 12, p->volr.h + 16), e->x, e->y)) p->drag_vol = true;
        set_from_mouse(w, e->x);
        paint(w);
        break;
    case EV_MOUSE_MOVE:
        if (p->drag_seek || p->drag_vol) { set_from_mouse(w, e->x); paint(w); }
        break;
    case EV_MOUSE_UP:
        if (p->drag_vol && p->voice >= 0) {             /* apply the new volume to the running voice */
            u32 pos = sound_position(p->voice);
            bool was = sound_playing(p->voice);
            if (was) { sound_stop(p->voice); p->voice = sound_play(&p->pcm, p->vol); sound_seek(p->voice, pos); sound_pause(p->voice, p->paused); }
        }
        p->drag_seek = p->drag_vol = false;
        break;
    case EV_KEY:
        if (e->key.pressed && e->key.key == ' ') play(w);
        break;
    case EV_DROP:
        if (load(w, e->path)) play(w);
        break;
    case EV_CLOSE: wm_destroy_window(w); break;
    case EV_DESTROY: stop(p); kfree(p->pcm.data); kfree(p); break;
    }
}

void mediaplayer_open(const char *path)
{
    struct player *p = kzalloc(sizeof *p);
    if (!p) return;
    p->voice = -1;
    p->vol = 220;
    window_t *w = wm_create("Media Player", ICON_AUDIO, W, H, 0, on_event, p);
    if (!w) { kfree(p); return; }
    wm_set_timer(w, 250);
    if (path && path[0] && load(w, path)) play(w);
}
