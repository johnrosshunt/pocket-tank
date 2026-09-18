# Browser installer

Plug the board in, open a page, click Install: the same "flash it from the
browser" flow ESPHome and Home Assistant use ([ESP Web
Tools](https://esphome.github.io/esp-web-tools/), Apache-2.0, vendored under
`vendor/`). Chrome or Edge on a desktop; it uses Web Serial, which Safari
and Firefox don't have.

## Build the upload folder

```
idf.py -B ~/.cache/pocket-tank/fw-build build     # in firmware/ (or any -B dir)
tools/make_installer.py                            # -> installer/dist/
python3 -m http.server -d installer/dist 8765      # local check at http://localhost:8765
```

The page follows stratobuilds.com's design (the `#f8f8f8` page, white /
`#222` / lavender cards at 10px, Inter Tight and Roboto Mono from Google
Fonts, the red button). When only the page changed, re-uploading
`index.html` is enough - the binaries and `vendor/` are untouched.

`installer/dist/` is the whole thing: `index.html`, `manifest.json`,
`manifest-erase.json`, `firmware/*.bin` (bootloader, partition table, app,
model), `vendor/`. The offsets in the manifests come from the build's
`flasher_args.json` and from `firmware/partitions.csv`, and the page carries
the git version. It is gitignored: rebuild it for every release.

## Updating never erases (the two manifests, and one patched line)

The tank's save lives in NVS at `0x9000`; the four parts sit at `0x0`,
`0x8000`, `0x10000` and `0x290000`, so a plain write of the four leaves it
alone and the tank carries on after an update (`tools/flash.sh` does the same
writes every day). The catch was in the dialog: ESP Web Tools 10.4.0 has no
manifest option for "install, don't erase" on a device without Improv.
`new_install_prompt_erase: true` asks (checkbox off by default), and without
it the dialog erases the chip first, unconditionally. So:

- `manifest.json` (the red button) carries `"never_erase": true` and
  `make_installer.py` patches the copied `install-dialog-*.js` at assembly:
  the two click handlers that read `_startInstall(!0)` (erase) become
  `_startInstall(!this._manifest.never_erase)`. The build fails loudly if
  the handler text is not found exactly twice, so a vendor upgrade can't
  ship an erasing page by accident. `vendor/` itself stays pristine.
- `manifest-erase.json` (the "Erase the board and install fresh" button)
  is the same parts with the erase question, for a board that is stuck or
  a keeper who wants a clean flash. The on-device reset (hold BOOT, tap the
  glass) is the normal way to start over and needs no page.

The firmware side of the promise: `common/progression.c` loads a save of any
older length (the tail only ever appends) and slides the one mid-struct
insertion (bubble_x, 2026-09-14) into place for saves from the first public
builds; `sim/fishsim --selftest-sleep` covers both.

## It updates itself (GitHub Pages)

`.github/workflows/installer.yml` in the PUBLIC repo builds the firmware
with ESP-IDF v5.4.1 on every push to `main` that touches `firmware/`,
`common/`, `model/out/`, `installer/` or the assembler, runs
`tools/make_installer.py`, and deploys the folder to
**https://mediacutlet.github.io/pocket-tank/**. Pages serves HTTPS with
`Access-Control-Allow-Origin: *`, so the copy on stratobuilds.com points
its button at that manifest:

```
tools/make_installer.py --manifest-url https://mediacutlet.github.io/pocket-tank/manifest.json --out /tmp/site
```

and that `index.html` PLUS its `vendor/esp-web-tools-<tag>/` folder live on
the site - the never-erase patch is in the vendor's dialog bundle. An old
`vendor/` next to the new manifest erased every install without asking
(the 09-11 upload, found 2026-09-18), and the host serves `.js` with a
year's max-age, so the folder name now carries the patched dialog's hash.
One command builds, uploads over ssh (host `stratobuilds`), purges
SiteGround's dynamic cache and checks the live URL:

```
pocket-tank/tools/publish_site_installer.sh
```

Run it whenever the page, the vendored ESP Web Tools or the patch changes.
The page fetches the manifest on load and shows the version and build date of what it will actually flash,
so pushing to the public repo is the whole release step: no upload, no
cache purge. (Manual failure mode: the Actions run is red - `gh run list
--repo mediacutlet/pocket-tank`.)

## Host it

Web Serial needs a secure context, so the page must be on **HTTPS** (or
`localhost`). Any static host works; the manifest and the `.bin` files must be
fetchable from the page's origin (or send CORS headers).

**stratobuilds.com (WordPress behind Cloudflare):** upload `installer/dist/`
as a folder next to WordPress, e.g. `public_html/pocket-tank/`, and link
`https://stratobuilds.com/pocket-tank/`. Being a plain folder it is outside
WordPress, so themes, caching and security plugins don't touch it. Things to
check once:

- Open the page, the button must say *Install Pocket Tank*, not the red
  unsupported text. If it never appears, Cloudflare's Rocket Loader is
  rewriting the module script: exclude `/pocket-tank/*` from it (a
  Configuration Rule), or turn it off.
- In the browser's network tab `manifest.json` and `firmware/*.bin` must be
  200. A 403 on `.bin` means the host blocks the type: the `.htaccess` in the
  folder adds it for Apache/LiteSpeed; on nginx add
  `types { application/octet-stream bin; }`.
- Cloudflare caches `.bin` and `.js` by default. After uploading a new build,
  purge `/pocket-tank/*` (the manifest carries the version, so a stale
  cache shows an old version string on the page).

**GitHub Pages** (the public repo) is the other easy option: push `dist/` to
a `gh-pages` branch or a `docs/installer/` folder. Pages serves HTTPS and
`Access-Control-Allow-Origin: *`, so the button can even live on a WordPress
page (Custom HTML block with the `<script type="module">` tag and the
`<esp-web-install-button manifest="https://...manifest.json">` element)
while the binaries stay on GitHub.

## What the user sees

Click → the browser's port picker (`USB JTAG/serial debug unit`) → *Install
Pocket Tank* → "Do you want to install Pocket Tank <version>?" → a progress
bar over the four parts → *Installation complete*, and the board resets into
the tank: fresh on a blank board, the same tank on one that had it. The
"start over" button adds the erase question (tick *Erase device*). The page's
"If something's off" section covers the usual snags: a sleeping tank hides
its USB port (press PWR), holding BOOT while plugging in forces download
mode, Linux group permissions, charge-only cables, and an optional esptool
backup of the save area for the cautious.

## Verified

The artifact set in `dist/firmware/` at the manifest's offsets was written to
the real board with `esptool.py write_flash` (the exact operation ESP Web
Tools performs, from the same files), and the tank booted; the page, manifest
and vendor bundle were checked from a local server. The browser's own port
picker is a native dialog, so the click-through itself is a human test.
