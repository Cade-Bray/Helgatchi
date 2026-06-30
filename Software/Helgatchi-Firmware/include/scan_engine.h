#pragma once
#include "event_bus.h"
#include "scan_types.h"
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

    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

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

    bool      _ble_initialized = false;
    bool      _ble_scanning    = false;
    bool      _in_scan_window  = false;   // tracks CMD_SCAN_START / CMD_SCAN_STOP

    // Stats
    uint32_t  _cb_count   = 0;
    uint32_t  _q_overflow = 0;
    uint32_t  _pub_count  = 0;

    void _startBle();
    void _stopBle();
};

extern ScanEngine g_scan_engine;
