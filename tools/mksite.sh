#!/bin/sh
# Assemble the NimbusOS website: the download page (web/) plus the signed
# update files from `make release`.
#   tools/mksite.sh <version> <iso-url> <iso-size-MB> <update-dir> <out-dir> [notes-file]
# With no arguments, generate a local preview in site-out using the default
# version and any update files already present in build/update.
set -eu
HERE="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$#" -gt 6 ]; then
    echo "usage: sh tools/mksite.sh [version [iso-url [iso-size-MB [update-dir [out-dir [notes-file]]]]]]" >&2
    exit 2
fi
VERSION=${1:-0.2}
ISO_URL=${2:-https://github.com/enderbrosik/nimbusOS/releases/download/v$VERSION/nimbusos-$VERSION.iso}
if [ -n "${3:-}" ]; then
    ISO_MB=$3
elif [ -f "$HERE/build/nimbus.iso" ]; then
    ISO_MB=$(du -m "$HERE/build/nimbus.iso" | cut -f1)
else
    ISO_MB=0
fi
UPD=${4:-$HERE/build/update}
OUT=${5:-$HERE/site-out}
NOTES_FILE=${6:-$HERE/release-notes.txt}
if [ -n "${4:-}" ] && [ ! -d "$UPD" ]; then
    echo "mksite: update directory not found: $UPD" >&2
    exit 1
fi
rm -rf "$OUT"
mkdir -p "$OUT"
HAS_UPDATE=0
if [ -d "$UPD" ]; then
    cp -R "$UPD"/. "$OUT"/
    HAS_UPDATE=1
fi
rm -rf "$OUT/.vercel"
cp "$HERE/web/icon.svg" "$OUT/icon.svg"
cp "$HERE/credits.txt" "$OUT/credits.txt"
{ printf 'NimbusOS %s\n\n' "$VERSION"; grep -v '^#' "$NOTES_FILE" | sed 's/^/- /'; } > "$OUT/release-notes.txt"
NOTES=$(grep -v '^#' "$NOTES_FILE" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/^/<li>/; s/$/<\/li>/' | tr -d '\n')
NOTES=$(printf '%s' "$NOTES" | sed 's/[&|\\]/\\&/g')       # escape for the sed replacement below
DATE=$(date -u +%Y-%m-%d)
python3 - "$HERE/web/index.html" "$OUT/index.html" "$VERSION" "$ISO_URL" "$ISO_MB" "$DATE" "$NOTES" "$HERE/credits.txt" "$HAS_UPDATE" <<'PY'
import html, pathlib, sys
src, dst, version, iso_url, iso_mb, date, notes, credits_path, has_update = sys.argv[1:10]
credits = pathlib.Path(credits_path).read_text(encoding='utf-8')
credits_html = html.escape(credits).replace('\n', '<br>\n')
updates_html = (
    '<h2>Updates</h2>\n'
    '<p>Installed systems check this site with Nimbus Update (Start menu, or '
    '<code>update</code> in the Terminal). Every release is signed with Ed25519, '
    'so NimbusOS only installs updates from its publisher.</p>'
) if has_update == '1' else ''
manifest_link = ' &middot; update manifest: <a href="manifest.txt">manifest.txt</a>' if has_update == '1' else ''
html_text = pathlib.Path(src).read_text(encoding='utf-8')
for key, value in {
    '{{VERSION}}': version,
    '{{ISO_URL}}': iso_url,
    '{{ISO_MB}}': iso_mb,
    '{{DATE}}': date,
    '{{NOTES}}': notes,
    '{{CREDITS}}': credits_html,
    '{{UPDATE_SECTION}}': updates_html,
    '{{MANIFEST_LINK}}': manifest_link,
}.items():
    html_text = html_text.replace(key, value)
pathlib.Path(dst).write_text(html_text, encoding='utf-8')
PY
echo "mksite: $OUT ready (NimbusOS $VERSION)"
