# User Programs and `.npkg` Apps

NimbusOS runs native static x86-64 ELF64 applications in ring 3. Applications
use the project C runtime and syscall interface; this environment is small
and is not a POSIX or Linux ABI.

## Write and build an app

Add a C file in `user/programs/`, include headers from `user/include/`, and
use the APIs in `user/include/nimbus.h` and `user/include/stdio.h`:

```c
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("Hello from NimbusOS\n");
    return 0;
}
```

Build the project with `make`. The program is emitted as
`build/userroot/Programs/<name>.elf`, and the project image includes it under
`/Programs`. To run it in the guest, type its name in the Terminal or
double-click its `.elf` file.

The linker script is `user/linker.ld`; startup code is `user/lib/crt0.asm`.
User programs are static `ET_EXEC` ELF64 binaries for little-endian
`EM_X86_64`, linked at the Nimbus user address. The user runtime does not
provide dynamic linking, shared libraries, FPU/SSE, POSIX file descriptors,
or a stable third-party ABI guarantee.

## Package an app

The host-side packer wraps a built ELF in a Nimbus package:

```sh
make package APP=hello
# build/packages/hello.npkg
```

For an ELF built outside this repository, use the packer directly:

```sh
build/tools/mknpkg /path/to/my-app.elf build/packages/my-app.npkg my-app
```

The input must already be a valid static NimbusOS ELF64 program. `mknpkg`
packages an executable; it does not cross-compile arbitrary source code.

To install it into an image without booting, use the image tool:

```sh
build/tools/nfstool put build/nimbus.img build/packages/hello.npkg /Documents/hello.npkg
```

Alternatively, put the file on a FAT32 volume and copy it into NimbusOS with
the Terminal's `fat get` command. In the guest, install and launch it:

```text
install-app /Documents/hello.npkg
hello
```

`install-app` validates the package header and ELF architecture, installs to
`/Programs/<name>.elf`, and refuses to overwrite an existing program. The
installer handles one application file; it does not execute package scripts
or install host dependencies. The existing graphical **Install NimbusOS**
wizard is a separate whole-operating-system disk installer.

## Package format

Version 1 (`NPKG_MAGIC`, defined in `npkg.h`) is a simple little-endian
container with a fixed 24-byte header, followed by the app name bytes, then
the ELF payload:

| Header field | Encoding | Meaning |
|---|---|---|
| Magic | 8 bytes | `NIMPKG1` followed by NUL |
| Version | `u32` | Currently 1 |
| Name length | `u32` | 1-48 ASCII letters, digits, `-`, or `_` |
| ELF length | `u64` | Exact payload length in bytes |

The package is not signed. Install software only from sources you trust.
The format intentionally contains no arbitrary install paths or scripts.

## Compatibility

`.npkg` is the NimbusOS application package extension. It carries a NimbusOS
ELF64 executable, rather than translating or emulating another platform's
binary format. Windows `.exe` files are usually PE binaries for Windows APIs;
macOS `.app` is a directory bundle for macOS. Neither can run on NimbusOS.
The package may be created on a little-endian Linux host, but the program
inside must be compiled for NimbusOS's x86-64 user ABI.