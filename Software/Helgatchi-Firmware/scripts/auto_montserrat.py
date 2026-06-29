# Auto-enable LVGL Montserrat font sizes referenced anywhere in src/.
#
# SquareLine-exported screens (src/UI/*.c) reference fonts as
# `&lv_font_montserrat_36` etc. Each referenced size needs the matching
# `LV_FONT_MONTSERRAT_NN=1` define so the LVGL library actually compiles
# that font's data section — otherwise you get a linker error
# ("undefined reference to lv_font_montserrat_NN") or a compile error
# in the SLS file itself.
#
# This script scans src/ for every `lv_font_montserrat_NN` reference and
# appends `-DLV_FONT_MONTSERRAT_NN=1` to the build flags. Combined with
# the #ifndef guards in include/lv_conf.h, this makes new sizes get picked
# up automatically on the next build after re-exporting from SLS.
#
# To take effect after adding a new font size, do `pio run -t clean` first
# (or delete .pio/libdeps/.../lvgl) — the LVGL library archive is cached
# and only rebuilds when build flags change.

import os
import re
import glob

Import("env")  # noqa: F821  (provided by PlatformIO)

PROJECT_DIR = env["PROJECT_DIR"]
SRC_ROOTS   = [os.path.join(PROJECT_DIR, "src"), os.path.join(PROJECT_DIR, "include")]
PATTERN     = re.compile(r"\blv_font_montserrat_(\d+)\b")

sizes = set()
for root in SRC_ROOTS:
    if not os.path.isdir(root):
        continue
    for ext in ("*.c", "*.cpp", "*.h", "*.hpp"):
        for path in glob.glob(os.path.join(root, "**", ext), recursive=True):
            try:
                with open(path, "r", encoding="utf-8", errors="ignore") as f:
                    for m in PATTERN.finditer(f.read()):
                        sizes.add(int(m.group(1)))
            except OSError:
                pass

for sz in sorted(sizes):
    env.Append(BUILD_FLAGS=[f"-DLV_FONT_MONTSERRAT_{sz}=1"])

if sizes:
    print(f"[auto_montserrat] enabled sizes: {sorted(sizes)}")
else:
    print("[auto_montserrat] no Montserrat fonts referenced — nothing to do")
