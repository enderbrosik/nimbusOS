# Testing

## Test targets

```sh
make test       # host filesystem, crypto, and calculator tests
make test-all   # host tests plus the QEMU stage and feature tests
```

`make test-all` requires the cross compiler, NASM, QEMU, and Python. It runs
tests for boot, kernel memory/tasking, drivers, filesystem, desktop, apps,
installer, user mode, network, web features, images, and update verification.
Individual scenarios can be run directly, for example:

```sh
python3 tools/test_stage4.py
python3 tools/test_user.py
python3 tools/test_stage7.py
```

The Python harness in `tools/qemu_harness.py` starts QEMU, sends keyboard and
mouse input, captures serial output, and can save screenshots. The scripts
assert behavior from the guest and inspect image contents with `nfstool`.

## User-program package coverage

`tools/test_user.py` exercises existing user-mode behavior and the `.npkg`
installation path: it creates a package from an ELF app, inserts the package
into a test image, boots NimbusOS, installs the app, executes it, and verifies
its arguments and exit status.

## Failure triage

- Check the first failing command and its prerequisites before interpreting
  downstream test failures.
- QEMU tests require `qemu-system-x86_64` on `PATH`.
- Assembly targets require NASM; kernel links require the selected x86-64
  cross compiler/linker pair.
- Image and QEMU tests write under `build/`, not to a physical disk.