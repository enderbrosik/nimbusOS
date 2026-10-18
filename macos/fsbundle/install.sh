#!/bin/sh
# Installs the NimbusFS format entry for Disk Utility. Run from the repo root after `make`:
#   sh macos/fsbundle/install.sh
# Only /Library/Filesystems/nimbusfs_fmt.fs is touched. Remove it with: sh macos/fsbundle/uninstall.sh
set -e
[ -x build/newfs_nimbusfs_fmt ] || { echo "run make first (build/newfs_nimbusfs_fmt is missing)"; exit 1; }
D=/Library/Filesystems/nimbusfs_fmt.fs
sudo rm -rf "$D"
sudo mkdir -p /Library/Filesystems
sudo cp -R macos/fsbundle/nimbusfs_fmt.fs "$D"
sudo cp build/newfs_nimbusfs_fmt "$D/Contents/Resources/newfs_nimbusfs_fmt"
sudo chown -R root:wheel "$D"
sudo chmod -R go-w "$D"
echo "installed $D - reboot (or relaunch Disk Utility) and look for NimbusFS in the Erase > Format list"
