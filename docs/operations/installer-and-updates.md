# OS Installer and Signed Updates

## Graphical OS installer

The **Install NimbusOS** wizard runs from the live system. It lists ATA
drives, excludes the live boot disk, rejects targets smaller than 16 MiB, and
shows a destructive-action confirmation before writing. The confirmation
checkbox is required to continue.

Installation copies the boot area and kernel, formats a NimbusFS partition,
copies system files, writes the installed marker, checks the filesystem, and
writes the MBR last. The MBR-last design avoids making a half-written disk
bootable. The target disk is erased; choose it carefully.

For a disposable QEMU target:

```sh
make installer-test
# In NimbusOS, launch Install NimbusOS and select the second disk.
make run-target
```

`build/target.img` is a regular image file. Do not replace it with a physical
device unless you deliberately intend to erase that device.

## Nimbus Update

Nimbus Update fetches release metadata and files over HTTPS, verifies the
manifest signature and each file hash, and installs eligible system files.
Release signatures use Ed25519 with the public key from `keys/update.pub`
compiled into the kernel. Kernel updates use two boot slots and change the
active slot only after the new kernel has been written and verified.

To build and publish a release, use the `make release` and `make deploy`
targets documented in the root README. Keep `keys/update.key` private and
backed up. Anyone who obtains that private key can sign updates accepted by
systems that trust its public key; losing the key prevents updates under that
trust root.

Nimbus Update applies to system files and does not replace the graphical
whole-OS installer. App packages are currently installed individually with
`install-app`; they are not part of the update channel.