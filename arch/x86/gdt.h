#ifndef NIMBUS_GDT_H
#define NIMBUS_GDT_H
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
void gdt_init(void);
#endif
