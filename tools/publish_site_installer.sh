#!/bin/bash
# The stratobuilds.com copy of the browser installer: page + patched vendor
# folder, its buttons pointed at the GitHub Pages manifests (the firmware
# itself is released by pushing the public repo, not from here).
#
#   pocket-tank/tools/publish_site_installer.sh
#
# Needed whenever installer/index.html, the vendored ESP Web Tools or the
# never_erase patch changes. Builds the folder, rsyncs it over ssh (host
# "stratobuilds" in ~/.ssh/config), purges SiteGround's dynamic cache (it
# holds the page; .htaccess headers don't reach nginx-served files there) and
# checks what the public URL serves. Old vendor-<tag> folders stay on the
# server on purpose: a page cached somewhere must never point at a 404.
set -euo pipefail
cd "$(dirname "$0")/.."
URL=https://stratobuilds.com/pocket-tank-installer
REMOTE=www/stratobuilds.com/public_html
OUT=$(mktemp -d)/site
trap 'rm -rf "$(dirname "$OUT")"' EXIT

python3 tools/make_installer.py --out "$OUT" \
    --manifest-url https://mediacutlet.github.io/pocket-tank/manifest.json
rm -rf "$OUT/firmware" "$OUT"/manifest*.json "$OUT/.nojekyll"   # served by Pages, not the site

rsync -rlt --checksum -e ssh "$OUT/" "stratobuilds:$REMOTE/pocket-tank-installer/"
ssh stratobuilds "cd $REMOTE && wp sg purge" | grep -i "dynamic cache"

vendor=$(grep -o 'vendor/esp-web-tools-[0-9a-f]*' "$OUT/index.html" | head -1)
live=$(curl -sL -m 30 "$URL/" | grep -o 'vendor/esp-web-tools-[0-9a-f]*' | head -1)
[ "$live" = "$vendor" ] || { echo "LIVE PAGE STILL POINTS AT '$live', expected $vendor"; exit 1; }
dialog=$(basename "$OUT/$vendor"/install-dialog-*.js)
n=$(curl -sL -m 60 "$URL/$vendor/$dialog" | grep -c never_erase || true)
[ "$n" -ge 1 ] || { echo "LIVE DIALOG IS NOT THE PATCHED ONE ($URL/$vendor/$dialog)"; exit 1; }
echo "ok: $URL serves $vendor with the never_erase dialog"
