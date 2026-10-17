# NimbusFS for macOS (FSKit file system extension)

Lets Finder and every Mac app open NimbusOS disk images, with no macFUSE. It uses Apple's FSKit (a user-space
file system extension, no kernel extension) and the same `fs/nbfs.c` that NimbusOS itself uses.

**Status: written without Xcode and never compiled.** The C layer (`NBFSShim`) builds and was checked on Linux.
The Swift files follow the FSKit API as I know it up to macOS 26. You have macOS 27, so expect to fix a few
signatures on the first build (Xcode's fix-it suggestions show the right ones). Send the errors if you get stuck.

## Build it (Xcode, about 10 minutes)

1. Install Xcode. Open it and choose **File ▸ New ▸ Project ▸ macOS ▸ App** (call it `NimbusFS`).
2. **File ▸ New ▸ Target ▸ macOS ▸ File System Extension** (call it `NimbusFSExtension`). If the template is not
   in the list, your Xcode is too old for FSKit.
3. Delete the template's generated Swift files in the extension target and drag these in (target: the extension):
   `NimbusFSExtensionMain.swift`, `NimbusFileSystem.swift`, `NimbusVolume.swift`, `NBFSShim.c`, `NBFSShim.h`, `NimbusFSExtension-Bridging-Header.h` and `../fs/nbfs.c`.
4. In the extension target's **Build Settings**: *Objective-C Bridging Header* = the full path of `NimbusFSExtension-Bridging-Header.h`;
   *Header Search Paths* = the repo root (so `../../fs/nbfs.h` resolves).
5. In the extension's `Info.plist`, under `EXAppExtensionAttributes`, set `FSShortName` = `nimbusfs`, `FSPersonalities`
   with `FSName` = `NimbusFS`, `FSMediaTypes` for a Basic Data / Linux partition type, and `FSSupportsBlockResources` = YES
   (the Xcode template already contains these keys; fill in the values).
6. Pick your Apple ID as the signing team (a free personal team is fine for local use) and Run.
7. Enable it in **System Settings ▸ General ▸ Login Items & Extensions ▸ File System Extensions ▸ NimbusFS**.

## Use it

Attach an image as a block device (this only attaches the image file, it never touches a real disk), then mount:

```sh
dev=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage /path/to/nimbus.img | awk 'NR==1{print $1}')
mkdir -p ~/nimbus-mount
mount -F -t nimbusfs $dev ~/nimbus-mount
# ... use it in Finder ...
umount ~/nimbus-mount && hdiutil detach $dev
```

NimbusOS images start with an MBR and the NimbusFS partition is the first partition, so mount the **partition**
device (`/dev/diskNs1`), not the whole disk, for `nimbus.img`.

## Not supported

Symbolic links, hard links, permissions and owners (NimbusFS does not store them), and two writers at once.
Unmount before you boot the image in QEMU.

## Ready-made Xcode project

`macos/xcode/NimbusFS.xcodeproj` is the working project (app + `NimbusFSExtension`).
Open it, pick your own Team under Signing & Capabilities for both targets, then run.

Mounting, as tested on macOS 27 (M4):

1. Run the app once from Xcode, then enable **NimbusFSExtension** in System Settings >
   General > Login Items & Extensions > File System Extensions
   (or `pluginkit -e use -i com.aaron.garsia.NimbusFS.NimbusFSExtension`).
2. **Reboot** the first time. Until then `mount` may say
   `No extension with fsShortName ... found` even though `pluginkit` lists it.
   After a later rebuild, if that message comes back, reboot again.
3. The extension's Info.plist needs `FSShortName` = `nimbusfs` and
   `FSSupportsBlockResources` = `true`.
4. Attach and mount (keep the image somewhere permanent; `/tmp` is cleared on reboot):

       diskutil image attach --noMount ~/nimbus-test.img     # prints /dev/diskN, /dev/diskNs1 0x4E
       mkdir -p ~/nimbus-mount
       mount -F -t nimbusfs /dev/diskNs1 ~/nimbus-mount
       umount ~/nimbus-mount && hdiutil detach /dev/diskN

## Icons

`macos/icon/NimbusFS.icns` / `NimbusFS-icon.png` are the NimbusOS logo (cloud, N and sun on hills). Use the PNG for the
app's own icon (drag it into AppIcon in Assets.xcassets).

Tried and **not working** on macOS 27 / FSKit, so do not retrace these:

- Custom icon for the mounted disk (`/.VolumeIcon.icns` plus a virtual `com.apple.FinderInfo` attribute on the root):
  FSKit never served the attribute, Finder kept the generic drive icon.
- Own file type / icon for `*.nimg` images (exported UTI conforming to `public.disk-image` or `public.data`):
  LaunchServices lists the type as registered, yet `UTType(filenameExtension: "nimg")` stayed `dyn.`.
  Keep using plain `.img` images; double-click mounting works for those.
