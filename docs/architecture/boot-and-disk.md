# Boot and Disk Images

## Boot stages

1. BIOS loads the 512-byte MBR at `0x7C00`.
2. The MBR reads the boot-info block and loads stage 2 from the reserved boot
   area.
3. Stage 2 enables A20, records the E820 memory map, copies the VGA ROM font,
   loads the kernel, and transitions through protected mode to x86-64 long
   mode.
4. The kernel entry establishes paging and calls `kmain` with the boot-info
   structure.
5. Kernel initialization starts the scheduler and drivers, mounts the system
   volume, and launches the desktop.

Both boot loaders fall back to BIOS CHS reads when LBA extensions are not
available.

## Installed disk layout

The default `build/nimbus.img` is 64 MiB. Its first partition is type `0x4E`
and contains NimbusFS.

| LBA range | Contents |
|---|---|
| 0 | MBR, boot info, disk signature, partition table |
| 1-63 | Stage 2 |
| 64-2079 | Kernel slot A |
| 2080-4095 | Kernel slot B |
| 4096-end | NimbusFS system partition |

The alternate kernel slot supports updates: a new kernel is written and
verified in the inactive slot before the MBR changes which slot boots.

## Live ISO

`make iso` builds `build/nimbus.iso`. The ISO contains an El Torito boot entry
using hard-disk emulation and an embedded 32 MiB disk image marked as a RAM
disk. Stage 2 loads this image into memory, allowing the kernel to mount the
system volume without writing to the read-only CD. The live system is not
persistent across power-off.

## QEMU disks

`make run` boots the installed-style disk image. `make run-iso` boots the
live CD. `make installer-test` attaches a separate `build/target.img` as the
installer target; this is a regular image file, not a physical drive. The
installer protects the live boot disk and requires an explicit erase
confirmation for the target. Never point QEMU at a physical block device
unless you intentionally want to expose it to the guest.