# Refreshes the cached IEEE OUI and BT SIG company-identifier sources that
# scripts/build_vendor_tables.py consumes. Run by hand when you want to pick
# up newly-registered vendors — typically once a month is plenty.
#
#   python scripts/refresh_vendor_sources.py
#
# Cache files (committed to the repo so first-time builds work offline):
#   scripts/vendor_sources/oui.csv.gz       (IEEE OUI registry, MA-L)
#   scripts/vendor_sources/bt_companies.yaml (BT SIG company identifiers)
#
# This script is intentionally NOT a PIO pre-script — we don't want every
# build to hit the network. Refresh is a deliberate, separate step.

import gzip
import os
import sys
import urllib.request

THIS_DIR    = os.path.dirname(os.path.abspath(__file__))
CACHE_DIR   = os.path.join(THIS_DIR, "vendor_sources")

OUI_URL      = "https://standards-oui.ieee.org/oui/oui.csv"
OUI_OUT      = os.path.join(CACHE_DIR, "oui.csv.gz")

BT_SIG_URL   = ("https://bitbucket.org/bluetooth-SIG/public/raw/HEAD/"
                "assigned_numbers/company_identifiers/company_identifiers.yaml")
BT_SIG_OUT   = os.path.join(CACHE_DIR, "bt_companies.yaml")

def fetch(url, timeout=30):
    print(f"  GET {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "helgatchi-vendor-refresh/1"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()

def main():
    os.makedirs(CACHE_DIR, exist_ok=True)

    print("[refresh] IEEE OUI registry")
    try:
        body = fetch(OUI_URL)
    except Exception as exc:
        sys.stderr.write(f"  FAILED: {exc}\n")
        return 1
    with gzip.open(OUI_OUT, "wb") as f:
        f.write(body)
    print(f"  wrote {OUI_OUT}  ({len(body):,} raw, "
          f"{os.path.getsize(OUI_OUT):,} compressed)")

    print("[refresh] BT SIG company identifiers")
    try:
        body = fetch(BT_SIG_URL)
    except Exception as exc:
        sys.stderr.write(f"  FAILED: {exc}\n")
        return 1
    with open(BT_SIG_OUT, "wb") as f:
        f.write(body)
    print(f"  wrote {BT_SIG_OUT}  ({len(body):,} bytes)")

    print()
    print("[refresh] done. Rebuild firmware to pick up new entries.")
    return 0

if __name__ == "__main__":
    sys.exit(main())
