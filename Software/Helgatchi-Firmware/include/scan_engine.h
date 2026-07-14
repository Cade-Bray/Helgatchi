#pragma once
#include "event_bus.h"
#include "scan_types.h"
#include "admin_types.h"
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// Scan engine — owns the BLE (and later, WiFi) radios and feeds ScanResults
// into ScanService. NimBLE callbacks run on the BLE host task; results are
// marshalled to the main loop via a FreeRTOS queue, so ScanService.publish()
// only ever runs from one thread.
//
// Triggered by:
//   CMD_SCAN_START — PowerManager opens a scan window
//   CMD_SCAN_STOP  — scan window closes
//
// SKEY_SCAN_MODE bit 0 gates BLE (bit 1 reserved for WiFi). Settings
// changes that touch SMASK_SCAN re-apply parameters on the next start.
// ---------------------------------------------------------------------------

class ScanEngine : public IEventHandler {
public:
    // Queue depth — how many BLE callbacks can land before tick() drains.
    // 50 advertisements/sec is typical; 64 gives just over a second of slack.
    static constexpr size_t QUEUE_DEPTH = 64;
    // Admin command frames are rare; a small queue drop-on-full is benign since
    // frames retransmit throughout the sender's broadcast burst.
    static constexpr size_t ADMIN_QUEUE_DEPTH = 8;

    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Idempotent one-time NimBLE init. Owned here (the only NimBLEDevice::init
    // in the firmware); AdminService calls this before it advertises so scan and
    // advertise share one init. Safe to call repeatedly. May block ~tens of ms
    // on the very first call while the host syncs — keep it on the main loop.
    void ensureBle();

    // Pop one received admin command frame (filled by the NimBLE callback).
    // Returns false when the admin queue is empty. Drained by AdminService::tick.
    bool popAdminFrame(AdminFrame& out);

    // Suspend all scanning (BLE now, WiFi when it lands) while true, so an admin
    // controller can dedicate the radio to advertising. Stops any live scan
    // immediately; releasing resumes it if we're inside a scan window. AdminService
    // holds this true only while it is broadcasting a command.
    void setScanInhibited(bool inhibit);
    bool scanInhibited() const { return _scan_inhibited; }

    // Stats counters — incremented in the BLE callback (queue full) and tick
    // (drained). For diagnostics via a future serial command.
    uint32_t callbacks()      const { return _cb_count; }
    uint32_t queueOverflows() const { return _q_overflow; }
    uint32_t published()      const { return _pub_count; }
    bool     bleActive()      const { return _ble_scanning; }

    // Number of ScanResults sitting in the BLE-host→main-loop queue. Used
    // by PowerManager to decide when post-scan-stop drain is complete.
    size_t   queueDepth()     const;

private:
    EventBus* _bus = nullptr;
    void*     _queue = nullptr;       // QueueHandle_t, opaque to keep FreeRTOS out of the header
    void*     _admin_queue = nullptr; // QueueHandle_t for received admin command frames

    bool      _ble_initialized = false;
    bool      _ble_scanning    = false;
    bool      _in_scan_window  = false;   // tracks CMD_SCAN_START / CMD_SCAN_STOP
    bool      _scan_inhibited  = false;   // admin broadcast owns the radio while true

    // Stats
    uint32_t  _cb_count   = 0;
    uint32_t  _q_overflow = 0;
    uint32_t  _pub_count  = 0;

    void _startBle();
    void _stopBle();

    // Post EV_SCAN_STATE_CHANGED{domain, active}. Called on every radio
    // start/stop so the UI (and any listener) can track per-domain scan
    // state. WiFi will call this with SCAN_WIFI once its radio path lands.
    void _emitScanState(uint8_t domain, bool active);
};

extern ScanEngine g_scan_engine;
