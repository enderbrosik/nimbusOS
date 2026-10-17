/* NimbusOS - USB: an xHCI host controller driver and USB mass-storage ("USB stick") disks. */
#ifndef NIMBUS_USB_H
#define NIMBUS_USB_H
#include "kernel/types.h"
bool usb_init(void);                         /* finds an xHCI controller and the first mass-storage device on it */
bool usb_disk_present(void);
u32  usb_disk_sectors(void);                 /* 512-byte sectors */
const char *usb_disk_name(void);             /* vendor and product text of the stick */
int  usb_disk_read(u32 lba, u32 count, void *buf);          /* 0 on success */
int  usb_disk_write(u32 lba, u32 count, const void *buf);
#endif
