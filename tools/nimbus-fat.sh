#!/bin/sh
# NimbusOS <-> FAT32 file exchange (macOS, also works on Linux).
#
#   sh nimbus-fat.sh export <nimbus.img> [name]
#       copies every file out of a NimbusOS disk image into a folder [name]
#       and makes a FAT32 disk image [name].dmg you can open and edit.
#
#   sh nimbus-fat.sh sync <name.dmg> <nimbus.img>
#       copies the files on the edited FAT32 image back into the NimbusOS
#       image, then ejects it. Quit NimbusOS first.
#
# nfstool (the NimbusFS tool) is compiled from the sources next to this
# script the first time, so you need a C compiler (Xcode Command Line Tools).
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
NFS="$HERE/nfstool"

build_nfstool() {
    [ -x "$NFS" ] && return 0
    [ -f "$HERE/tools/nfstool.c" ] || { echo "missing tools/nfstool.c next to this script" >&2; exit 1; }
    echo "Building nfstool (first time only) ..."
    cc -std=gnu11 -O2 -I"$HERE" "$HERE/tools/nfstool.c" "$HERE/fs/nbfs.c" -o "$NFS"
}

# Recursively copy NimbusFS path $1 into host folder $2.
# Each level runs in a subshell so its variables don't leak into the parent loop.
copy_tree() {
    src="$1"
    dst="$2"
    mkdir -p "$dst"
    # `nfstool ls` prints: "<d|-> <size> <date> <time>  <name>[/]"
    "$NFS" ls "$IMG" "$src" | sed -E 's/^([d-]) +[0-9]+ +[0-9-]+ +[0-9:]+ +//' | while IFS= read -r entry; do
        case "$src" in
            /) path="/$entry" ;;
            *) path="$src/$entry" ;;
        esac
        case "$entry" in
            */)
                name="${entry%/}"
                (copy_tree "${path%/}" "$dst/$name")
                ;;
            *)
                echo "  $path"
                "$NFS" get "$IMG" "$path" "$dst/$entry"
                ;;
        esac
    done
}

cmd_export() {
    IMG="${1:?usage: $0 export <nimbus.img> [name]}"
    OUT="${2:-nimbus-export}"
    [ -f "$IMG" ] || { echo "no such image file: $IMG" >&2; exit 1; }
    build_nfstool
    rm -rf "$OUT"
    echo "Copying files from $IMG to $OUT/ ..."
    copy_tree / "$OUT"
    echo "Making FAT32 disk image $OUT.dmg ..."
    rm -f "$OUT.dmg"
    # FAT32 needs a volume of at least ~33 MB, so make a fixed 64 MB image
    # and copy the files into it (a folder that small can't be used directly).
    # Newer macOS versions prefer `diskutil image create`; fall back to hdiutil.
    if ! diskutil image create blank --fs MS-DOS --volumeName NIMBUS --size 64M --format UDRW "$OUT.dmg" >/dev/null 2>&1; then
        rm -f "$OUT.dmg"
        hdiutil create -size 64m -fs MS-DOS -volname NIMBUS -format UDRW "$OUT.dmg" >/dev/null
    fi
    MNT="$(mktemp -d "${TMPDIR:-/tmp}/nimbus-fat.XXXXXX")"
    trap 'hdiutil detach "$MNT" -quiet >/dev/null 2>&1 || true; rmdir "$MNT" 2>/dev/null || true' EXIT INT TERM
    hdiutil attach "$OUT.dmg" -nobrowse -mountpoint "$MNT" >/dev/null
    cp -R "$OUT"/. "$MNT"/
    hdiutil detach "$MNT" -quiet
    echo "Done: folder $OUT/ and FAT32 image $OUT.dmg"
}

cmd_sync() {
    DMG="${1:?usage: $0 sync <name.dmg> <nimbus.img>}"
    IMG="${2:?usage: $0 sync <name.dmg> <nimbus.img>}"
    for f in "$DMG" "$IMG"; do
        [ -f "$f" ] || { echo "no such file: $f" >&2; exit 1; }
    done
    build_nfstool
    MNT="$(mktemp -d "${TMPDIR:-/tmp}/nimbus-fat.XXXXXX")"
    trap 'hdiutil detach "$MNT" -quiet >/dev/null 2>&1 || true; rmdir "$MNT" 2>/dev/null || true' EXIT INT TERM
    # Mount privately (no Finder window) at a temporary mount point.
    hdiutil attach "$DMG" -nobrowse -mountpoint "$MNT" >/dev/null
    echo "Copying files from $DMG into $IMG ..."
    "$NFS" import "$IMG" "$MNT" /
    "$NFS" check "$IMG"
    echo "Ejecting $DMG ..."
    hdiutil detach "$MNT" -quiet
    echo "Done. Boot NimbusOS and look in the folders you added files to."
}

case "${1:-}" in
    export) shift; cmd_export "$@" ;;
    sync)   shift; cmd_sync "$@" ;;
    *) echo "usage: $0 export <nimbus.img> [name]  |  $0 sync <name.dmg> <nimbus.img>" >&2; exit 1 ;;
esac
