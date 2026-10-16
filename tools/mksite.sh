#!/bin/sh
# Assemble the NimbusOS website: the download page (web/) plus the signed
# update files from `make release`.
#   tools/mksite.sh <version> <iso-url> <iso-size-MB> <update-dir> <out-dir>
set -eu
VERSION=$1 ISO_URL=$2 ISO_MB=$3 UPD=$4 OUT=$5
HERE="$(cd "$(dirname "$0")/.." && pwd)"
rm -rf "$OUT"
mkdir -p "$OUT"
cp -R "$UPD"/. "$OUT"/
rm -rf "$OUT/.vercel"
cp "$HERE/web/icon.svg" "$OUT/icon.svg"
NOTES=$(grep -v '^#' "$HERE/release-notes.txt" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/^/<li>/; s/$/<\/li>/' | tr -d '\n')
NOTES=$(printf '%s' "$NOTES" | sed 's/[&|\\]/\\&/g')       # escape for the sed replacement below
DATE=$(date -u +%Y-%m-%d)
sed -e "s|{{VERSION}}|$VERSION|g" -e "s|{{ISO_URL}}|$ISO_URL|g" -e "s|{{ISO_MB}}|$ISO_MB|g" \
    -e "s|{{DATE}}|$DATE|g" -e "s|{{NOTES}}|$NOTES|g" "$HERE/web/index.html" > "$OUT/index.html"
echo "mksite: $OUT ready (NimbusOS $VERSION)"
