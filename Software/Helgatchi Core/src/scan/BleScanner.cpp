#include "BleScanner.h"

#include <Arduino.h>

#include <string.h>

#include <NimBLEDevice.h>
#include <stdio.h>

#include "../core/Config.h"

namespace {

static bool s_debugPerformance = false;

static uint32_t parseOui24_(const std::string& mac) {
  unsigned int b0 = 0, b1 = 0, b2 = 0;
  if (sscanf(mac.c_str(), "%2x:%2x:%2x", &b0, &b1, &b2) == 3) {
    return ((uint32_t)(b0 & 0xFFu) << 16) | ((uint32_t)(b1 & 0xFFu) << 8) | ((uint32_t)(b2 & 0xFFu));
  }
  return 0;
}

static uint64_t parseAddr48_(const std::string& mac) {
  unsigned int b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0;
  if (sscanf(mac.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &b0, &b1, &b2, &b3, &b4, &b5) == 6) {
    return ((uint64_t)(b0 & 0xFFu) << 40) | ((uint64_t)(b1 & 0xFFu) << 32) | ((uint64_t)(b2 & 0xFFu) << 24) |
           ((uint64_t)(b3 & 0xFFu) << 16) | ((uint64_t)(b4 & 0xFFu) << 8) | ((uint64_t)(b5 & 0xFFu));
  }
  return 0;
}

static int hexNibble_(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static bool parseUuid128_(const std::string& s, uint8_t out[16]) {
  if (!out) return false;
  // Accept canonical UUID strings (with dashes) or raw 32 hex digits.
  char hex[33];
  int n = 0;
  for (char c : s) {
    const int h = hexNibble_(c);
    if (h >= 0) {
      if (n >= 32) break;
      hex[n++] = c;
    }
  }
  if (n != 32) return false;
  hex[32] = 0;

  for (int i = 0; i < 16; i++) {
    const int hi = hexNibble_(hex[i * 2]);
    const int lo = hexNibble_(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

struct PendingBleSighting {
  uint32_t tsMs;
  uint64_t addr48;
  uint32_t oui24;
  int8_t rssi;
  char name[20];
	uint8_t hasMsdCompanyId;
	uint16_t msdCompanyId;
	uint8_t serviceUuidCount;
	uint8_t serviceUuids[BleSightingEvent::kMaxServiceUuids][16];
};

// Single producer (NimBLE host task) -> single consumer (Arduino loop).
// We protect indices with a critical section to avoid tearing.
static constexpr size_t kQueueCap = 32;
static PendingBleSighting s_queue[kQueueCap];
static volatile size_t s_head = 0;
static volatile size_t s_tail = 0;
static uint32_t s_dropped = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void enqueue_(const PendingBleSighting& v) {
  portENTER_CRITICAL(&s_mux);
  const size_t next = (s_head + 1) % kQueueCap;
  if (next == s_tail) {
    // queue full: drop oldest
    s_tail = (s_tail + 1) % kQueueCap;
    s_dropped++;
  }
  s_queue[s_head] = v;
  s_head = next;
  portEXIT_CRITICAL(&s_mux);
}

static bool tryDequeue_(PendingBleSighting& out) {
  bool ok = false;
  portENTER_CRITICAL(&s_mux);
  if (s_tail != s_head) {
    out = s_queue[s_tail];
    s_tail = (s_tail + 1) % kQueueCap;
    ok = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return ok;
}

class ScanCallbacks : public NimBLEScanCallbacks {
public:
  explicit ScanCallbacks(BleScanner* owner) : owner_(owner) {}

  void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
    uint32_t _t0 = 0;
    if (s_debugPerformance) _t0 = micros();
    
    if (!owner_ || !owner_->running()) {
      if (s_debugPerformance && _t0 > 0) {
        Serial.printf("[PERF] ble_callback_early_exit: %lu us\n", (unsigned long)(micros() - _t0));
      }
      return;
    }
    if (!advertisedDevice) {
      if (s_debugPerformance && _t0 > 0) {
        Serial.printf("[PERF] ble_callback_null_device: %lu us\n", (unsigned long)(micros() - _t0));
      }
      return;
    }

    PendingBleSighting s{};
    s.tsMs = millis();
    s.rssi = (int8_t)advertisedDevice->getRSSI();
    s.name[0] = 0;
		s.hasMsdCompanyId = 0;
		s.msdCompanyId = 0;
    s.serviceUuidCount = 0;
    memset(s.serviceUuids, 0, sizeof(s.serviceUuids));

    uint32_t _t_addr = s_debugPerformance ? micros() : 0;
    const std::string mac = advertisedDevice->getAddress().toString();
    s.addr48 = parseAddr48_(mac);

    // Derive 24-bit prefix from the parsed 48-bit address (avoids any string-order ambiguity).
    // NOTE: If the BLE stack provides a random/private address, this prefix will NOT be a vendor OUI.
    if (s.addr48 != 0) {
      s.oui24 = (uint32_t)((s.addr48 >> 24) & 0xFFFFFFu);
    } else {
      s.oui24 = parseOui24_(mac);
    }
    if (s_debugPerformance && _t_addr > 0) {
      Serial.printf("[PERF] ble_parse_addr: %lu us\n", (unsigned long)(micros() - _t_addr));
    }

    if (advertisedDevice->haveName()) {
      uint32_t _t_name = s_debugPerformance ? micros() : 0;
      const std::string n = advertisedDevice->getName();
      strncpy(s.name, n.c_str(), sizeof(s.name) - 1);
      s.name[sizeof(s.name) - 1] = 0;
      if (s_debugPerformance && _t_name > 0) {
        Serial.printf("[PERF] ble_parse_name: %lu us\n", (unsigned long)(micros() - _t_name));
      }
    }

    if (advertisedDevice->haveManufacturerData()) {
      uint32_t _t_mfg = s_debugPerformance ? micros() : 0;
      const std::string md = advertisedDevice->getManufacturerData();
      if (md.size() >= 2) {
        const uint8_t b0 = (uint8_t)md[0];
        const uint8_t b1 = (uint8_t)md[1];
        s.msdCompanyId = (uint16_t)((uint16_t)b0 | ((uint16_t)b1 << 8));
        s.hasMsdCompanyId = 1;
      }
      if (s_debugPerformance && _t_mfg > 0) {
        Serial.printf("[PERF] ble_parse_mfg: %lu us\n", (unsigned long)(micros() - _t_mfg));
      }
    }

    if (advertisedDevice->haveServiceUUID()) {
      uint32_t _t_uuids = s_debugPerformance ? micros() : 0;
      const int count = advertisedDevice->getServiceUUIDCount();
      for (int i = 0; i < count && s.serviceUuidCount < BleSightingEvent::kMaxServiceUuids; i++) {
        const NimBLEUUID uuid = advertisedDevice->getServiceUUID(i);
        const std::string u = uuid.toString();
        uint8_t bytes[16];
        if (parseUuid128_(u, bytes)) {
          memcpy(s.serviceUuids[s.serviceUuidCount], bytes, 16);
          s.serviceUuidCount++;
        }
      }
      if (s_debugPerformance && _t_uuids > 0) {
        Serial.printf("[PERF] ble_parse_uuids: %lu us\n", (unsigned long)(micros() - _t_uuids));
      }
    }

    enqueue_(s);
    
    if (s_debugPerformance && _t0 > 0) {
      Serial.printf("[PERF] ble_callback_total: %lu us\n", (unsigned long)(micros() - _t0));
    }
  }

private:
  BleScanner* owner_;
};

static NimBLEScan* s_scan = nullptr;
static ScanCallbacks* s_callbacks = nullptr;
static bool s_initialized = false;

static void ensureInit_(BleScanner* owner) {
  if (s_initialized) return;

  NimBLEDevice::init("");
  // Optional: a reasonable default TX power for scanning/debug.
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);

  s_scan = NimBLEDevice::getScan();
  s_callbacks = new ScanCallbacks(owner);

  s_scan->setScanCallbacks(s_callbacks, /*wantDuplicates=*/false);
  s_scan->setActiveScan(true);
  s_scan->setInterval(80); // ms
  s_scan->setWindow(40);   // ms
  s_scan->setDuplicateFilter(1);
  s_scan->setMaxResults(0); // keep results minimal; we use callbacks

  s_initialized = true;
}

} // namespace

void BleScanner::begin() {
  ensureInit_(this);
}

void BleScanner::start() {
  ensureInit_(this);
  running_ = true;

  if (s_scan && !s_scan->isScanning()) {
    // duration=0 => continuous (NimBLE-Arduino behavior)
    s_scan->start(0, /*isContinue=*/true, /*restart=*/true);
  }
}

void BleScanner::stop() {
  running_ = false;
  if (s_scan && s_scan->isScanning()) {
    s_scan->stop();
  }
}

void BleScanner::poll(EventBus& bus) {
  (void)bus;

  // Drain a limited number per loop to avoid starving UI/event handling.
  PendingBleSighting s{};
  uint8_t drained = 0;
  while (drained < 8 && tryDequeue_(s)) {
    Event e{};
    e.type = EventType::BleSighting;
    e.ble.tsMs = s.tsMs;
    e.ble.addr48 = s.addr48;
    e.ble.oui24 = s.oui24;
    e.ble.rssi = s.rssi;
    memset(e.ble.name, 0, sizeof(e.ble.name));
    memcpy(e.ble.name, s.name, sizeof(e.ble.name));
		e.ble.hasMsdCompanyId = s.hasMsdCompanyId;
		e.ble.msdCompanyId = s.msdCompanyId;
    e.ble.serviceUuidCount = s.serviceUuidCount;
    memset(e.ble.serviceUuids, 0, sizeof(e.ble.serviceUuids));
    memcpy(e.ble.serviceUuids, s.serviceUuids, sizeof(e.ble.serviceUuids));
    bus.push(e);
    drained++;
  }
}

void BleScanner::setDebugPerformance(bool enabled) {
  s_debugPerformance = enabled;
}

bool BleScanner::debugPerformance() {
  return s_debugPerformance;
}
