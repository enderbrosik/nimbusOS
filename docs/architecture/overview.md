# Project Architecture

NimbusOS is a freestanding operating system for x86-64. The kernel, drivers,
filesystem, compositor, widgets, and applications are implemented in this
repository. The kernel does not use a hosted OS kernel, libc, or GUI toolkit.

## Source map

| Directory | Responsibility |
|---|---|
| `arch/x86/` | MBR and stage-2 assembly, long-mode entry, interrupts, GDT/IDT, linker script |
| `kernel/` | Boot orchestration, memory, tasks, processes, syscalls, settings, update logic |
| `drivers/` | ATA, PCI, PS/2, USB, framebuffer, serial, RTC, PIT, audio, and text VGA |
| `fs/` | NimbusFS, VFS, FAT32 |
| `gui/` | Graphics, widgets, window manager, desktop, taskbar, menus, dialogs, icons |
| `apps/` | Built-in desktop applications and the app registry |
| `installer/` | Graphical whole-OS disk installer |
| `user/` | Ring-3 C runtime, headers, and example programs |
| `net/` | Network stack, TLS/crypto, DNS/HTTP jobs, and random pool |
| `tools/` | Host image tools, test harnesses, and test programs |
| `rootfs/` | Files copied into the system volume when building an image |
| `web/` | Project website source |

## Startup path

The BIOS loads the MBR from LBA 0. It reads stage 2, which enables A20,
collects the E820 memory map, loads the kernel, switches to long mode, and
calls `kmain`. The kernel initializes CPU tables, memory management, tasking,
drivers, and NimbusFS before starting the desktop task and system monitor.

The live ISO uses El Torito hard-disk emulation. It embeds a RAM-disk-flagged
image, so the boot code copies the OS image into RAM and mounts the filesystem
there. The live system is writable while running, but those changes disappear
when it powers off. See [boot and disk images](boot-and-disk.md).

## Runtime boundaries

Kernel tasks and user processes share the scheduler, but user processes run in
ring 3 with a separate address space. They access kernel services through
`int 0x80`; the kernel validates user pointers and proxies filesystem work to
the GUI task. Built-in desktop apps run as kernel code and use the internal
GUI/VFS APIs directly.

The current user process loader supports static x86-64 ELF64 executables. It
does not implement Windows PE or macOS application-bundle loading. See
[user programs](../development/user-programs.md).