# NimbusOS disk tools

Host-side tools for [NimbusOS](https://github.com/enderbrosik/nimbusOS) disk images (`.img` files).
They only open image files, never real disks.

| Tool | What it does |
|---|---|
| `nfstool` | `mkfs`, `ls`, `tree`, `cat`, `put`, `get`, `mkdir`, `rm`, `mv`, `import`, `export`, `check`, `df` on a NimbusFS image |
| `nimbusfuse` | mounts an image as a folder (libfuse 2 on Linux, macFUSE on macOS, WinFsp on Windows) |

```sh
make                                   # build/nfstool and build/nimbusfuse
build/nfstool ls nimbus.img /
build/nfstool export nimbus.img /Documents ./Documents
build/nimbusfuse nimbus.img /Volumes/nimbus     # unmount before booting the image
make windows                           # build/nfstool.exe (needs MinGW)
```

Status: `nfstool` and `nimbusfuse` are tested on Linux (`make test`). The Windows build compiles but has not been run,
and macFUSE / WinFsp mounting has not been tried. NimbusFS has no support for two writers: unmount before you boot the image.
