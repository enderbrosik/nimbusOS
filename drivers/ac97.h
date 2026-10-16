#ifndef NIMBUS_AC97_H
#define NIMBUS_AC97_H
#include "kernel/types.h"
#define AC97_BUF_FRAMES 512          /* stereo 16-bit frames per DMA buffer */
bool ac97_init(void);
bool ac97_present(void);
u32  ac97_rate(void);
u16  ac97_iobase(void);
/* Streaming: returns the next free buffer to fill (AC97_BUF_FRAMES frames,
 * interleaved s16 L/R) or NULL when enough audio is already queued. */
i16 *ac97_next_buffer(void);
void ac97_submit(void);               /* queue the buffer returned last */
#endif
