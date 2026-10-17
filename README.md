# NimbusOS

A small hobby operating system for x86-64 (64-bit long mode), written from scratch in C with a
little assembly. It boots from its own MBR bootloader into a Windows-style
desktop with draggable, resizable windows, a taskbar and Start menu, a File
Explorer, Notepad, Paint, a Terminal, a Task Manager and a graphical installer.
Everything (bootloader, kernel, drivers, file system, compositor, widgets,
apps) is original code: no existing kernel, libc or GUI library is used.

![Desktop with the Start menu](docs/screenshots/stage5-start-menu.png)

| | |
|---|---|
| ![apps](docs/screenshots/stage6-apps.png) | ![paint](docs/screenshots/stage6-paint.png) |
| ![task manager](docs/screenshots/stage6-taskmgr.png) | ![installer](docs/screenshots/stage7-confirm.png) |

---

## Quick start

```sh
make build        # -> build/nimbus.img (64 MiB bootable disk image)
make run          # boot it in QEMU (serial log on the terminal)
make test         # host-side file system test suite
make test-all     # host suite + every QEMU stage test (headless)
```

| Target | What it does |
|---|---|
| `make build` | Assembles the bootloader, compiles the kernel, builds the host tools, creates `build/nimbus.img` and populates its NimbusFS partition from `rootfs/` plus generated wallpapers |
| `make run` | `qemu-system-x86_64 -m 256M -vga std` on the image, COM1 on stdio |
| `make debug` | Same, but QEMU starts paused with a GDB stub on `:1234` (prints the `gdb` command to use; symbols come from `build/kernel.elf`) |
| `make iso` | Builds the bootable live CD `build/nimbus.iso` |
| `make run-iso` | Boots the ISO in QEMU as a CD-ROM |
| `make clean` | Removes `build/` |
| `make test` | Builds and runs `tools/fstest` (NimbusFS test suite, on a scratch image file) |
| `make fsimage` | Uses the formatter to create an empty standalone file system image `build/empty-fs.img` |
| `make installer-test` | Boots the live image with a **blank** 64 MiB `build/target.img` attached as the second drive |
| `make run-target` | Boots only `build/target.img` (after installing onto it) |
| `make test-all` | Runs everything automated (≈ 2 minutes) |

Add QEMU options with `QEMU_EXTRA`, e.g. `make run QEMU_EXTRA="-display sdl"`.

> **Safety.** The build and every tool only operate on regular `.img`
> files they create and name explicitly. `mkimage` and `nfstool` refuse to
> open block/character devices, and nothing uses `dd`, `fdisk`, `mkfs` or
> `diskutil`. The installer only runs inside QEMU against the attached image
> files, and only after an explicit confirmation (see Stage 7).

## Live CD (`make iso`)

`build/nimbus.iso` is an ISO 9660 image with an El Torito boot entry using
**hard-disk emulation**, written by `tools/mkiso.c` (no external ISO tools
are needed). The BIOS presents the embedded 32 MiB copy of the disk image as
drive 0x80, so the normal MBR and stage 2 boot from it.

* That copy carries a `RAMD` flag, so stage 2 loads the *whole* image into RAM
  at 16 MiB. The kernel then mounts its file system from that RAM disk, which
  is needed because a CD is read-only and there is no ATAPI driver.
* It is a true live system: everything works, but changes are lost at power-off.
* Run the installer from the live CD to put NimbusOS on a hard disk; the
  installed disk boots normally from ATA.
* Both boot loaders fall back to CHS reads (INT 13h AH=02h, geometry from
  AH=08h) when the BIOS has no LBA extensions, which is typical for emulated
  disks.

```sh
make iso && make run-iso
# VirtualBox / VMware: create an "Other/Unknown (64-bit)" VM with a 64+ MiB IDE disk,
# attach nimbus.iso as the CD, use the standard VGA adapter.
```

## Installing the toolchain

You need: a cross compiler for `x86_64-elf` (GCC **or** Clang + LLD), NASM, a host C
compiler, QEMU, GNU make and Python 3 (Pillow is optional: it converts test
screenshots to PNG and checks Paint's BMP output).

The Makefile auto-detects the compiler: if `x86_64-elf-gcc` is on `PATH` it uses
`x86_64-elf-gcc` + `x86_64-elf-ld`, otherwise `clang --target=x86_64-elf` + `ld.lld`.
Override with `make TOOLCHAIN=gcc` / `make KCC=... KLD=...`.

**macOS (Homebrew)**
```sh
brew install x86_64-elf-gcc x86_64-elf-binutils nasm qemu  # GCC cross toolchain
# or, instead of x86_64-elf-gcc:  brew install llvm lld     (Clang path)
python3 -m pip install pillow                               # optional
```

**Debian / Ubuntu**
```sh
sudo apt install build-essential nasm qemu-system-x86 clang lld python3-pil
```

**Building x86_64-elf-gcc from source** (any Unix; see OSDev wiki "GCC Cross-Compiler"):
```sh
export PREFIX="$HOME/opt/cross" TARGET=x86_64-elf PATH="$HOME/opt/cross/bin:$PATH"
# binutils
../binutils-2.42/configure --target=$TARGET --prefix="$PREFIX" --with-sysroot --disable-nls --disable-werror
make && make install
# gcc (C only, no libc)
../gcc-14.1.0/configure --target=$TARGET --prefix="$PREFIX" --disable-nls --enable-languages=c --without-headers
make all-gcc all-target-libgcc && make install-gcc install-target-libgcc
```
The kernel does not link libgcc. It is built with `-mno-red-zone` (interrupts
run on the same stack) and `-mno-sse` (no FPU/SSE state is saved on a task switch).

## Source layout

```
arch/x86/      boot/mbr.asm, boot/stage2.asm, entry.asm, isr.asm, GDT, IDT/PIC, linker script
kernel/        main, printf, string, panic, PMM, paging (VMM), heap, tasks/scheduler, ACPI power, self tests
drivers/       serial, VGA text, PIT, PS/2 controller, keyboard, mouse, PCI, framebuffer (Bochs VBE), ATA PIO, RTC,
               AC'97 audio, e1000 network
fs/            nbfs.[ch] NimbusFS (shared with host tools), vfs.[ch] volume glue for the kernel
gui/           gfx (primitives + font), wm (compositor/window manager), desktop, taskbar, menu, widgets,
               dialogs (message/input/file), icons, cursor, BMP codec, clipboard
apps/          explorer, notepad, paint, terminal, taskmgr, sysprops, settings, minesweeper, snake,
               solitaire, mediaplayer, web browser, calculator (+ calc_math.h arithmetic), app registry
net/           TCP/IP stack (net.c), TLS 1.3 client (tls.c), crypto (crypto.c), random pool (rng.c),
               background network jobs (netjob.c)
installer/     graphical installer
user/          user-mode C library (lib/), headers (include/), sample programs (programs/), linker script
tools/         mkimage, nfstool (formatter + file tool), fstest (FS test suite), mkwallpaper,
               qemu_harness.py + test_stage1..7.py (headless QEMU tests)
rootfs/        files copied into the system volume at build time
```

## Boot sequence

```
BIOS ──► LBA 0: MBR (arch/x86/boot/mbr.asm, 512 bytes, loaded at 0x7C00)
          │  reads its boot-info block (patched by tools/mkimage) and loads
          │  stage 2 with INT 13h AH=42h (LBA extended read)
          ▼
         LBA 1..: stage 2 (arch/x86/boot/stage2.asm, at 0x8000, real mode)
          │  1. enable A20 (BIOS INT 15h/2401 + port 0x92)
          │  2. E820 memory map  -> 0x5100
          │  3. copy the 8x16 VGA ROM font (INT 10h AX=1130h) -> 0x6000
          │  4. load the kernel 32 KiB at a time into a bounce buffer at
          │     0x20000 and copy it to 1 MiB using "unreal mode"
          │  5. load a flat GDT, set CR0.PE, far jump to 32-bit code
          ▼
         0x100000: kernel entry (arch/x86/entry.asm, 32-bit part): zero .bss,
          │  check CPUID for long mode, build boot page tables (first 1 GiB,
          │  2 MiB pages), CR4.PAE, CR3, EFER.LME, CR0.PG, load a 64-bit GDT
          │  and far jump to 64-bit code; set up the boot stack and call
          │  kmain(bootinfo*)   (boot info at 0x5000)
          ▼
         kmain (kernel/main.c)
            serial + VGA text log, GDT + TSS, IDT + PIC remap  (stage 1)
            PMM, paging, heap, PIT, scheduler, self tests      (stage 2)
            PS/2 kbd + mouse, ATA, PCI                         (stage 3)
            mount NimbusFS from the boot disk, FS self test    (stage 4)
            ACPI, font, framebuffer 1024x768x32, start the
            "desktop" task (window manager + apps) and the
            "sysmon" task                                      (stages 5-7)
```

### Disk layout (`build/nimbus.img`, 64 MiB)

| LBA | Contents |
|---|---|
| 0 | MBR: boot code, boot-info block at 0x1A0 (`NBT1`, stage 2 + kernel location), disk signature at 0x1B8, partition table |
| 1 – 63 | stage 2 |
| 64 – 2079 | kernel slot A (flat binary, up to ~1 MiB) |
| 2080 – 4095 | kernel slot B (used by Nimbus Update; the MBR says which slot boots) |
| 4096 – end | partition 1, type `0x4E`: NimbusFS system volume |

### Memory layout

| Virtual | Use |
|---|---|
| `0x00000000` | unmapped page (NULL-pointer guard) |
| `0x00001000 – RAM top` | identity mapped (all RAM below 3 GiB, 2 MiB pages): BIOS data, boot info + font (0x5000/0x6000), kernel at 1 MiB, PMM bitmap, page tables, live-CD RAM disk |
| `0xC0000000 – 0xCFFFFFFF` | kernel heap, grown page by page from the PMM |
| `0xD0000000 – 0xD1FFFFFF` | DMA buffers and device registers (uncached) |
| `0xE0000000 – …` | linear framebuffer (physical address from PCI BAR0) |
| `0x80_0000_0000 – …` | user programs: one address space (PML4) per process |

ACPI tables and other MMIO are identity mapped on demand.

---

## Stage 1 – Boot

**What's there:** `mbr.asm` (stage 1), `stage2.asm`, `entry.asm` (switch to 64-bit long mode), a long-mode GDT with a TSS
(`arch/x86/gdt.c`), 256 64-bit IDT gates generated by a NASM macro (`isr.asm`), the
8259 PIC remapped to vectors 0x20–0x2F, a VGA text console and a COM1 serial
logger (115200 8N1). Every kernel log line goes to the serial port.

**Run:** `make run` and look at the first second of output, or `python3 tools/test_stage1.py`.

**Expect:** `NimbusOS MBR`, `NimbusOS stage2`, `Loading kernel....... ok` from
the BIOS, then the kernel's log: GDT/IDT loaded, an `int3` breakpoint exception
that is handled and returns.

![stage 1](docs/screenshots/stage1-boot.png)

## Stage 2 – Core kernel

* **Physical memory:** bitmap allocator, one bit per 4 KiB frame over 4 GiB,
  initialised from the E820 map (`kernel/pmm.c`).
* **Paging:** x86-64 four-level paging (`kernel/vmm.c`). All RAM is identity
  mapped, so page tables are edited through their physical address; NULL
  guard page, heap, DMA, MMIO and framebuffer windows as above; per-process
  address spaces share the kernel's low 4 GiB.
* **Heap:** `kmalloc/kzalloc/krealloc/kfree` with 16-byte headers, next-fit
  allocation, splitting, coalescing of both neighbours, magic numbers to catch
  corruption, `heap_check()` walker (`kernel/heap.c`).
* **Timer + scheduler:** PIT at 1 kHz; preemptive round-robin kernel threads
  with a 10 ms quantum. The context switch is done by the IRQ stub, which
  returns a different task's saved register frame; `task_yield`/`task_sleep`
  use a software interrupt. Per-task CPU accounting feeds Task Manager.
* **Panic:** exceptions and `panic()` dump RAX–R15, RIP, RFLAGS, CS/SS and
  control registers (CR2 for page faults) to serial and to the screen (VGA
  text early, a blue framebuffer screen once graphics are on).

**Test:** `python3 tools/test_stage2.py` runs the boot self tests (PMM, VMM,
2000 random heap operations with content checks, three tasks, one of which never
yields and so needs preemption). It then builds a second kernel with
`-DPANIC_TEST`, which writes to an unmapped address, and checks the register dump.

![panic](docs/screenshots/stage2-panic.png)

## Stage 3 – Drivers

* **PS/2 keyboard:** scancode set 1 (controller translation on), `0xE0`
  extended keys, Shift/Ctrl/Alt/Win/Caps Lock, a 128-entry key event queue.
* **PS/2 mouse:** 3-byte packets with bit-3 resynchronisation, overflow
  rejection, absolute position clamped to the screen, button state. Motion is
  coalesced in the queue, but button changes are never merged, so clicks keep
  their exact position.
* **Graphics:** Bochs/QEMU "DISPI" VBE extension (ports 0x1CE/0x1CF), LFB
  address from PCI BAR0, 1024×768×32. `gui/gfx.c` has pixel, Bresenham line,
  thick line, rect, filled rect, alpha-blended rect, gradient, midpoint
  circle/filled circle, rounded rect, opaque/ARGB/scaled blits and 8×16 text.
  All of it is clipped to a per-surface clip rectangle. The font is the VGA
  BIOS ROM font, copied by stage 2.
* **ATA PIO:** IDENTIFY on all four IDE positions, LBA28 READ/WRITE SECTORS
  with polling, FLUSH CACHE after writes.
* **Also:** PCI config mechanism #1, CMOS RTC with BCD/12h handling.

**Test:** `python3 tools/test_stage3.py` attaches a second blank disk and
checks that both drives are identified and the framebuffer mode is set. It then
types `echo Hello, World! (Caps & Symbols: ~!@#$%^&*_+{}|:<>?)` into the
Terminal and checks it arrives exactly, and double-clicks an icon at exact
coordinates. (Before Stage 5 replaced it, this stage had a dedicated
driver-demo screen; see the git history, commit "Stage 3".)

## Stage 4 – File system: NimbusFS

**Design decision: custom FS vs FAT.**
* FAT16/32 could be mounted on the host directly. But long names need the
  VFAT LFN extension, timestamps are coarse, and cluster chains make random
  writes and metadata awkward.
* A small ext2-style design is less code for more features. I chose
  **NimbusFS**; the cost is that you inspect images with our own `nfstool`.

**On-disk format** (`fs/nbfs.h`), 1 KiB blocks:
* Layout: block 0 is reserved, block 1 is the superblock, followed by the
  block bitmap, the inode bitmap, the inode table and the data blocks.
* Inodes are 128 bytes: type, links, size, `ctime`, `mtime`, parent inode,
  24 direct pointers, 1 indirect and 1 double-indirect pointer (≈64 MiB per file).
* Directories are files of 64-byte entries (inode, type, name length, 57-char
  name). Inode 1 is the root.

**Operations:** format, mount, lookup, stat, create, mkdir, read, write
(sparse-safe), truncate (zero-fills the tail), unlink, recursive delete,
rename/move (refuses to move a directory into itself), readdir, get-path, and
`nbfs_check` (a full consistency checker). A 64-entry write-through block cache
sits in front of the device.

`fs/nbfs.c` is compiled **unchanged** into the kernel and into the host tools:

* `nfstool mkfs <new.img> <MiB>` is the formatter: it creates a fresh, empty
  file system image (`make fsimage`).
* `nfstool format|ls|tree|cat|put|get|mkdir|rm|mv|import|check|df|blank`
  works on plain FS images or on the NimbusFS partition inside a disk image.
* `make test` runs `tools/fstest` (6,700+ checks). It covers direct, indirect
  and double-indirect files, chunked unaligned writes, truncate + regrow,
  nested directories, 100-entry directories with slot reuse, rename/move, name
  limits, timestamps, disk-full, persistence across remount, and a 3,000-step
  randomized run against an in-memory model. `fsck` runs after every test.
  (The suite found a real block leak in directory shrinking during development.)

**Kernel side:** `fs/vfs.c` finds the boot disk by matching its MBR disk
signature with the one the BIOS booted from, then mounts partition type `0x4E`.
At boot, a self test creates, writes, reads, renames and deletes on the real
ATA disk, then runs the checker (`python3 tools/test_stage4.py`).

## Stage 5 – Desktop environment

**Window manager / compositor** (`gui/wm.c`):
* **Rendering:** each window owns a client surface. Damaged screen rectangles
  are collected (and merged), and only those are recomposed into a back
  buffer: wallpaper → icons → windows bottom-to-top (drop shadow, 1 px border,
  gradient title bar with icon, title and min/max/close buttons, client
  pixels) → dragged icon → taskbar → popup menus. Then only those rectangles
  are copied to the framebuffer, so there is no flicker.
* **Cursor:** drawn last, directly on the framebuffer, with the pixels under it
  saved and restored. There are resize/text/crosshair shapes.
* **Window operations:** move (title bar), resize (all edges and corners,
  ±4 px grab zone), minimize, maximize/restore (button or title double-click),
  close, z-order, focus. Keyboard focus follows the active window. Alt+Tab
  cycles windows, Alt+F4 closes. Modal dialogs block their owner.
* **Event routing:** popup menu → taskbar → windows top-to-bottom (with mouse
  capture while a button is held) → desktop.

**Desktop** (`gui/desktop.c`):
* **Wallpaper** comes from `/System/desktop.cfg` (default
  `/System/wallpaper.bmp`), falling back to a solid colour.
* **Icons** in a grid: one per app plus one per entry of `/Desktop`.
  Single-click selects, double-click opens, and drag snaps to the grid. Icon
  positions are saved in `/System/icons.cfg`.
* **Right-click menu:** New Folder, New Text File (created, then renamed
  inline), Change Wallpaper (live chooser with preview), Refresh, Properties.
  Icons have their own Open/Rename/Delete/Properties menu.
* The desktop notices changes made by other apps.

**Taskbar** (`gui/taskbar.c`):
* Start button, one button per window (click to focus, minimize or restore;
  right-click for a window menu), and a tray with a memory meter.
* The clock reads the RTC at boot and every minute, and advances with the PIT
  in between.
* The Start menu (button, Win key or Ctrl+Esc) lists the apps, System
  Properties, Restart and Shut Down.
* **Shut Down** uses ACPI S5: RSDP → RSDT → FADT → `\_S5` in the DSDT, then
  `SLP_TYP|SLP_EN` to `PM1a_CNT`. **Restart** uses the 8042 reset line, with a
  triple-fault fallback.

**Run:** `make run`.
**Expect:** the desktop above; double-click icons, drag windows, right-click
the desktop.
**Test plan** (`python3 tools/test_stage5.py`):
* Icons appear.
* Double-click opens a window.
* Title-bar drag and corner resize land at exact coordinates.
* Minimize, then the taskbar button restores the window.
* Right-click shows the menu; New Folder + rename creates `/Desktop/Projects`.
* The Start menu opens; icon drag position is persisted.
* Alt+Tab switches windows.
* Shut Down powers QEMU off; Restart reboots it.

![context menu](docs/screenshots/stage5-context-menu.png)

## Stage 6 – Applications

| App | Features |
|---|---|
| **File Explorer** | Back / Up / Refresh, address bar, Places sidebar, details list (icon, name, size, modified), double-click opens folders or files (`.txt` → Notepad, `.bmp` → Paint), right-click: Open / Rename / Delete / Properties or New Folder / New Text File / Refresh / Open Terminal Here; keyboard: arrows, Enter, Backspace, Delete, F2, F5; auto-refresh |
| **Notepad** | multi-line editing, caret, mouse and Shift-selection, double-click word selection, insert/delete, Tab, scrolling (scrollbar, PgUp/PgDn, horizontal follow), Ctrl+A/C/X/V (system clipboard), New / Open / Save / Save As (file dialog), "save changes?" prompts on New/Open/Close |
| **Paint** | pencil, brush, eraser, line, rectangle, filled rectangle, flood fill, colour picker; 28-colour palette (left = primary, right = secondary), 4 brush sizes, Clear, live shape preview, scrollable canvas, open/save 24-bit BMP |
| **Terminal** | `ls [-l]`, `cd`, `pwd`, `cat`, `echo`, `mkdir`, `rm [-r]`, `touch`, `mv`, `cp`, `tree`, `stat`, `open`, `df`, `mem`, `ps`, `date`, `uptime`, `fsck`, `history`, `uname`, `clear`, `exit`, `shutdown`, `reboot`, app launchers; `>` / `>>` redirection of any command; history (↑/↓), scrollback (PgUp/PgDn) |
| **Task Manager** | kernel tasks (state, CPU %, stack), application windows with memory use, End Task, physical memory / heap stats, CPU history graph |

**Test:** `python3 tools/test_stage6.py`.
* Explorer: navigation, back/up, create + rename + delete through the context
  menu, opening a file.
* Notepad: edit + Ctrl+S, then `nfstool cat` confirms the content.
* Terminal: a dozen commands including redirection; results are verified on
  the image.
* Task Manager: End Task.
* Paint: draw with six tools, save via the file dialog, reopen it, and check
  pixel colours of the BMP on the host with Pillow.

![explorer](docs/screenshots/stage6-explorer.png)

## Stage 7 – Installer

`installer/installer.c` is a wizard on the live system: **Welcome → Choose
disk → Confirm → Install → Done/Restart.**

* **Choose disk** lists every attached ATA drive with model and size. The live
  boot disk (found by MBR signature) and disks under 16 MiB are shown but
  cannot be selected. Nothing is pre-selected.
* **Confirm** is a red warning page. It shows the drive name, model, size in
  MiB and sectors, and what is on it now (blank / NimbusOS / other partitions).
  It states that *all existing data on this drive will be destroyed*. The
  **Erase and Install** button stays disabled until you tick *"I understand
  that all data on … will be destroyed"*. No byte is written before this
  point, and the target is re-checked right before the first write.
* **Install**, driven by a timer so the UI stays responsive:
  1. Copy stage 2 + kernel (LBA 1–4095).
  2. Create a NimbusFS partition covering the rest of the disk.
  3. Copy every file and folder of the live volume.
  4. Write `/System/installed.txt` (this hides the installer icon on the
     installed system).
  5. Run the consistency checker.
  6. Write the MBR **last**, with a fresh disk signature, so an interrupted
     install never leaves a bootable half-written disk.
* **Done:** Restart.

**Run it yourself:**
```sh
make installer-test       # live image + blank build/target.img (Primary Slave)
# in QEMU: double-click "Install NimbusOS", pick "Primary Slave", tick the box, Erase and Install
make run-target           # boot the freshly installed disk on its own
```
**Automated test** (`python3 tools/test_stage7.py`):
* Proves the live disk can't be picked, and that "Erase and Install" does
  nothing while unticked: the target is still all zeros at that point.
* Installs, restarts, and checks the live image's boot area is byte-identical.
* Checks the target's MBR, runs `nfstool check`, and compares the file tree.
* Boots the target alone to the desktop: it mounts from Primary Master and the
  installer icon is gone.

![done](docs/screenshots/stage7-done.png)

## User mode: programs, system calls, libc

NimbusOS runs **user programs in ring 3**, each in its own address space.
The samples live in `/Programs` (sources in `/Programs/Source`); type their
name in the Terminal, or double-click a `.elf` file in File Explorer:

| Program | Shows |
|---|---|
| `hello [args]` | argv, getpid, printf |
| `primes [n]` | sieve on `calloc`'ed memory (heap grows with `sbrk`) |
| `guess` | reading lines from the Terminal (stdin) |
| `mandel [w] [h]` | fixed-point Mandelbrot set, CPU bound |
| `spin [s]` | pure ring 3 busy loop: the desktop stays responsive, Ctrl+C stops it |
| `wc <file>...` | reading files through system calls |
| `todo [add <text> / done <n> / clear]` | writing `/Documents/todo.txt` |
| `sysinfo` | `SYS_SYSINFO` / `SYS_TIME` |
| `crash null / kernel / div / cli / stack` | a faulting program is terminated with a report; nothing else is affected |

**How it works**
* `kernel/proc.c` loads a static **ELF64** executable (`PT_LOAD` segments)
  into a fresh PML4 whose user half starts at `0x80_0000_0000`; kernel pages
  are shared but supervisor-only. argc/argv go on a 256 KiB user stack.
* The process is a kernel task that enters ring 3 by `iretq` through a
  hand-built frame (user CS/SS selectors from the GDT). The scheduler loads
  each task's CR3 and points `TSS.RSP0` at its kernel stack.
* **System calls** use `int 0x80` (a DPL-3 gate): RAX = number, arguments in
  RDI, RSI, RDX, R10, R8; numbers in `user/include/syscalls.h`. Calls run with
  interrupts enabled and may sleep. User pointers are checked against the
  process's page tables before use.
* Console I/O goes through ring buffers polled by the Terminal. File system
  calls are proxied to the GUI task (the only task that touches NimbusFS)
  through a bounce buffer.
* CPU exceptions from ring 3 end only that process (`Program terminated:
  Page Fault at address ...`). A killed process ends on its next return to
  ring 3, even if it never makes a system call. Its address space is freed
  by the idle task.

**libc** (`user/lib`, written from scratch): `crt0`, syscall wrappers,
`printf` family, `puts/putchar/getchar/fgets`, `malloc/calloc/realloc/free`
(first fit with coalescing on `sbrk`), `string.h`, `ctype.h`, `strtol/atoi`,
`rand`, `qsort`, `exit`, plus `<nimbus.h>` for `readfile`, `writefile`,
`listdir`, `sleep_ms`, `ticks_ms`, `gettime`, `sysinfo`. Programs are built
with `-mcmodel=large` (they live above 4 GiB) and no SSE.

**Writing a program:** put `myprog.c` in `user/programs/` and run `make`. It
is built, linked with `user/linker.ld` and installed as `/Programs/myprog.elf`.

## Nimbus Update: signed updates from your own server

**Nimbus Update** (Start menu, or `update` in the Terminal) checks a static
HTTPS server for a newer release, shows its notes, downloads it, verifies
it and installs it: the kernel and the system files under `/Programs` and
`/System`. Your documents, pictures and settings are never touched.

**Why it is safe even though TLS certificates are not checked:**
* every release is signed with **Ed25519** (`net/crypto.c`, checked against
  the RFC 8032 test vectors). The public key in `keys/update.pub` is compiled
  into the kernel; a manifest whose signature doesn't verify is rejected;
* the signed manifest lists the SHA-256 of every file. Nothing is written
  until *all* downloads have been checked;
* the kernel has two **boot slots** (A at LBA 64, B at LBA 2080). The new
  kernel goes to the slot that is not booted, is read back and verified, and
  only then does a single MBR write switch slots (`kernel/bootarea.c`). If
  anything fails before that, the old kernel still boots.

Kernel updates need an installed system; on the live CD Nimbus Update refuses.

### Publishing a release (Vercel)

```sh
make keys                     # once: keys/update.key (SECRET) + keys/update.pub
# build and install NimbusOS from this tree, so it trusts keys/update.pub
make release VERSION=0.3 UPDATE_URL=https://<your-project>.vercel.app
npm i -g vercel               # once
cd build/update && vercel deploy --prod
```

`make release` builds the kernel and system files for that version, writes
`build/update/` (`manifest.txt`, `manifest.sig`, `files/<sha256>`,
`vercel.json`, `index.html`) and signs it with `keys/update.key`. Release
notes come from `release-notes.txt`. Systems built with that `UPDATE_URL`
find the server on their own; any other system can be pointed at it with
`update url https://<your-project>.vercel.app`.

> **Keep `keys/update.key` secret and backed up.** It is in `.gitignore`.
> Anyone who has it can publish updates your systems will accept, and if
> you lose it, systems that trust `keys/update.pub` can't be updated again
> (you'd have to reinstall them with a new key).

Test: `tools/test_update.py` builds a 0.2 system with a throwaway key,
publishes a signed 0.3 release over HTTPS on the host, checks that a
tampered manifest and a corrupted payload are rejected without installing
anything, installs the real release, reboots into 0.3 from slot B, and
checks the live CD refuses kernel updates.

## Beyond the stages: games, polish, sound, networking, HTTPS

**Games** (Start menu ▸ Games, and on the desktop): Minesweeper (three
difficulties, first click is always safe, chording), Snake (speeds up as it
grows, best score saved), Klondike Solitaire (drag cards or double-click to
send them home; draw 1 or 3).

**Settings** (Start menu ▸ Settings), stored in `/System/settings.cfg`:
* Display: 800×600 … 1920×1080, switched live through Bochs DISPI, with a
  15 s "keep these settings?" countdown that reverts automatically.
* Appearance: wallpaper, accent colour, smooth (anti-aliased, proportional) UI font.
* Mouse: pointer speed, double-click speed; the IntelliMouse **wheel** scrolls
  lists, text and the browser.
* Sound: master volume, mute, system sounds.  Network: adapter status, DHCP lease renewal.
* Date & Time: time zone offset, 12/24 h clock.

**Drag and drop:** drag files between Explorer windows and the desktop
(move; hold Ctrl to copy), or drop an audio file on the Media Player to play it.

**Sound:** an Intel AC'97 driver (`-device AC97`) with a DMA ring, a mixer
task that resamples any 8/16-bit mono/stereo WAV, synthesized system sounds
(startup, error, notify, shutdown) and a **Media Player**. The terminal has
`play <file>`, `beep [hz] [ms]` and `volume [0-100]`.
On macOS `make run` uses CoreAudio; elsewhere pass `AUDIO=pa` / `alsa` / `sdl`
(default `none` = silent but functional).

**Networking:** an Intel e1000 driver (QEMU's default NIC, `-nic user,model=e1000`)
and a TCP/IP stack written from scratch: Ethernet, ARP, IPv4, ICMP, UDP,
DHCP, DNS, TCP (client, with retransmission) and HTTP/1.0. Terminal commands:

| Command | |
|---|---|
| `ifconfig` | address, gateway, DNS, MAC, packet counters |
| `dhcp` | renew the lease |
| `ping <host>` | ICMP echo (QEMU's user network answers for 10.0.2.2) |
| `nslookup <name> [server]` | DNS A lookup |
| `wget <url> [file]` | HTTP or HTTPS download into the file system |
| `http <url>` | print a page |

The **Web Browser** (Start menu) is a text-mode HTML viewer: it renders
headings, paragraphs, lists, `<pre>` and links (click to follow, Back,
address bar). It opens `http://` and `https://` pages; Unicode text is
shown as its closest ASCII spelling (the UI font is ASCII).

**HTTPS:** a TLS 1.3 client written from scratch (`net/tls.c`), with its
own cryptography (`net/crypto.c`): SHA-256, HMAC, HKDF, X25519 key
exchange, and the AES-128-GCM and ChaCha20-Poly1305 ciphers. The HTTP
client speaks HTTP/1.1 (including chunked responses) and follows
redirects, so `http://` sites that move to `https://` work.

> **Certificates are not checked.** Connections are encrypted and
> tamper-proof, but NimbusOS does not verify the server's certificate, so
> it cannot prove which server it is talking to. The browser's status bar
> says so on every secure page. Don't use it for anything that matters.

Tests: `make test` runs `tools/cryptotest` (official test vectors from the
RFCs and NIST, 25 checks); `tools/test_https.py` boots NimbusOS against
OpenSSL's `s_server` on the host (one server per cipher suite) and checks
downloads, chunked responses, an http→https redirect and the browser.
`build/tools/tlstest <host> <port>` tries the TLS client from the host.

---

## Design notes and trade-offs

* **Bootloader:** a custom MBR + stage 2 rather than GRUB/multiboot, because
  the task asks for one. Unreal mode keeps stage 2 small while allowing a
  kernel larger than conventional memory.
* **Graphics:** Bochs VBE (DISPI) instead of BIOS VBE calls. It needs no
  real-mode switch at runtime and the mode can be set from C. The trade-off is
  that it only works on Bochs/QEMU/VirtualBox-style adapters (`-vga std`).
* **Identity-mapped kernel** instead of a higher-half kernel: simpler to boot.
  The VMM still provides a real virtual map: NULL guard, heap and framebuffer
  windows, per-process user address spaces.
* **GUI apps are kernel code:** the built-in apps are event handlers inside the
  "desktop" task, much like a classic single-threaded GUI toolkit. That keeps
  the window manager lock-free. Separately compiled programs run in ring 3
  (see "User mode"); they talk to the Terminal, not to windows.
* **Font:** taken from the VGA ROM at boot rather than embedded, so no
  third-party font data is in the repository.
* **Damage-based compositing:** apps that update a lot (Paint, Terminal)
  invalidate only the rectangles they change. This matters under QEMU's
  software emulation.

## Known limitations

* No USB. TCP is client-only. HTTPS does not verify certificates, supports only
  TLS 1.3 with X25519, and the browser shows no images, CSS or JavaScript.
* User programs are console programs: no GUI API, no fork/exec from user
  space, no FPU/SSE in user mode, whole-file I/O only.
* LBA28 ATA PIO only (disks up to 128 GiB); no AHCI.
* Single CPU; the file system is used only from the GUI task.
* Needs a Bochs-VBE-compatible display adapter (`-vga std`).

## References

* Intel® 64 and IA-32 Architectures Software Developer's Manual, Vol. 3A
  (protected mode, IA-32e/long mode, descriptors, interrupts, 4-level paging).
* OSDev wiki: *Rolling Your Own Bootloader*, *Unreal Mode*, *A20 Line*,
  *Detecting Memory (x86)*, *GDT*, *IDT*, *8259 PIC*, *PIT*, *PS/2 Keyboard*,
  *PS/2 Mouse*, *8042 PS/2 Controller*, *ATA PIO Mode*, *PCI*, *Bochs VBE
  Extensions*, *CMOS*, *Serial Ports*, *Shutdown*, *Reboot*, *Page Frame
  Allocation*, *GCC Cross-Compiler*.
* ACPI Specification 6.4: RSDP, FADT, `\_Sx` objects, PM1 control register.
* T13 ATA/ATAPI-6: IDENTIFY DEVICE, READ/WRITE SECTORS, FLUSH CACHE.
* Microsoft BMP file format (BITMAPINFOHEADER).
