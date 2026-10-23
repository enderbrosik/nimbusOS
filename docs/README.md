# NimbusOS Documentation

NimbusOS is a freestanding x86-64 hobby operating system. This directory is
the project guide: start with the build guide, then use the subsystem pages as
a map from behavior to source.

## Start Here

- [Build and run](development/getting-started.md): prerequisites, compiler
  selection, build targets, QEMU, and noVNC.
- [Project architecture](architecture/overview.md): source ownership and the
  path from BIOS boot to desktop.
- [Development and tests](development/testing.md): focused tests and the full
  automated suite.

## Architecture

- [Boot and disk images](architecture/boot-and-disk.md): MBR, stage 2, disk
  layout, RAM-disk live ISO, and installed disks.
- [Kernel, tasks, and system calls](architecture/kernel-and-syscalls.md):
  address spaces, scheduling, user processes, and the syscall ABI.
- [Filesystems](architecture/filesystems.md): NimbusFS, VFS, FAT32, and host
  image tools.
- [Hardware and drivers](architecture/hardware.md): supported devices and
  driver boundaries.
- [Networking and HTTPS](architecture/networking.md): E1000, the network
  stack, browser, TLS, and security limits.
- [Desktop and built-in apps](desktop/README.md): window manager, desktop,
  file associations, and shell.

## Development

- [User programs and `.npkg` apps](development/user-programs.md): compile a
  native application, package it, move it into NimbusOS, and install it.
- [Build and run](development/getting-started.md)
- [Testing](development/testing.md)

## Operations

- [OS installer and signed updates](operations/installer-and-updates.md)
- [Docker and noVNC](operations/docker-novnc.md)

## Compatibility

The kernel runs NimbusOS's static, little-endian x86-64 ELF64 executables.
The `.npkg` format is an app container around that executable; it is not a
Windows PE `.exe`, macOS `.app`, Linux distribution package, or virtual
machine image. The OS installer installs NimbusOS to a disk; `install-app`
installs one Nimbus app package into `/Programs`.