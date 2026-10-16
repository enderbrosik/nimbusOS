/*
 * NimbusOS - software mixer and system sounds.
 *
 * A kernel thread ("audio") keeps the AC'97 DMA ring topped up: for every
 * free buffer it mixes all active voices, resampled from their own rate to
 * the codec rate with 16.16 fixed-point linear interpolation, scaled by the
 * voice and master volumes and clipped to 16 bits.  Voices only reference
 * their PCM data; the owner keeps it alive while it plays.
 *
 * System sounds are synthesised at boot with integer maths only: a sine
 * table built from Bhaskara I's approximation, notes with an exponential
 * decay envelope, and a noise burst for the explosion.
 */
#include "kernel/sound.h"
#include "drivers/ac97.h"
#include "drivers/pit.h"
#include "kernel/task.h"
#include "kernel/heap.h"
#include "kernel/random.h"
#include "kernel/settings.h"
#include "kernel/string.h"
#include "kernel/kprintf.h"
#include "arch/x86/io.h"

#define MAXV 8
#define SYNTH_RATE 22050

struct voice {
    bool active, paused;
    struct pcm p;
    u64 pos;            /* source frame position, 32.32 fixed point */
    u64 step;
    int vol;            /* 0..256 */
};

static struct voice voices[MAXV];
static int master = 80;                 /* percent */
static bool muted, present;
static struct pcm sys[SND_COUNT];

/* ---------------------------------------------------------------- synth */
static i16 sine_tab[1024];              /* one period */

static void build_sine(void)
{
    /* Bhaskara I: sin(x deg) ~= 4x(180-x) / (40500 - x(180-x)), x in [0,180] */
    for (int i = 0; i < 1024; i++) {
        int d = i * 360 / 1024, sign = 1;
        if (d >= 180) { d -= 180; sign = -1; }
        int num = 4 * d * (180 - d), den = 40500 - d * (180 - d);
        sine_tab[i] = (i16)(sign * 32767 * num / den);
    }
}

/* adds a decaying sine note into buf (mono, SYNTH_RATE) */
static void note(i16 *buf, u32 len, u32 start_ms, u32 dur_ms, u32 hz, int amp)
{
    u32 s0 = start_ms * SYNTH_RATE / 1000, n = dur_ms * SYNTH_RATE / 1000;
    u32 phase = 0, inc = (u32)((u64)hz * 1024 * 65536 / SYNTH_RATE);
    int env = amp << 8;
    for (u32 i = 0; i < n && s0 + i < len; i++) {
        int attack = i < 220 ? (int)i * 256 / 220 : 256;          /* 10 ms fade in */
        int v = sine_tab[(phase >> 16) & 1023] * (env >> 8) / 32768 * attack / 256;
        /* add a quieter octave for warmth */
        v += sine_tab[((phase >> 15)) & 1023] * (env >> 8) / 32768 / 4 * attack / 256;
        int s = buf[s0 + i] + v;
        buf[s0 + i] = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        phase += inc;
        env = (int)((i64)env * 65526 / 65536);   /* about -30 dB per second */
    }
}

static void make(int id, u32 ms)
{
    sys[id].frames = ms * SYNTH_RATE / 1000;
    sys[id].channels = 1;
    sys[id].rate = SYNTH_RATE;
    sys[id].data = kzalloc(sys[id].frames * 2);
}

static void build_system_sounds(void)
{
    build_sine();
    make(SND_STARTUP, 1400);         /* C5 E5 G5 C6 arpeggio, held together */
    static const u32 chord[4] = { 523, 659, 784, 1047 };
    for (int i = 0; i < 4; i++) note(sys[SND_STARTUP].data, sys[SND_STARTUP].frames, i * 130, 1400 - i * 130, chord[i], 7000);
    make(SND_SHUTDOWN, 1100);        /* the same, descending */
    for (int i = 0; i < 4; i++) note(sys[SND_SHUTDOWN].data, sys[SND_SHUTDOWN].frames, i * 150, 1100 - i * 150, chord[3 - i], 7000);
    make(SND_ERROR, 500);
    note(sys[SND_ERROR].data, sys[SND_ERROR].frames, 0, 250, 440, 9000);
    note(sys[SND_ERROR].data, sys[SND_ERROR].frames, 120, 380, 330, 9000);
    make(SND_WARNING, 450);
    note(sys[SND_WARNING].data, sys[SND_WARNING].frames, 0, 450, 587, 9000);
    make(SND_INFO, 400);
    note(sys[SND_INFO].data, sys[SND_INFO].frames, 0, 400, 880, 6000);
    make(SND_CLICK, 40);
    note(sys[SND_CLICK].data, sys[SND_CLICK].frames, 0, 40, 1500, 5000);
    make(SND_EAT, 120);
    note(sys[SND_EAT].data, sys[SND_EAT].frames, 0, 60, 988, 8000);
    note(sys[SND_EAT].data, sys[SND_EAT].frames, 50, 70, 1319, 8000);
    make(SND_WIN, 900);
    static const u32 fanfare[5] = { 523, 659, 784, 1047, 1319 };
    for (int i = 0; i < 5; i++) note(sys[SND_WIN].data, sys[SND_WIN].frames, i * 110, 900 - i * 110, fanfare[i], 6000);
    make(SND_BOOM, 700);             /* decaying, slowly darkening noise */
    i16 *b = sys[SND_BOOM].data;
    int env = 20000, prev = 0;
    for (u32 i = 0; i < sys[SND_BOOM].frames; i++) {
        int n = (int)(rand32() & 0xFFFF) - 32768;
        int k = 2 + (int)(i * 30 / sys[SND_BOOM].frames);   /* stronger low-pass over time */
        prev += (n - prev) / k;
        b[i] = (i16)(prev * env / 32768);
        env = env * 65480 / 65536;
    }
}

/* ---------------------------------------------------------------- mixer */
static void mix(i16 *out, u32 frames, u32 rate)
{
    static i32 acc[AC97_BUF_FRAMES * 2];
    memset(acc, 0, frames * 2 * sizeof acc[0]);
    u32 f = irq_save();
    for (int v = 0; v < MAXV; v++) {
        struct voice *vc = &voices[v];
        if (!vc->active || vc->paused) continue;
        if (!vc->step) vc->step = ((u64)vc->p.rate << 32) / rate;
        int vol = vc->vol;
        for (u32 i = 0; i < frames; i++) {
            u32 idx = (u32)(vc->pos >> 32);
            if (idx + 1 >= vc->p.frames) { vc->active = false; break; }
            u32 frac = (u32)(vc->pos >> 16) & 0xFFFF;
            int l, r;
            if (vc->p.channels == 2) {
                const i16 *s = vc->p.data + idx * 2;
                l = s[0] + (int)(((i64)(s[2] - s[0]) * frac) >> 16);
                r = s[1] + (int)(((i64)(s[3] - s[1]) * frac) >> 16);
            } else {
                const i16 *s = vc->p.data + idx;
                l = r = s[0] + (int)(((i64)(s[1] - s[0]) * frac) >> 16);
            }
            acc[i * 2] += l * vol / 256;
            acc[i * 2 + 1] += r * vol / 256;
            vc->pos += vc->step;
        }
    }
    irq_restore(f);
    int m = muted ? 0 : master * 256 / 100;
    for (u32 i = 0; i < frames * 2; i++) {
        i32 s = acc[i] * m / 256;
        out[i] = s > 32767 ? 32767 : s < -32768 ? -32768 : (i16)s;
    }
}

static void audio_task(void *arg)
{
    u32 rate = ac97_rate();
    for (;;) {
        i16 *b;
        while ((b = ac97_next_buffer())) {
            mix(b, AC97_BUF_FRAMES, rate);
            ac97_submit();
        }
        task_sleep(4);
    }
}

void sound_init(void)
{
    master = settings_get_int("volume", 80);
    muted = settings_get_int("muted", 0);
    if (!ac97_init()) return;
    present = true;
    build_system_sounds();
    task_create("audio", audio_task, NULL);
}

bool sound_available(void) { return present; }

int sound_play(const struct pcm *p, int volume)
{
    if (!present || !p || !p->data || p->frames < 2) return -1;
    u32 f = irq_save();
    int slot = -1;
    for (int v = 0; v < MAXV && slot < 0; v++) if (!voices[v].active) slot = v;
    if (slot < 0) slot = 0;                    /* steal the oldest slot */
    voices[slot] = (struct voice){ true, false, *p, 0, 0, volume };
    irq_restore(f);
    return slot;
}

void sound_stop(int v) { if (v >= 0 && v < MAXV) voices[v].active = false; }
void sound_pause(int v, bool p) { if (v >= 0 && v < MAXV) voices[v].paused = p; }
bool sound_playing(int v) { return v >= 0 && v < MAXV && voices[v].active; }
u32 sound_position(int v) { return v >= 0 && v < MAXV ? (u32)(voices[v].pos >> 32) : 0; }

void sound_seek(int v, u32 frame)
{
    if (v < 0 || v >= MAXV) return;
    u32 f = irq_save();
    if (frame + 2 < voices[v].p.frames) voices[v].pos = (u64)frame << 32;
    irq_restore(f);
}

void sound_event(int which)
{
    if (!present || which < 0 || which >= SND_COUNT || !settings_get_int("system_sounds", 1)) return;
    klog("[snd ] event %d\n", which);
    sound_play(&sys[which], 256);
}

void sound_set_volume(int p) { master = MAX(0, MIN(100, p)); }
int sound_volume(void) { return master; }
void sound_set_muted(bool m) { muted = m; }
bool sound_muted(void) { return muted; }

/* PC speaker: PIT channel 2 square wave gated through port 0x61
 * (OSDev wiki "PC Speaker"). */
void sound_beep(u32 hz, u32 ms)
{
    if (hz < 20) hz = 20;
    u32 div = 1193182 / hz;
    outb(0x43, 0xB6);
    outb(0x42, div & 0xFF);
    outb(0x42, div >> 8);
    outb(0x61, inb(0x61) | 3);
    task_sleep(ms);
    outb(0x61, inb(0x61) & ~3);
}

/* ---------------------------------------------------------------- WAV */
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }

int wav_decode(const u8 *d, u32 len, struct pcm *out)
{
    if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return -1;
    u32 off = 12;
    u16 fmt = 0, ch = 0, bits = 0;
    u32 rate = 0;
    const u8 *data = NULL;
    u32 dlen = 0;
    while (off + 8 <= len) {
        u32 sz = rd32(d + off + 4);
        const u8 *body = d + off + 8;
        if (off + 8 + sz > len) sz = len - off - 8;
        if (!memcmp(d + off, "fmt ", 4) && sz >= 16) {
            fmt = rd16(body); ch = rd16(body + 2); rate = rd32(body + 4); bits = rd16(body + 14);
        } else if (!memcmp(d + off, "data", 4)) {
            data = body; dlen = sz;
        }
        off += 8 + sz + (sz & 1);
    }
    if (fmt != 1 || !data || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate < 4000 || rate > 96000) return -2;
    u32 frames = dlen / (ch * bits / 8);
    i16 *pcm = kmalloc(frames * ch * 2 + 4);
    if (!pcm) return -3;
    for (u32 i = 0; i < frames * ch; i++)
        pcm[i] = bits == 16 ? (i16)rd16(data + i * 2) : (i16)((data[i] - 128) << 8);
    out->data = pcm;
    out->frames = frames;
    out->channels = ch;
    out->rate = rate;
    return 0;
}
