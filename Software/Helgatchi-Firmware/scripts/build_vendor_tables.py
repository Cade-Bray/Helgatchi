# Generates include/vendor_tables_data.h from the cached IEEE OUI registry
# and BT SIG company-identifier list at scripts/vendor_sources/.
#
# Output: raw tables with a deduplicated name string pool. No curation, no
# pattern matching — the firmware sees every MA-L OUI and every BT SIG mfg
# id. Rule matching against vendor names happens at rule load time
# (RulesService walks the table once and pre-computes prefix/id sets).
#
# Skips rewriting the header when content is unchanged.
#
# Layout of generated data (all inside namespace vendor_tables_data):
#
#   NAME_POOL    — concatenated NUL-terminated org names. One unique string
#                  per distinct registry name.
#   NAME_OFFSETS — uint32_t offset into NAME_POOL for each unique name.
#                  Indexed by name_idx.
#   OUI_PREFIXES — uint32_t 24-bit prefix, sorted ascending. Parallel to:
#   OUI_NAME_IDX — uint16_t name index for each prefix.
#   MFG_IDS      — uint16_t BT SIG company ID, sorted ascending. Parallel to:
#   MFG_NAME_IDX — uint16_t name index for each mfg id.

import csv
import gzip
import os
import re
import sys

Import("env")  # noqa: F821

PROJECT_DIR  = env["PROJECT_DIR"]
SOURCES_DIR  = os.path.join(PROJECT_DIR, "scripts", "vendor_sources")

OUI_CSV_GZ   = os.path.join(SOURCES_DIR, "oui.csv.gz")
BT_YAML      = os.path.join(SOURCES_DIR, "bt_companies.yaml")
OUT_HEADER   = os.path.join(PROJECT_DIR, "include", "vendor_tables_data.h")

# ---------------------------------------------------------------------------
# Parse upstream sources
# ---------------------------------------------------------------------------

if not os.path.isfile(OUI_CSV_GZ):
    sys.stderr.write(f"[vendor_tables] FATAL: {OUI_CSV_GZ} not found — "
                     f"run `python scripts/refresh_vendor_sources.py`\n")
    sys.exit(1)
if not os.path.isfile(BT_YAML):
    sys.stderr.write(f"[vendor_tables] FATAL: {BT_YAML} not found — "
                     f"run `python scripts/refresh_vendor_sources.py`\n")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Corporate-suffix stripping
#
# Most upstream org names carry boilerplate like ", Inc.", " Co., Ltd.",
# " GmbH" — useful for IP lawyers, useless on a 280x240 display. We strip
# these once here, so both the dedup pool and runtime lookups produce the
# clean form ("Apple, Inc." -> "Apple"). Pure brand prefixes like
# "Samsung Electronics" or "Cisco Systems" are NOT touched — they're part
# of the brand, not a corporate form.
#
# Suffixes are listed most-specific-first so "Co., Ltd." matches before
# "Co." or "Ltd." alone. Each pattern requires whitespace (or a comma) to
# the LEFT to avoid eating substrings like "...Co" in "Cisco".
# ---------------------------------------------------------------------------

_SUFFIX_PATTERNS = [
    r"GmbH\s*&\s*Co\.?\s*KG\.?",
    r"Co\.?\s*,?\s*Ltd\.?",
    r"Pte\.?\s*Ltd\.?",
    r"Pty\.?\s*Ltd\.?",
    r"Pvt\.?\s*Ltd\.?",
    r"Sdn\.?\s*Bhd\.?",
    r"Private\s+Limited",
    r"S\.?r\.?l\.?",
    r"S\.?p\.?A\.?",
    r"S\.?A\.?S\.?",
    r"S\.?A\.?",
    r"B\.?V\.?",
    r"L\.?L\.?C\.?",
    r"K\.?K\.?",
    r"Inc(?:orporated)?\.?",
    r"Ltd\.?",
    r"Limited",
    r"Corp(?:oration)?\.?",
    r"Co(?:mpany)?\.?",
    r"GmbH\.?",
    r"AG\.?",
    r"AB\.?",
    r"Oy\.?",
]

# Combined regex anchored at end-of-string, with optional leading comma +
# required whitespace OR a comma immediately before the suffix.
_SUFFIX_RE = re.compile(
    r"(?:\s*,)?\s+(?:" + "|".join(_SUFFIX_PATTERNS) + r")\s*$",
    re.IGNORECASE,
)

# Trailing junk left behind: lone trailing commas, periods, or whitespace.
_TRAIL_JUNK_RE = re.compile(r"[\s,.]+$")

def normalize_name(name):
    """Strip corporate suffixes iteratively. Bounded loop in case the source
    name has stacked forms like 'Foo Co., Ltd., Inc.' (rare but real)."""
    s = name.strip()
    for _ in range(4):
        new_s = _SUFFIX_RE.sub("", s)
        new_s = _TRAIL_JUNK_RE.sub("", new_s)
        if new_s == s or not new_s:
            # Either nothing changed, or stripping emptied the name — keep
            # what we have. An emptied name is the input itself.
            break
        s = new_s
    return s if s else name.strip()

# IEEE OUI CSV — only MA-L (24-bit prefixes). Skip anything else.
# Use a dict so duplicate prefixes (rare but they happen on reorgs) get
# overwritten by the latest occurrence rather than silently dropped.
ouis = {}    # prefix_int -> org_name
with gzip.open(OUI_CSV_GZ, "rt", encoding="utf-8", newline="") as f:
    reader = csv.reader(f)
    next(reader, None)  # skip header
    for row in reader:
        if len(row) < 3 or row[0] != "MA-L":
            continue
        assignment, org = row[1], row[2].strip()
        if len(assignment) != 6 or not org:
            continue
        try:
            prefix = int(assignment, 16)
        except ValueError:
            continue
        ouis[prefix] = normalize_name(org)

# BT SIG company_identifiers.yaml — line-based, no PyYAML dep.
mfgs = {}    # mfg_id_int -> name
with open(BT_YAML, "r", encoding="utf-8") as f:
    cur_value = None
    for line in f:
        s = line.strip()
        if s.startswith("- value:"):
            v = s[len("- value:"):].strip()
            try:
                cur_value = int(v, 0)
            except ValueError:
                cur_value = None
        elif s.startswith("name:") and cur_value is not None:
            name = s[len("name:"):].strip()
            if len(name) >= 2 and name[0] in "'\"" and name[-1] == name[0]:
                name = name[1:-1]
            name = name.replace("''", "'").strip()
            if name:
                mfgs[cur_value] = normalize_name(name)
            cur_value = None

# ---------------------------------------------------------------------------
# Build dedup'd name pool
# ---------------------------------------------------------------------------

# Collect all unique names. Pool order is determined by insertion order
# (first-seen wins) for stable diffs.
name_to_idx  = {}
pool_strings = []

def intern(name):
    idx = name_to_idx.get(name)
    if idx is not None:
        return idx
    idx = len(pool_strings)
    name_to_idx[name] = idx
    pool_strings.append(name)
    return idx

# Iterate OUIs in sorted prefix order so the same name gets the same index
# regardless of dict iteration order quirks.
oui_sorted = sorted(ouis.items())
mfg_sorted = sorted(mfgs.items())

oui_records = [(prefix, intern(name)) for prefix, name in oui_sorted]
mfg_records = [(mfg_id, intern(name)) for mfg_id, name in mfg_sorted]

# Compute byte offsets for each unique string in the concatenated pool.
name_offsets = []
total = 0
for s in pool_strings:
    name_offsets.append(total)
    total += len(s.encode("utf-8")) + 1   # +1 for NUL

# Names exceed uint16_t (~65K) once there are more than 65K unique strings.
# We have ~17K, well within range. Sanity check anyway.
if len(pool_strings) > 0xFFFF:
    sys.stderr.write(f"[vendor_tables] FATAL: {len(pool_strings)} unique names "
                     f"exceeds uint16_t name index\n")
    sys.exit(1)

# Pool larger than 4 GiB is comically impossible but guard anyway.
if total > 0xFFFFFFFF:
    sys.stderr.write(f"[vendor_tables] FATAL: name pool {total:,} bytes exceeds uint32_t\n")
    sys.exit(1)

# ---------------------------------------------------------------------------
# Emit header
# ---------------------------------------------------------------------------

def c_escape(s):
    """Escape a string for a C/C++ string literal. Handles non-printable and
    UTF-8 (already in source as bytes via \\xHH escapes)."""
    out = []
    for byte in s.encode("utf-8"):
        if byte == ord('"'):  out.append('\\"')
        elif byte == ord('\\'): out.append('\\\\')
        elif byte == ord('\n'): out.append('\\n')
        elif byte == ord('\t'): out.append('\\t')
        elif 0x20 <= byte < 0x7F: out.append(chr(byte))
        else: out.append(f"\\x{byte:02x}\"\"")
    return "".join(out)

lines = []
lines.append("#pragma once")
lines.append("// Auto-generated by scripts/build_vendor_tables.py — do not edit by hand.")
lines.append("// Source: scripts/vendor_sources/{oui.csv.gz,bt_companies.yaml}.")
lines.append("// Included only by src/vendor_lookup.cpp.")
lines.append("")
lines.append("#include <stdint.h>")
lines.append("")
lines.append("namespace vendor_tables_data {")
lines.append("")
lines.append(f"static constexpr uint16_t NAME_COUNT  = {len(pool_strings)};")
lines.append(f"static constexpr uint32_t OUI_COUNT   = {len(oui_records)};")
lines.append(f"static constexpr uint32_t MFG_COUNT   = {len(mfg_records)};")
lines.append("")

# --- NAME_POOL ---
# Array left unsized — adjacent string literals concatenate AND get a
# C-implicit trailing null, so the actual byte count is (sum of name+\0) + 1.
# Letting the compiler size it dodges off-by-one nitpicking.
lines.append("static const char NAME_POOL[] =")
chunk = []
chunk_count = 0
for s in pool_strings:
    chunk.append(f'"{c_escape(s)}\\0"')
    chunk_count += 1
    if chunk_count >= 32:
        lines.append("    " + "".join(chunk))
        chunk = []
        chunk_count = 0
if chunk:
    lines.append("    " + "".join(chunk))
lines.append("    ;")
lines.append("")

# --- NAME_OFFSETS ---
lines.append("static const uint32_t NAME_OFFSETS[NAME_COUNT] = {")
for i in range(0, len(name_offsets), 8):
    chunk = name_offsets[i:i+8]
    lines.append("    " + ", ".join(str(x) for x in chunk) + ",")
lines.append("};")
lines.append("")

# --- OUI_PREFIXES + OUI_NAME_IDX ---
lines.append("static const uint32_t OUI_PREFIXES[OUI_COUNT] = {")
for i in range(0, len(oui_records), 8):
    chunk = oui_records[i:i+8]
    lines.append("    " + ", ".join(f"0x{p:06X}" for p, _ in chunk) + ",")
lines.append("};")
lines.append("")
lines.append("static const uint16_t OUI_NAME_IDX[OUI_COUNT] = {")
for i in range(0, len(oui_records), 16):
    chunk = oui_records[i:i+16]
    lines.append("    " + ", ".join(str(n) for _, n in chunk) + ",")
lines.append("};")
lines.append("")

# --- MFG_IDS + MFG_NAME_IDX ---
lines.append("static const uint16_t MFG_IDS[MFG_COUNT] = {")
for i in range(0, len(mfg_records), 16):
    chunk = mfg_records[i:i+16]
    lines.append("    " + ", ".join(f"0x{m:04X}" for m, _ in chunk) + ",")
lines.append("};")
lines.append("")
lines.append("static const uint16_t MFG_NAME_IDX[MFG_COUNT] = {")
for i in range(0, len(mfg_records), 16):
    chunk = mfg_records[i:i+16]
    lines.append("    " + ", ".join(str(n) for _, n in chunk) + ",")
lines.append("};")
lines.append("")

lines.append("}  // namespace vendor_tables_data")
lines.append("")

contents = "\n".join(lines)

# ---------------------------------------------------------------------------
# Write (only on content change)
# ---------------------------------------------------------------------------

existing = ""
if os.path.isfile(OUT_HEADER):
    try:
        with open(OUT_HEADER, "r", encoding="utf-8") as f:
            existing = f.read()
    except OSError:
        pass

if existing != contents:
    os.makedirs(os.path.dirname(OUT_HEADER), exist_ok=True)
    with open(OUT_HEADER, "w", encoding="utf-8") as f:
        f.write(contents)
    state = "regenerated"
else:
    state = "up to date"

# Size summary so the build log is informative.
oui_bytes = len(oui_records) * (4 + 2)
mfg_bytes = len(mfg_records) * (2 + 2)
off_bytes = len(name_offsets) * 4
total_bytes = total + oui_bytes + mfg_bytes + off_bytes

def fmt_kb(n):
    return f"{n/1024:.0f} KB" if n >= 10240 else f"{n} B"

print(f"[vendor_tables] {state}: "
      f"{len(ouis):,} OUIs, {len(mfgs):,} mfg ids, "
      f"{len(pool_strings):,} unique names")
print(f"[vendor_tables] flash cost: "
      f"pool {fmt_kb(total)}, offsets {fmt_kb(off_bytes)}, "
      f"oui {fmt_kb(oui_bytes)}, mfg {fmt_kb(mfg_bytes)} "
      f"= {fmt_kb(total_bytes)} total")
