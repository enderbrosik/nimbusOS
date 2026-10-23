# Filesystems

## NimbusFS

NimbusFS is an ext2-inspired filesystem implemented in `fs/nbfs.c` and shared
between the kernel and host tools. It uses 1 KiB blocks, bitmaps, 128-byte
inodes, 64-byte directory entries, 24 direct pointers, and single- and
double-indirect blocks. A 64-entry write-through block cache sits above its
block device interface. The implementation includes allocation, path lookup,
directories, read/write/truncate, rename, deletion, and a consistency checker.

The on-disk filesystem is partition type `0x4E`, after the boot area. Use
`build/tools/nfstool` to inspect an image without booting it:

```sh
build/tools/nfstool tree build/nimbus.img /
build/tools/nfstool cat build/nimbus.img /readme.txt
build/tools/nfstool check build/nimbus.img
```

The host tool refuses block and character devices; it works on regular image
files. See `build/tools/nfstool` usage output for `put`, `get`, `import`,
`mkdir`, `mv`, and other supported operations.

## VFS and FAT32

`fs/vfs.c` is the kernel-facing path API over NimbusFS. At boot, it identifies
the boot disk from its MBR signature and mounts the NimbusFS partition.
User-program file operations pass through the syscall proxy described in
[kernel and syscalls](kernel-and-syscalls.md).

FAT32 is a separate compatibility path for removable/secondary FAT32
volumes. The Terminal's `fat` commands expose listing, reading, writing, and
directory operations. FAT32 is not the boot/system filesystem.

## Images and tests

`make build` makes the bootable disk image. `make iso` makes a live CD image.
`make fsimage` creates a standalone empty NimbusFS image. The filesystem test
suite is `make test`; it exercises image persistence, directory operations,
large files, allocation failure, remounting, and consistency checks.