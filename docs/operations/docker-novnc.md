# Docker and noVNC

The simplest setup is to install the QEMU and noVNC packages on the host and
run `make run-novnc`. This target builds the live ISO, creates
`build/target.img` if needed, attaches it as a writable second disk, and
serves the QEMU display through a local websockify/noVNC process.

## Host setup

On Debian or Ubuntu:

```sh
sudo apt update
sudo apt install build-essential clang lld nasm qemu-system-x86 python3 novnc
```

Then from the repository root:

```sh
make run-novnc
```

Open `http://localhost:6080/`. Click the guest display to capture the mouse;
press `Esc` or use **Release mouse** to unlock it. Stop the foreground command
with `Ctrl+C`. Use `NOVNC_PORT=6081` to choose a different browser port or
`VNC_DISPLAY=1` to choose a different local VNC display. `NOVNC_WEB` can point
to the noVNC web asset directory if the distribution installs it elsewhere.
The target serves its own pointer-lock page and links the installed noVNC
`core/` and `vendor/` modules into a build-only web root.

The guest boots from the live ISO, whose changes are temporary. The attached
`build/target.img` is a persistent blank disk image used for the installer;
it is not the live system's boot disk.

## Docker outline

Docker is useful when the host lacks NASM, QEMU, or noVNC. Build and run in a
Linux container that has the repository mounted and the required packages
installed. A container needs access to `/dev/kvm` for hardware acceleration;
without it, QEMU may run much more slowly or fail depending on its options.

An interactive container can be started from the repository root with:

```sh
docker run --rm -it \
  -p 6080:6080 \
  -v "$PWD:/work" -w /work \
  ubuntu:24.04
```

Inside the container, install the packages above, then run `make run-novnc`.
For example, as root inside the container:

```sh
apt-get update
apt-get install -y build-essential clang lld nasm qemu-system-x86 python3 novnc
make run-novnc
```

For acceleration, pass `/dev/kvm` into the container with
`--device=/dev/kvm`; this is optional. When the container exits, the VM and
noVNC proxy stop, but the mounted `build/` images remain in the repository
workspace.

Docker installation commands require appropriate host Docker permissions.
Do not mount host block devices into the container for this workflow. The
Make targets operate on project image files.

## Troubleshooting

- `nasm: command not found`: install NASM in the environment running Make.
- `qemu-system-x86_64: command not found`: install QEMU's x86 system package.
- `websockify not found`: install noVNC/websockify or set up the documented
  proxy in that environment.
- noVNC assets not found: set `NOVNC_WEB` to the directory containing
  `core/` and `vendor/`.
- Port already in use: change `NOVNC_PORT`.
- Blank or slow display in Docker: check QEMU logs and consider passing
  `/dev/kvm` if the host supports KVM.