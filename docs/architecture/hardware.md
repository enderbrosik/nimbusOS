# Hardware and Drivers

The kernel targets x86-64 under QEMU and compatible BIOS-based virtual
machines. Drivers live in `drivers/`; the hardware interfaces are deliberately
small and mostly polled rather than using a general-purpose device framework.

| Driver | Source | Role |
|---|---|---|
| Serial | `drivers/serial.c` | COM1 diagnostics, including kernel logs |
| VGA text | `drivers/vga_text.c` | Early boot and panic text output |
| PIT | `drivers/pit.c` | Timer ticks and scheduler preemption |
| PS/2 keyboard and mouse | `drivers/keyboard.c`, `drivers/mouse.c`, `drivers/ps2.c` | Input events and pointer movement |
| PCI | `drivers/pci.c` | PCI configuration-space enumeration |
| ATA | `drivers/ata.c` | PIO disk access and installer target disks |
| Framebuffer | `drivers/fb.c` | Bochs/QEMU DISPI linear framebuffer |
| RTC | `drivers/rtc.c` | Clock initialization and time reads |
| AC'97 | `drivers/ac97.c` | Audio output |
| USB | `drivers/usb.c` | USB controller code; USB storage support is limited |
| E1000 | `drivers/e1000.c` | QEMU network adapter |

The default QEMU configuration uses a VGA device, AC'97 audio, an E1000 NIC,
and IDE disks. Hardware not emulated by QEMU may be unsupported or may behave
differently; the project is not a general hardware compatibility promise.

Focused driver and boot checks are listed in
[the testing guide](../development/testing.md).