# NimbusFS for macOS

Open, read, write and **erase** NimbusOS disks (images and USB sticks) on a Mac, with no macFUSE and no kernel
extension. Two small pieces, both user space, sharing `fs/nbfs.c` with NimbusOS itself:

| Piece | What it does |
|---|---|
| **FSKit extension** (`macos/xcode`, Swift) | Mounts NimbusFS volumes: read, write, check. Finder, double-click, `mount`. |
| **Format bundle** (`macos/fsbundle`, C) | Puts NimbusFS in Disk Utility's *Erase > Format* list and formats with `newfs_nimbusfs_fmt`. |

Tested on macOS 27.0, Apple M4 Mac mini. Not supported: symbolic links, hard links, permissions/owners (NimbusFS does
not store them), two writers at once. Unmount before you boot an image in QEMU. NimbusFS is limited to 4000 MiB.

## 1. Build and enable the extension

`macos/xcode/NimbusFS.xcodeproj` is the working project (app + `NimbusFSExtension`).

1. Open it. Under **Signing & Capabilities** pick your own Team for both targets (a free personal team is fine).
2. The extension's `Info.plist` (folder `NimbusFSExtension`) must look like this, in particular `FSShortName`,
   `FSSupportsBlockResources` and the two `FSMediaTypes` entries:

```xml
<key>FSMediaTypes</key>
<dict>
    <key>0x4E</key>   <!-- MBR partition type 0x4E: images made by nfstool mkdisk / the NimbusOS installer -->
    <dict><key>FSMediaProperties</key><dict>
        <key>Content Hint</key><string>0x4E</string><key>Leaf</key><true/></dict></dict>
    <key>876437C4-C160-4036-ACF0-B48784DF0284</key>   <!-- GPT type GUID of NimbusFS: sticks erased on the Mac -->
    <dict><key>FSMediaProperties</key><dict>
        <key>Content Hint</key><string>876437C4-C160-4036-ACF0-B48784DF0284</string><key>Leaf</key><true/></dict></dict>
</dict>
<key>FSPersonalities</key>
<dict><key>NimbusFSExtensionPersonality</key>
    <dict><key>FSName</key><string>NimbusFS</string>
          <key>FSfileObjectsAreCaseSensitive</key><false/></dict></dict>
<key>FSShortName</key><string>nimbusfs</string>
<key>FSSupportsBlockResources</key><true/>
```

3. Press Run (⌘R), then install and enable it:

```sh
rm -rf /Applications/NimbusFS.app
cp -R ~/Library/Developer/Xcode/DerivedData/NimbusFS-*/Build/Products/Debug/NimbusFS.app /Applications/
open /Applications/NimbusFS.app
pluginkit -e use -i com.aaron.garsia.NimbusFS.NimbusFSExtension      # a "+" in front of it in: pluginkit -m -p com.apple.fskit.fsmodule
```

4. **Reboot.** FSKit caches its extension list. Until you reboot, `mount` may say
   `No extension with fsShortName ... found` even though `pluginkit` lists it. Reboot again after later rebuilds
   if that message comes back.

## 2. Mount images and sticks

Double-click an image, or plug the stick in: it mounts in Finder under its label. The volume name is the label stored
in the volume (`nbfs_shim_label`), e.g. `MYSTICK`; unlabelled volumes show as `NimbusFS`.

By hand (raw devices are root-only, so a **stick** needs `sudo`; an image does not):

```sh
diskutil image attach --noMount ~/nimbus-test.img        # prints /dev/diskN and /dev/diskNs1
mkdir -p ~/nimbus-mount
mount -F -t nimbusfs /dev/diskNs1 ~/nimbus-mount          # sudo mount ... for a USB stick
umount ~/nimbus-mount && hdiutil detach /dev/diskN
```

Keep images somewhere permanent: `/tmp` is cleared on reboot.

## 3. Erase a disk as NimbusFS (Disk Utility)

Disk Utility builds its Format list from bundles in `/Library/Filesystems` (like Paragon's `ufsd_ExtFS.fs`), not from the
FSKit extension. `macos/fsbundle/nimbusfs_fmt.fs` is a format-only bundle: it declares the NimbusFS personality with
its own GPT type GUID and ships `newfs_nimbusfs_fmt` (from `tools/newfs_nimbusfs.c`), which Disk Utility runs on the
partition it creates. Mounting stays with the FSKit extension. The bundle is called `nimbusfs_fmt` on purpose, so the
legacy `mount -t nimbusfs` does not pick it instead of the extension.

```sh
make                                  # builds build/nfstool and build/newfs_nimbusfs_fmt
sh macos/fsbundle/install.sh          # copies the bundle to /Library/Filesystems (sudo)
sh macos/fsbundle/uninstall.sh        # removes it again
```

Then quit and reopen Disk Utility, **View > Show All Devices**, select the *whole* stick, **Erase**, Format =
**NimbusFS**, Scheme = **GUID Partition Map**. Re-run `make` and `install.sh` after pulling a new version.
Check with `diskutil listFilesystems | grep -i nimbus`.

Same thing from Terminal, with confirmations (it ERASES the disk you pick, and finds our partition by its GUID):

```sh
bash macos/fsbundle/format-nimbusfs-stick.sh MYSTICK
```

Or by hand: `diskutil partitionDisk /dev/diskN GPT "%876437C4-C160-4036-ACF0-B48784DF0284%" MYSTICK R`, then
`sudo /Library/Filesystems/nimbusfs_fmt.fs/Contents/Resources/newfs_nimbusfs_fmt -V MYSTICK /dev/rdiskNsM`
for the partition that carries that GUID (never go by position: macOS may add EFI or "Boot OS X" partitions).

## 4. Make a stick with the host tools instead (any OS)

`nfstool mkdisk` builds an image with an MBR and a type `0x4E` partition (the layout NimbusOS images use). Write it to
the stick with `dd`. This **erases the stick**; `of=` is the stick, `if=` the image.

```sh
./build/nfstool mkdisk ~/stick.img 900 MYSTICK
diskutil list external                                # the "(external, physical)" disk. The number changes at every plug-in!
diskutil unmountDisk /dev/diskN
sudo dd if=$HOME/stick.img of=/dev/rdiskN bs=4m
```

If macOS says "The disk you attached was not readable", click **Ignore**, never Initialize. A bare `nfstool mkfs`
image (no partition table) is not offered to the extension; mount it by hand with `sudo mount -F -t nimbusfs /dev/diskN`.

To check a stick from the host side, copy the **whole** partition (a truncated copy fails `nfstool` on purpose: the volume
records its size) and make the copy yours: `sudo dd if=/dev/rdiskNsM of=$HOME/check.img bs=4m`,
`sudo chown $(whoami) $HOME/check.img`, `./build/nfstool check $HOME/check.img`.

## Icons

`macos/icon/NimbusFS.icns` / `NimbusFS-icon.png` are the NimbusOS logo (cloud, N and sun on hills). Use the PNG for the
app's own icon (drag it into AppIcon in Assets.xcassets).

Tried and **not working** on macOS 27 / FSKit, so do not retrace these:

- Custom icon for the mounted disk (`/.VolumeIcon.icns` plus a virtual `com.apple.FinderInfo` attribute on the root):
  FSKit never served the attribute, Finder kept the generic drive icon.
- Own file type / icon for `*.nimg` images (exported UTI conforming to `public.disk-image` or `public.data`):
  LaunchServices lists the type as registered, yet `UTType(filenameExtension: "nimg")` stayed `dyn.`.
  Keep using plain `.img` images; double-click mounting works for those.
- Passing a partition type through the extension's own `FSPersonalities` (`FSFormatContentMask`): not picked up. The
  format entry lives in the `/Library/Filesystems` bundle.
- A kernel extension: not needed, and not realistic on Apple silicon.
