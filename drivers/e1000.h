#ifndef NIMBUS_E1000_H
#define NIMBUS_E1000_H
#include "kernel/types.h"
bool e1000_init(void);
bool e1000_present(void);
void e1000_mac(u8 out[6]);
bool e1000_link_up(void);
int  e1000_send(const void *frame, u32 len);   /* 0 on success */
int  e1000_poll(void *buf, u32 max);            /* frame length, or 0 if none */
#endif
