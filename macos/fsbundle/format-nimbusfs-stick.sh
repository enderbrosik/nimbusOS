#!/bin/bash
# Formats a USB stick (or any EXTERNAL disk) as NimbusFS on macOS:
#   GPT partition map with the NimbusFS GUID -> newfs_nimbusfs_fmt -> mount via the FSKit extension.
# It ERASES the whole disk you pick. Run:  bash macos/fsbundle/format-nimbusfs-stick.sh [label]
set -e
GUID=876437C4-C160-4036-ACF0-B48784DF0284
LABEL=${1:-NimbusFS}
NEWFS=/Library/Filesystems/nimbusfs_fmt.fs/Contents/Resources/newfs_nimbusfs_fmt
PB=/usr/libexec/PlistBuddy

[ "$(uname)" = Darwin ] || { echo "macOS only"; exit 1; }
[ -x "$NEWFS" ] || { echo "install the bundle first: sh macos/fsbundle/install.sh"; exit 1; }

echo "External physical disks:"
diskutil list external physical
printf "\nType the identifier of the disk to ERASE (for example disk4), or nothing to cancel: "
read -r ID
[ -n "$ID" ] || { echo "cancelled"; exit 0; }
case "$ID" in disk[0-9]*) ;; *) echo "not a disk identifier"; exit 1;; esac
case "$ID" in *s[0-9]*) echo "give the whole disk (diskN), not a partition"; exit 1;; esac

INFO=$(mktemp)
diskutil info -plist "$ID" > "$INFO" || { echo "no such disk"; rm -f "$INFO"; exit 1; }
INTERNAL=$($PB -c "Print :Internal" "$INFO"); WHOLE=$($PB -c "Print :WholeDisk" "$INFO")
SIZE=$($PB -c "Print :TotalSize" "$INFO"); NAME=$($PB -c "Print :MediaName" "$INFO")
VIRTUAL=$($PB -c "Print :VirtualOrPhysical" "$INFO" 2>/dev/null || echo Physical)
rm -f "$INFO"
[ "$INTERNAL" = false ] || { echo "refusing: $ID is an internal disk"; exit 1; }
[ "$WHOLE" = true ] || { echo "refusing: $ID is not a whole disk"; exit 1; }

echo
echo "About to ERASE $ID: \"$NAME\", $((SIZE / 1000000)) MB ($VIRTUAL). Everything on it will be lost."
printf "Type the identifier again to confirm (%s): " "$ID"
read -r AGAIN
[ "$AGAIN" = "$ID" ] || { echo "did not match, cancelled"; exit 1; }

# NimbusFS tops out at 4000 MiB, so the partition is at most 3900 MB; the rest stays free.
PSIZE=3900m
[ "$SIZE" -lt 3900000000 ] && PSIZE=R
if [ "$PSIZE" = R ]; then
  diskutil partitionDisk "/dev/$ID" GPT "%$GUID%" "$LABEL" R
else
  diskutil partitionDisk "/dev/$ID" GPT "%$GUID%" "$LABEL" "$PSIZE" "Free Space" Free R
fi
# find OUR partition by its type GUID (macOS may add other partitions, e.g. EFI or "Boot OS X", so never go by position)
PART=$(diskutil list "$ID" | awk -v g="$GUID" 'index($0, g) { print $NF }')
case "$PART" in disk[0-9]*s[0-9]*) ;; *) echo "could not find the NimbusFS partition on $ID"; exit 1;; esac
[ "$(echo "$PART" | wc -l | tr -d ' ')" = 1 ] || { echo "found several NimbusFS partitions ($PART), refusing"; exit 1; }
echo "formatting /dev/$PART"
diskutil unmount "/dev/$PART" 2>/dev/null || true
sudo "$NEWFS" -V "$LABEL" "/dev/r$PART"
diskutil mount "/dev/$PART" || echo "formatted, but it did not mount; try: sudo mount -F -t nimbusfs /dev/$PART <folder>"
