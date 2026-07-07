#include "scan_engine.h"
#include "scan_service.h"
#include "settings_service.h"
#include "settings_keys.h"
#include "vendor_lookup.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

ScanEngine g_scan_engine;

// ---------------------------------------------------------------------------
// NimBLE callback bridge
//
// onResult fires on the BLE host task (NimBLE's internal FreeRTOS task), so
// it must NOT call ScanService::publish() directly — publish is single-
// threaded by contract. The callback instead builds a ScanResult and pushes
// it through a FreeRTOS queue; ScanEngine::tick() drains the queue on the
// main loop and calls publish() from there.
// ---------------------------------------------------------------------------

namespace {

QueueHandle_t s_queue = nullptr;

// Counter pointers — set in ScanEngine::begin so the callback can update
// stats without needing g_scan_engine private accessors.
uint32_t* s_cb_count   = nullptr;
uint32_t* s_q_overflow = nullptr;

// Classify a BLE address into a MacAddrType. `ble_type` is ble_addr_t.type
// (0=public, 1=random, 2=public_id, 3=random_id — the _id variants are
// controller-resolved RPAs, so even => public identity, odd => random). For
// random addresses the sub-type lives in the top two bits of the MSB.
uint8_t classifyBleMac(uint8_t ble_type, uint8_t msb) {
    if ((ble_type & 0x01) == 0) return MAC_TYPE_PUBLIC;
    switch (msb >> 6) {
        case 0b11: return MAC_TYPE_RANDOM_STATIC;
        case 0b01: return MAC_TYPE_RPA;
        case 0b00: return MAC_TYPE_NRPA;
        default:   return MAC_TYPE_RANDOM_OTHER;   // 0b10 reserved
    }
}

class HelgatchiScanCallbacks : public NimBLEScanCallbacks {
public:
    void onResult(const NimBLEAdvertisedDevice* dev) override {
        if (!s_queue || !dev) return;
        if (s_cb_count) (*s_cb_count)++;

        ScanResult r{};
        r.domain        = SCAN_BLE;
        r.timestamp_ms  = millis();

        // MAC — NimBLEAddress::getBase() returns ble_addr_t with `val` in
        // little-endian (radio wire order). We store display order (MSB first).
        const NimBLEAddress addr = dev->getAddress();
        const uint8_t* native = addr.getBase()->val;
        for (int i = 0; i < 6; i++) r.mac[i] = native[5 - i];

        // Classify from the advertised address type + the MSB (r.mac[0]).
        r.mac_type = classifyBleMac(addr.getBase()->type, r.mac[0]);

        r.rssi = (int8_t)dev->getRSSI();

        // Adv name. NimBLE returns std::string; empty if none.
        if (dev->haveName()) {
            const std::string& nm = dev->getName();
            const size_t n = nm.size() < (sizeof(r.name) - 1)
                             ? nm.size() : (sizeof(r.name) - 1);
            memcpy(r.name, nm.data(), n);
            r.name[n] = '\0';
        }

        // Manufacturer ID — first 2 bytes of mfg-specific data, little-endian.
        if (dev->haveManufacturerData()) {
            const std::string& md = dev->getManufacturerData();
            if (md.size() >= 2) {
                r.mfg_id = (uint16_t)((uint8_t)md[0]) |
                           ((uint16_t)((uint8_t)md[1]) << 8);
            }
        }

        // Service UUIDs — stash up to 4, normalized to 128-bit wire order.
        // NimBLEUUID::to128() mutates the UUID in place, so we need a
        // non-const copy. getValue() returns a raw byte pointer (16 bytes
        // after promotion).
        r.service_count = 0;
        const size_t n_svc = dev->getServiceUUIDCount();
        for (size_t i = 0; i < n_svc && r.service_count < 4; i++) {
            NimBLEUUID big(dev->getServiceUUID(i));
            big.to128();
            const uint8_t* bytes = big.getValue();
            if (bytes) {
                memcpy(r.service_uuids[r.service_count], bytes, 16);
                r.service_count++;
            }
        }

        // Non-blocking push. If the main loop is starved and we run out of
        // queue space, count the loss and drop. Re-firings on the same MAC
        // are common at active advertisement rates, so a missed one is rarely
        // the only chance we get.
        if (xQueueSend(s_queue, &r, 0) != pdTRUE) {
            if (s_q_overflow) (*s_q_overflow)++;
        }
    }

    // onScanEnd fires when a finite duration scan completes. We use
    // duration=0 (forever) so this normally doesn't fire — but if NimBLE
    // ever stops on its own (e.g. due to a stack error), restart so we
    // don't go silent.
    void onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) override {
        if (g_scan_engine.bleActive()) {
            NimBLEScan* scan = NimBLEDevice::getScan();
            if (scan) scan->start(0, false);
        }
    }
};

HelgatchiScanCallbacks s_callbacks;

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ScanEngine::begin(EventBus& bus) {
    _bus = &bus;

    // Queue lives in DRAM (FreeRTOS internal). ~64 entries * sizeof(ScanResult)
    // ~= 7 KB. Acceptable; if we ever need to shrink, move ScanResult to a
    // smaller "raw" payload and reconstruct in tick().
    if (!s_queue) {
        s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(ScanResult));
        if (!s_queue) return;
    }
    _queue       = s_queue;
    s_cb_count   = &_cb_count;
    s_q_overflow = &_q_overflow;

    bus.subscribe(CMD_SCAN_START,      this);
    bus.subscribe(CMD_SCAN_STOP,       this);
    bus.subscribe(EV_SETTINGS_CHANGED, this);
}

size_t ScanEngine::queueDepth() const {
    if (!_queue) return 0;
    return (size_t)uxQueueMessagesWaiting((QueueHandle_t)_queue);
}

void ScanEngine::tick() {
    if (!_queue) return;
    // Drain whatever's in the queue. Cap per-tick so a flood doesn't starve
    // the rest of the loop; 32 is well above the steady-state inflow rate
    // and matches a couple of advertisements per millisecond.

    // Raw-dump mode is opted into via DEBUG_SCANNING_PERF + serial enabled.
    // Settings reads are cheap (in-memory uint32 fetch); checking once per
    // tick avoids interleaving the lookup with the queue drain.
    const bool log_raw = g_settings.getBool(SKEY_DEBUG_SERIAL_ENABLED) &&
                         g_settings.get(SKEY_DEBUG_LEVEL) == DEBUG_SCANNING_PERF;

    ScanResult r;
    for (int drained = 0; drained < 32; drained++) {
        if (xQueueReceive((QueueHandle_t)_queue, &r, 0) != pdTRUE) break;
        g_scan.publish(r);
        _pub_count++;

        if (log_raw) {
            const char* oui_org = vendor_for_mac(r.mac);
            const char* mfg_org = r.mfg_id ? vendor_mfg_lookup(r.mfg_id) : nullptr;
            Serial.printf("[scan] %02X:%02X:%02X:%02X:%02X:%02X "
                          "type=%-8s rssi=%-4d mfg=0x%04X svc=%u "
                          "oui=%-16.16s mfg_org=%-16.16s name=\"%s\"\n",
                          r.mac[0], r.mac[1], r.mac[2],
                          r.mac[3], r.mac[4], r.mac[5],
                          macTypeName(r.mac_type),
                          (int)r.rssi, (unsigned)r.mfg_id,
                          (unsigned)r.service_count,
                          oui_org ? oui_org : "----",
                          mfg_org ? mfg_org : "----",
                          r.name);
        }
    }
}

void ScanEngine::onEvent(const Event& e) {
    switch (e.id) {
        case CMD_SCAN_START: {
            _in_scan_window = true;
            const uint32_t mode = g_settings.get(SKEY_SCAN_MODE);
            if (mode & 1u) _startBle();
            // (WiFi bit handled in a later phase)
            break;
        }
        case CMD_SCAN_STOP:
            _in_scan_window = false;
            _stopBle();
            break;
        case EV_SETTINGS_CHANGED:
            // SCAN_MODE toggle. Disable stops the radio immediately. Enable
            // only restarts scanning if we're currently in a scan window —
            // radio stays dark between windows regardless of the toggle.
            if (e.data.settings.mask & SMASK_SCAN) {
                const uint32_t mode = g_settings.get(SKEY_SCAN_MODE);
                const bool want_ble = (mode & 1u);
                if (!want_ble && _ble_scanning) {
                    _stopBle();
                } else if (want_ble && !_ble_scanning && _in_scan_window) {
                    _startBle();
                } else if (want_ble && _ble_scanning) {
                    // A scan-domain setting changed while the radio is live
                    // (e.g. the active/passive toggle). Restart to re-apply.
                    _stopBle();
                    _startBle();
                }
            }
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// BLE control
// ---------------------------------------------------------------------------

void ScanEngine::_startBle() {
    if (_ble_scanning) return;

    if (!_ble_initialized) {
        // Empty device name — we never advertise, only scan.
        NimBLEDevice::init("");
        // Maximum scanning sensitivity (the receiver). Doesn't affect TX.
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        _ble_initialized = true;
    }

    NimBLEScan* scan = NimBLEDevice::getScan();
    if (!scan) return;
    scan->setScanCallbacks(&s_callbacks, /*wantDuplicates*/ true);
    // Active scan sends scan requests to solicit scan responses (more names /
    // data) at the cost of TX power and being observable; passive only listens.
    // User-controlled via SKEY_SCAN_ACTIVE.
    scan->setActiveScan(g_settings.getBool(SKEY_SCAN_ACTIVE));
    // Radio always-on within the scan window — duty cycle is governed by the
    // outer SCAN_DURATION_S / SLEEP_DURATION_S pair, not by BLE-level params.
    scan->setInterval(100);
    scan->setWindow(100);

    // duration=0 → run forever (until stop()); is_continue=false → start fresh.
    if (!scan->start(0, false)) return;
    _ble_scanning = true;
}

void ScanEngine::_stopBle() {
    if (!_ble_scanning) return;
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan) scan->stop();
    _ble_scanning = false;
}
