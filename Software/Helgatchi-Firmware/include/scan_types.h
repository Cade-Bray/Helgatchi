#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Scan result — one device sighting from BLE or WiFi.
//
// Produced by ScanService (NimBLE callbacks + WiFi scan) and consumed by:
//   - RulesService          drains the ring buffer, matches against loaded
//                           rulesets, raises alerts on hit.
//   - DeviceListScreen      reads the dedup'd seen-devices map for browsing.
//
// Stays a POD struct so it can sit in PSRAM ring storage and be memcpy'd
// freely. No pointers, no virtuals.
// ---------------------------------------------------------------------------

enum ScanDomain : uint8_t {
    SCAN_BLE  = 0,
    SCAN_WIFI = 1,
};

struct ScanResult {
    uint8_t  domain;            // ScanDomain
    uint8_t  mac[6];
    int8_t   rssi;
    char     name[32];          // BLE adv name OR WiFi SSID. NUL-terminated; truncated if longer.
    uint16_t mfg_id;            // BT SIG company ID (BLE). 0 = none. Unused for WiFi.
    uint8_t  service_count;     // number of populated entries in service_uuids
    // 128-bit UUIDs in BLE wire order (LSB first). 16/32-bit UUIDs from
    // NimBLE are promoted via the BLE base UUID at scan-emit time so
    // rule matching has a single canonical form.
    uint8_t  service_uuids[4][16];
    uint32_t timestamp_ms;      // millis() at the scan callback
};

static_assert(sizeof(ScanResult) <= 128, "ScanResult bloating — reduce service slots or trim name");
