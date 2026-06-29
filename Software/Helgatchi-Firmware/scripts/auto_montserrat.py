# Auto-enable LVGL Montserrat font sizes ACTUALLY USED in src/.
#
# EEZ-exported screens (src/UI/*.c) reference fonts as
# `&lv_font_montserrat_36` etc. Each referenced size needs the matching
# `LV_FONT_MONTSERRAT_NN=1` define so the LVGL library compiles that
# font's data section — otherwise you get a linker error
# ("undefined reference to lv_font_montserrat_NN").
#
# EEZ's generated screens.c also contains a registration table that
# enumerates EVERY Montserrat size from 8..48, each entry guarded by
# `#if LV_FONT_MONTSERRAT_NN`. Those guarded references are circular —
# they only compile if the size is already enabled — so they should not
# count as "in use". This script tracks `#if LV_FONT_MONTSERRAT_NN`
# nesting and ignores self-guarded references.
#
# To take effect after the set of used sizes changes, do `pio run -t clean`
# first (or delete .pio/libdeps/.../lvgl) — the LVGL library archive is
# cached and only rebuilds when build flags change.

import os
import re
import glob

Import("env")  # noqa: F821  (provided by PlatformIO)

PROJECT_DIR = env["PROJECT_DIR"]
SRC_ROOTS   = [os.path.join(PROJECT_DIR, "src"), os.path.join(PROJECT_DIR, "include")]

USE_RE   = re.compile(r"\blv_font_montserrat_(\d+)\b")
IF_RE    = re.compile(r"^\s*#\s*if(?:def|ndef)?\s+(.+?)\s*(?://.*)?$")
ENDIF_RE = re.compile(r"^\s*#\s*endif\b")
GATE_RE  = re.compile(r"^LV_FONT_MONTSERRAT_(\d+)$")

# Strips C/C++ comments so font references inside them don't count as uses.
# Not a full preprocessor — good enough for source files: replaces /*...*/ and
# everything from // to end-of-line with spaces (preserves line numbering).
_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT  = re.compile(r"//[^\n]*")


def strip_comments(text):
    text = _BLOCK_COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)
    text = _LINE_COMMENT.sub("", text)
    return text


def scan_file(path):
    """Return the set of Montserrat sizes used unconditionally in `path`."""
    sizes = set()
    # Stack entry is the gated size (int) if the surrounding #if is
    # `#if LV_FONT_MONTSERRAT_NN`, otherwise None. Lets us nest properly
    # without parsing arbitrary preprocessor expressions.
    stack = []
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        text = strip_comments(f.read())
    for line in text.splitlines():
        m_if = IF_RE.match(line)
        if m_if:
            m_gate = GATE_RE.match(m_if.group(1))
            stack.append(int(m_gate.group(1)) if m_gate else None)
            continue
        if ENDIF_RE.match(line):
            if stack:
                stack.pop()
            continue
        for m in USE_RE.finditer(line):
            n = int(m.group(1))
            if n not in stack:
                sizes.add(n)
    return sizes


sizes = set()
for root in SRC_ROOTS:
    if not os.path.isdir(root):
        continue
    for ext in ("*.c", "*.cpp", "*.h", "*.hpp"):
        for path in glob.glob(os.path.join(root, "**", ext), recursive=True):
            try:
                sizes |= scan_file(path)
            except OSError:
                pass

for sz in sorted(sizes):
    env.Append(BUILD_FLAGS=[f"-DLV_FONT_MONTSERRAT_{sz}=1"])

if sizes:
    print(f"[auto_montserrat] enabled sizes: {sorted(sizes)}")
else:
    print("[auto_montserrat] no Montserrat fonts referenced — nothing to do")
