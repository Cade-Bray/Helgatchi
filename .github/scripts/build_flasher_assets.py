#!/usr/bin/env python3
"""Mirror release firmware into the Web Flasher site and generate its manifests.

Run by .github/workflows/pages.yml on the runner (server-side, no CORS). For
every published release that carries the four raw images, this:

  * downloads bootloader/partitions/firmware/littlefs .bin into firmware/<tag>/
  * writes a blank (0xFF) NVS image there — flashing it resets settings
  * emits one manifest per flash mode (see MODES below)
  * appends the release to versions.json (newest first)

Offsets mirror Software/Helgatchi-Firmware/partitions.csv. esp-web-tools writes
each listed part at its offset and, when the user leaves erase unchecked, leaves
every other region (notably NVS at 0x9000) untouched — so a firmware-only flash
preserves settings and rules, exactly like `pio run -t upload`.

Usage: build_flasher_assets.py <site_dir>
Env:   GITHUB_REPOSITORY, GH_TOKEN (for the gh CLI)
"""

import json
import os
import pathlib
import subprocess
import sys

# Must match partitions.csv (nvs @ 0x9000/0x7000, factory @ 0x10000,
# spiffs/LittleFS @ 0x510000).
BOOTLOADER = 0x0
PARTITIONS = 0x8000
NVS        = 0x9000
NVS_SIZE   = 0x7000
APP        = 0x10000
FS         = 0x510000

REQUIRED = ["bootloader.bin", "partitions.bin", "firmware.bin", "littlefs.bin"]

SITE = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "WebFlasher")
REPO = os.environ["GITHUB_REPOSITORY"]


def part(path, offset):
    return {"path": path, "offset": offset}


# Each flash mode -> the parts it writes. Keys match the manifest filenames the
# page requests based on the firmware/rules/wipe checkboxes.
def modes():
    boot = part("bootloader.bin", BOOTLOADER)
    ptab = part("partitions.bin", PARTITIONS)
    app  = part("firmware.bin",   APP)
    fs   = part("littlefs.bin",   FS)
    nvs  = part("nvs_blank.bin",  NVS)
    return {
        "manifest-fw.json":   [boot, ptab, app],            # firmware only
        "manifest-fs.json":   [fs],                          # rules only
        "manifest-fwfs.json": [boot, ptab, app, fs],         # firmware + rules
        "manifest-full.json": [boot, ptab, nvs, app, fs],    # full wipe + reflash
    }


def list_releases():
    # `gh api --paginate` concatenates one JSON array per page; decode them in
    # sequence and flatten.
    raw = subprocess.run(
        ["gh", "api", f"repos/{REPO}/releases", "--paginate"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    decoder = json.JSONDecoder()
    items, idx = [], 0
    while idx < len(raw):
        page, idx = decoder.raw_decode(raw, idx)
        items.extend(page)
        while idx < len(raw) and raw[idx].isspace():
            idx += 1
    return items


def write_manifest(dst, name, version, parts):
    manifest = {
        "name": "Helgatchi",
        "version": version,
        # true -> esp-web-tools shows an erase prompt that DEFAULTS to unchecked,
        # so a plain continue writes only these parts. false would force a full
        # chip erase (our firmware has no improv "same firmware" signal), which
        # is exactly what wiped settings before.
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
    }
    (dst / name).write_text(json.dumps(manifest, indent=2))


def main():
    firmware_dir = SITE / "firmware"
    firmware_dir.mkdir(parents=True, exist_ok=True)

    versions = []
    for rel in list_releases():
        if rel.get("draft"):
            continue
        tag = rel["tag_name"]
        have = {a["name"] for a in rel.get("assets", [])}
        missing = [r for r in REQUIRED if r not in have]
        if missing:
            print(f"skip {tag}: missing {missing}")
            continue

        dst = firmware_dir / tag
        dst.mkdir(parents=True, exist_ok=True)
        patterns = []
        for r in REQUIRED:
            patterns += ["--pattern", r]
        subprocess.run(
            ["gh", "release", "download", tag, "--repo", REPO,
             *patterns, "--dir", str(dst), "--clobber"],
            check=True,
        )
        # Blank NVS == erased flash (0xFF); flashing it makes NVS reinitialize.
        (dst / "nvs_blank.bin").write_bytes(b"\xff" * NVS_SIZE)

        version = tag[1:] if tag.startswith("v") else tag
        for name, parts in modes().items():
            write_manifest(dst, name, version, parts)

        versions.append({
            "tag": tag,
            "prerelease": rel["prerelease"],
            "date": rel["published_at"],
        })
        print(f"bundled {tag}")

    (SITE / "versions.json").write_text(json.dumps(versions, indent=2))
    print(f"wrote versions.json with {len(versions)} release(s)")


if __name__ == "__main__":
    main()
