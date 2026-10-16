/* NimbusOS - sound mixer, system sounds and WAV decoding. */
#ifndef NIMBUS_SOUND_H
#define NIMBUS_SOUND_H
#include "kernel/types.h"

enum { SND_STARTUP, SND_SHUTDOWN, SND_ERROR, SND_WARNING, SND_INFO, SND_CLICK,
       SND_EAT, SND_BOOM, SND_WIN, SND_COUNT };

struct pcm {
    i16 *data;          /* interleaved samples */
    u32 frames;
    u32 channels;       /* 1 or 2 */
    u32 rate;           /* Hz */
};

void sound_init(void);                     /* starts the mixer task if a card exists */
bool sound_available(void);
/* Plays pcm (not copied: must stay valid while playing). Returns a voice id or -1. */
int  sound_play(const struct pcm *p, int volume /* 0..256 */);
void sound_stop(int voice);
void sound_pause(int voice, bool paused);
bool sound_playing(int voice);             /* still active (also when paused) */
u32  sound_position(int voice);            /* frames played */
void sound_seek(int voice, u32 frame);
void sound_event(int which);               /* system sound, honours settings */
void sound_set_volume(int percent);        /* master 0..100 */
int  sound_volume(void);
void sound_set_muted(bool m);
bool sound_muted(void);
void sound_beep(u32 hz, u32 ms);           /* PC speaker, blocking (no sound card needed) */

/* Decodes a PCM WAV file (8/16-bit, mono/stereo, any rate) from memory.
 * out->data is kmalloc'ed.  Returns 0 or a negative error. */
int  wav_decode(const u8 *data, u32 len, struct pcm *out);
#endif
