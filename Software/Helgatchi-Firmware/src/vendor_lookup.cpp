#include "vendor_lookup.h"
#include "vendor_tables_data.h"   // generated; see scripts/build_vendor_tables.py

using namespace vendor_tables_data;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Map a name_idx into the pool to a C-string pointer. Inline so the cost is
// just an array lookup.
static inline const char* _name_for(uint16_t name_idx) {
    return &NAME_POOL[NAME_OFFSETS[name_idx]];
}

// ---------------------------------------------------------------------------
// Forward lookups — bsearch over sorted parallel arrays.
// ---------------------------------------------------------------------------

const char* vendor_oui_lookup(uint32_t oui_prefix) {
    if (OUI_COUNT == 0) return nullptr;
    size_t lo = 0, hi = OUI_COUNT;
    while (lo < hi) {
        const size_t   mid = (lo + hi) >> 1;
        const uint32_t p   = OUI_PREFIXES[mid];
        if      (p < oui_prefix) lo = mid + 1;
        else if (p > oui_prefix) hi = mid;
        else                     return _name_for(OUI_NAME_IDX[mid]);
    }
    return nullptr;
}

const char* vendor_mfg_lookup(uint16_t mfg_id) {
    if (MFG_COUNT == 0) return nullptr;
    size_t lo = 0, hi = MFG_COUNT;
    while (lo < hi) {
        const size_t   mid = (lo + hi) >> 1;
        const uint16_t m   = MFG_IDS[mid];
        if      (m < mfg_id) lo = mid + 1;
        else if (m > mfg_id) hi = mid;
        else                 return _name_for(MFG_NAME_IDX[mid]);
    }
    return nullptr;
}

const char* vendor_for_mac(const uint8_t mac[6]) {
    const uint32_t prefix =
        ((uint32_t)mac[0] << 16) |
        ((uint32_t)mac[1] <<  8) |
        ((uint32_t)mac[2]);
    return vendor_oui_lookup(prefix);
}

// ---------------------------------------------------------------------------
// Iteration accessors
// ---------------------------------------------------------------------------

size_t vendor_oui_count() { return OUI_COUNT; }
size_t vendor_mfg_count() { return MFG_COUNT; }

bool vendor_oui_at(size_t idx, uint32_t* prefix_out, const char** name_out) {
    if (idx >= OUI_COUNT) return false;
    if (prefix_out) *prefix_out = OUI_PREFIXES[idx];
    if (name_out)   *name_out   = _name_for(OUI_NAME_IDX[idx]);
    return true;
}

bool vendor_mfg_at(size_t idx, uint16_t* mfg_id_out, const char** name_out) {
    if (idx >= MFG_COUNT) return false;
    if (mfg_id_out) *mfg_id_out = MFG_IDS[idx];
    if (name_out)   *name_out   = _name_for(MFG_NAME_IDX[idx]);
    return true;
}
