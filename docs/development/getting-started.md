# Build and Run

## Prerequisites

On Debian or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential clang lld nasm qemu-system-x86 python3 python3-pil novnc
```

The kernel compiler is selected automatically. If `x86_64-elf-gcc` is on
`PATH`, Make uses it; otherwise it uses Clang targeting `x86_64-elf` and
`ld.lld`. A host C compiler, GNU Make, and NASM are required. Python Pillow is
used by screenshot and image tests.

## Build

```sh
make build        # build/nimbus.img, a bootable 64 MiB image
make iso          # build/nimbus.iso, a live CD image
make test         # host-side filesystem, crypto, and arithmetic tests
```

Select another compiler with `make TOOLCHAIN=gcc`, or override `KCC` and
`KLD` directly. Build outputs live under `build/`; `make clean` removes them.

## Run in QEMU

```sh
make run          # boot build/nimbus.img; serial log is in the terminal
make run-iso      # boot build/nimbus.iso as a CD
make installer-test
```

`make installer-test` attaches `build/target.img` as a second, blank 64 MiB
disk. The graphical installer erases that target only after its confirmation
step. After installation, use `make run-target` to boot the installed disk.

Pass additional emulator arguments with `QEMU_EXTRA`, for example:

```sh
make run QEMU_EXTRA='-display sdl'
```

For a browser console, see [Docker and noVNC](../operations/docker-novnc.md).