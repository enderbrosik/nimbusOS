#ifndef NIMBUS_PS2_H
#define NIMBUS_PS2_H
#include "kernel/types.h"
#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64
bool ps2_wait_write(void);
bool ps2_wait_read(void);
void ps2_write_cmd(u8 cmd);
void ps2_write_data(u8 data);
int  ps2_read_data(void);          /* -1 on timeout */
void ps2_controller_init(void);
#endif
