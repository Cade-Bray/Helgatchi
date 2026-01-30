
#pragma once

#include <Preferences.h>
#include <stdint.h>
#include <algorithm>
#include <vector>
#include <string>

#include "../core/CoreState.h"
#include "Models.h"

class Store {
public:
  void begin() {
    prefs_.begin("helgatchi", false);
  }

  void flush() {
    // Preferences commits immediately; keep for symmetry.
  }

  uint32_t incrementBootCount() {
    const uint32_t boot = prefs_.getUInt("boot", 0) + 1;
    prefs_.putUInt("boot", boot);
    return boot;
  }

  void loadSettings(Settings& out) {
    out.sleepTimeoutMs = prefs_.getUInt("slp_to", out.sleepTimeoutMs);
    out.wakeIntervalMs = prefs_.getUInt("wake_iv", out.wakeIntervalMs);
    out.scanBurstMs    = prefs_.getUInt("scan_b", out.scanBurstMs);

    out.alertScreen = prefs_.getBool("a_scr", out.alertScreen);
    out.alertLed    = prefs_.getBool("a_led", out.alertLed);
    out.alertVibe   = prefs_.getBool("a_vib", out.alertVibe);

    out.debugLevel = prefs_.getUChar("dbg_lvl", out.debugLevel);
    out.backlightLevel = prefs_.getUChar("bkl_lvl", out.backlightLevel);
    out.alertTrackingMs = prefs_.getUInt("a_trk_ms", out.alertTrackingMs);
  }

  void saveSettings(const Settings& s) {
    prefs_.putUInt("slp_to", s.sleepTimeoutMs);
    prefs_.putUInt("wake_iv", s.wakeIntervalMs);
    prefs_.putUInt("scan_b", s.scanBurstMs);

    prefs_.putBool("a_scr", s.alertScreen);
    prefs_.putBool("a_led", s.alertLed);
    prefs_.putBool("a_vib", s.alertVibe);

    prefs_.putUChar("dbg_lvl", s.debugLevel);
    prefs_.putUChar("bkl_lvl", s.backlightLevel);
    prefs_.putUInt("a_trk_ms", s.alertTrackingMs);
  }

  void clearSettings() {
    // Only clears settings keys (does NOT clear rules, BLE unique sketch, etc.)
    prefs_.remove("slp_to");
    prefs_.remove("wake_iv");
    prefs_.remove("scan_b");
    prefs_.remove("a_scr");
    prefs_.remove("a_led");
    prefs_.remove("a_vib");
    prefs_.remove("dbg");
    prefs_.remove("dbg_lvl");
    prefs_.remove("bkl_lvl");
    prefs_.remove("a_trk_ms");
  }

  void clearBootCount() {
    prefs_.remove("boot");
  }

  void clearBleUniqueSketch() {
    prefs_.remove("ble_hll");
  }

  void clearDisabledPacks() {
    prefs_.remove("pk_dis");
  }

  void clearDisabledPackRules() {
    prefs_.remove("pk_rdis");
  }

  void clearRules() {
    prefs_.remove("rules");
  }

  std::vector<OuiRule> loadRules() {
    // Blob format:
    //  - v1: [u16 version=1][u16 count][OuiRuleV1[count]] (OUI-only)
    //  - v2: [u16 version=2][u16 count][OuiRuleV2[count]] (OUI or MAC via addr48; no pack field)
    //  - v3: [u16 version=3][u16 count][OuiRuleV3[count]] (OUI or MAC via addr48; includes pack)
    //  - v4: [u16 version=4][u16 count][OuiRuleV4[count]] (adds nameContains)
    //  - v5: [u16 version=5][u16 count][OuiRuleV5[count]] (adds service UUID matching)
		//  - v6: [u16 version=6][u16 count][OuiRule[count]]   (adds MSD company ID matching)
    const size_t len = prefs_.getBytesLength("rules");
    if (len < (sizeof(uint16_t) * 2)) return {};

    std::vector<uint8_t> buf(len);
    const size_t got = prefs_.getBytes("rules", buf.data(), buf.size());
    if (got != len) return {};

    const uint16_t version = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    const uint16_t count = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);

    if (version == 1) {
      struct OuiRuleV1 {
        uint32_t id;
        bool enabled;
        uint32_t oui24;
        char label[16];
      };

      const size_t expected = (sizeof(uint16_t) * 2) + ((size_t)count * sizeof(OuiRuleV1));
      if (expected != len) return {};

      std::vector<OuiRuleV1> v1;
      v1.resize(count);
      memcpy(v1.data(), buf.data() + 4, (size_t)count * sizeof(OuiRuleV1));

      std::vector<OuiRule> rules;
      rules.resize(count);
      for (size_t i = 0; i < count; i++) {
        rules[i].id = v1[i].id;
        rules[i].enabled = v1[i].enabled;
        rules[i].oui24 = v1[i].oui24 & 0xFFFFFFu;
        rules[i].addr48 = 0;
        strncpy(rules[i].pack, "USER", sizeof(rules[i].pack) - 1);
        rules[i].pack[sizeof(rules[i].pack) - 1] = 0;
        memcpy(rules[i].label, v1[i].label, sizeof(rules[i].label));
        rules[i].label[sizeof(rules[i].label) - 1] = 0;
      }
      return rules;
    }

    if (version == 2) {
      struct OuiRuleV2 {
        uint32_t id;
        bool enabled;
        uint32_t oui24;
        uint64_t addr48;
        char label[16];
      };

      const size_t expected = (sizeof(uint16_t) * 2) + ((size_t)count * sizeof(OuiRuleV2));
      if (expected != len) return {};

      std::vector<OuiRuleV2> v2;
      v2.resize(count);
      memcpy(v2.data(), buf.data() + 4, (size_t)count * sizeof(OuiRuleV2));

      std::vector<OuiRule> rules;
      rules.resize(count);
      for (size_t i = 0; i < count; i++) {
        rules[i].id = v2[i].id;
        rules[i].enabled = v2[i].enabled;
        rules[i].oui24 = v2[i].oui24 & 0xFFFFFFu;
        rules[i].addr48 = v2[i].addr48 & 0xFFFFFFFFFFFFULL;
        strncpy(rules[i].pack, "USER", sizeof(rules[i].pack) - 1);
        rules[i].pack[sizeof(rules[i].pack) - 1] = 0;
        memcpy(rules[i].label, v2[i].label, sizeof(rules[i].label));
        rules[i].label[sizeof(rules[i].label) - 1] = 0;
      }
      return rules;
    }

    if (version == 3) {
      struct OuiRuleV3 {
        uint32_t id;
        bool enabled;
        uint32_t oui24;
        uint64_t addr48;
        char pack[16];
        char label[16];
      };

      const size_t expected = (sizeof(uint16_t) * 2) + ((size_t)count * sizeof(OuiRuleV3));
      if (expected != len) return {};

      std::vector<OuiRuleV3> v3;
      v3.resize(count);
      memcpy(v3.data(), buf.data() + 4, (size_t)count * sizeof(OuiRuleV3));

      std::vector<OuiRule> rules;
      rules.resize(count);
      for (size_t i = 0; i < count; i++) {
        rules[i].id = v3[i].id;
        rules[i].enabled = v3[i].enabled;
        rules[i].oui24 = v3[i].oui24 & 0xFFFFFFu;
        rules[i].addr48 = v3[i].addr48 & 0xFFFFFFFFFFFFULL;
        rules[i].nameContains[0] = 0;
        memcpy(rules[i].pack, v3[i].pack, sizeof(rules[i].pack));
        rules[i].pack[sizeof(rules[i].pack) - 1] = 0;
        memcpy(rules[i].label, v3[i].label, sizeof(rules[i].label));
        rules[i].label[sizeof(rules[i].label) - 1] = 0;
      }
      return rules;
    }

    if (version == 4) {
      struct OuiRuleV4 {
        uint32_t id;
        bool enabled;
        uint32_t oui24;
        uint64_t addr48;
        char nameContains[20];
        char pack[16];
        char label[16];
      };

      const size_t expected = (sizeof(uint16_t) * 2) + ((size_t)count * sizeof(OuiRuleV4));
      if (expected != len) return {};

      std::vector<OuiRuleV4> v4;
      v4.resize(count);
      memcpy(v4.data(), buf.data() + 4, (size_t)count * sizeof(OuiRuleV4));

      std::vector<OuiRule> rules;
      rules.resize(count);
      for (size_t i = 0; i < count; i++) {
        rules[i].id = v4[i].id;
        rules[i].enabled = v4[i].enabled;
        rules[i].oui24 = v4[i].oui24 & 0xFFFFFFu;
        rules[i].addr48 = v4[i].addr48 & 0xFFFFFFFFFFFFULL;
			rules[i].hasCompanyId = false;
			rules[i].companyId = 0;
        memcpy(rules[i].nameContains, v4[i].nameContains, sizeof(rules[i].nameContains));
        rules[i].nameContains[sizeof(rules[i].nameContains) - 1] = 0;
        rules[i].hasServiceUuid = false;
        memset(rules[i].serviceUuid128, 0, sizeof(rules[i].serviceUuid128));
        memcpy(rules[i].pack, v4[i].pack, sizeof(rules[i].pack));
        rules[i].pack[sizeof(rules[i].pack) - 1] = 0;
        memcpy(rules[i].label, v4[i].label, sizeof(rules[i].label));
        rules[i].label[sizeof(rules[i].label) - 1] = 0;
      }
      return rules;
    }

    if (version == 5) {
      struct OuiRuleV5 {
        uint32_t id;
        bool enabled;
        uint32_t oui24;
        uint64_t addr48;
        char nameContains[20];
        bool hasServiceUuid;
        uint8_t serviceUuid128[16];
        char pack[16];
        char label[16];
      };

      const size_t expected = (sizeof(uint16_t) * 2) + ((size_t)count * sizeof(OuiRuleV5));
      if (expected != len) return {};

      std::vector<OuiRuleV5> v5;
      v5.resize(count);
      memcpy(v5.data(), buf.data() + 4, (size_t)count * sizeof(OuiRuleV5));

      std::vector<OuiRule> rules;
      rules.resize(count);
      for (size_t i = 0; i < count; i++) {
        rules[i].id = v5[i].id;
        rules[i].enabled = v5[i].enabled;
        rules[i].oui24 = v5[i].oui24 & 0xFFFFFFu;
        rules[i].addr48 = v5[i].addr48 & 0xFFFFFFFFFFFFULL;
        rules[i].hasCompanyId = false;
        rules[i].companyId = 0;
        memcpy(rules[i].nameContains, v5[i].nameContains, sizeof(rules[i].nameContains));
        rules[i].nameContains[sizeof(rules[i].nameContains) - 1] = 0;
        rules[i].hasServiceUuid = v5[i].hasServiceUuid;
        memcpy(rules[i].serviceUuid128, v5[i].serviceUuid128, sizeof(rules[i].serviceUuid128));
        memcpy(rules[i].pack, v5[i].pack, sizeof(rules[i].pack));
        rules[i].pack[sizeof(rules[i].pack) - 1] = 0;
        memcpy(rules[i].label, v5[i].label, sizeof(rules[i].label));
        rules[i].label[sizeof(rules[i].label) - 1] = 0;
      }
      return rules;
    }

    if (version == 6) {
      const size_t expected = (sizeof(uint16_t) * 2) + ((size_t)count * sizeof(OuiRule));
      if (expected != len) return {};

      std::vector<OuiRule> rules;
      rules.resize(count);
      memcpy(rules.data(), buf.data() + 4, (size_t)count * sizeof(OuiRule));

      for (auto& r : rules) {
        r.oui24 &= 0xFFFFFFu;
        r.addr48 &= 0xFFFFFFFFFFFFULL;
        r.companyId &= 0xFFFFu;
        r.nameContains[sizeof(r.nameContains) - 1] = 0;
        r.pack[sizeof(r.pack) - 1] = 0;
        r.label[sizeof(r.label) - 1] = 0;
      }
      return rules;
    }

    return {};
  }

  void saveRules(const std::vector<OuiRule>& rules) {
		const uint16_t version = 6;
    const uint16_t count = (rules.size() > 0xFFFFu) ? 0xFFFFu : (uint16_t)rules.size();
    const size_t len = (sizeof(uint16_t) * 2) + ((size_t)count * sizeof(OuiRule));

    std::vector<uint8_t> buf;
    buf.resize(len);

    buf[0] = (uint8_t)(version & 0xFF);
    buf[1] = (uint8_t)((version >> 8) & 0xFF);
    buf[2] = (uint8_t)(count & 0xFF);
    buf[3] = (uint8_t)((count >> 8) & 0xFF);

    if (count > 0) {
      // Copy a sanitized POD array.
      std::vector<OuiRule> tmp;
      tmp.reserve(count);
      for (uint16_t i = 0; i < count; i++) {
        OuiRule r = rules[i];
        r.oui24 &= 0xFFFFFFu;
        r.addr48 &= 0xFFFFFFFFFFFFULL;
        r.nameContains[sizeof(r.nameContains) - 1] = 0;
        // serviceUuid128 is binary; keep as-is.
			r.companyId &= 0xFFFFu;
        r.pack[sizeof(r.pack) - 1] = 0;
        r.label[sizeof(r.label) - 1] = 0;
        tmp.push_back(r);
      }
      memcpy(buf.data() + 4, tmp.data(), (size_t)count * sizeof(OuiRule));
    }

    prefs_.putBytes("rules", buf.data(), buf.size());
  }

  bool loadBleUniqueSketch(uint8_t* out, size_t len) {
    if (!out || len == 0) return false;
    const size_t stored = prefs_.getBytesLength("ble_hll");
    if (stored != len) {
      memset(out, 0, len);
      return false;
    }
    const size_t got = prefs_.getBytes("ble_hll", out, len);
    if (got != len) {
      memset(out, 0, len);
      return false;
    }
    return true;
  }

  void saveBleUniqueSketch(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    prefs_.putBytes("ble_hll", data, len);
  }

  bool loadSleepScanStats(SleepScanStats& out) {
    // Blob format:
    //  - v1: [u16 version=1][SleepScanStatsV1]
    struct SleepScanStatsV1 {
      uint32_t bursts;
      uint32_t bleHits;
      uint32_t wifiHits;
      uint32_t lastBleOui24;
      int8_t lastBleRssi;
      char lastBleName[20];
      uint32_t lastWifiOui24;
      int8_t lastWifiRssi;
    };

    const size_t len = prefs_.getBytesLength("slp_sum");
    const size_t expected = (sizeof(uint16_t) * 1) + sizeof(SleepScanStatsV1);
    if (len != expected) {
      out = SleepScanStats{};
      return false;
    }

    uint8_t buf[sizeof(uint16_t) + sizeof(SleepScanStatsV1)];
    const size_t got = prefs_.getBytes("slp_sum", buf, sizeof(buf));
    if (got != sizeof(buf)) {
      out = SleepScanStats{};
      return false;
    }

    const uint16_t version = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    if (version != 1) {
      out = SleepScanStats{};
      return false;
    }

    SleepScanStatsV1 v1{};
    memcpy(&v1, buf + 2, sizeof(v1));

    out.bursts = v1.bursts;
    out.bleHits = v1.bleHits;
    out.wifiHits = v1.wifiHits;
    out.lastBleOui24 = v1.lastBleOui24 & 0xFFFFFFu;
    out.lastBleRssi = v1.lastBleRssi;
    memcpy(out.lastBleName, v1.lastBleName, sizeof(out.lastBleName));
    out.lastBleName[sizeof(out.lastBleName) - 1] = 0;
    out.lastWifiOui24 = v1.lastWifiOui24 & 0xFFFFFFu;
    out.lastWifiRssi = v1.lastWifiRssi;
    return true;
  }

  void saveSleepScanStats(const SleepScanStats& s) {
    struct SleepScanStatsV1 {
      uint32_t bursts;
      uint32_t bleHits;
      uint32_t wifiHits;
      uint32_t lastBleOui24;
      int8_t lastBleRssi;
      char lastBleName[20];
      uint32_t lastWifiOui24;
      int8_t lastWifiRssi;
    };

    const uint16_t version = 1;
    SleepScanStatsV1 v1{};
    v1.bursts = s.bursts;
    v1.bleHits = s.bleHits;
    v1.wifiHits = s.wifiHits;
    v1.lastBleOui24 = s.lastBleOui24 & 0xFFFFFFu;
    v1.lastBleRssi = s.lastBleRssi;
    memcpy(v1.lastBleName, s.lastBleName, sizeof(v1.lastBleName));
    v1.lastBleName[sizeof(v1.lastBleName) - 1] = 0;
    v1.lastWifiOui24 = s.lastWifiOui24 & 0xFFFFFFu;
    v1.lastWifiRssi = s.lastWifiRssi;

    uint8_t buf[sizeof(uint16_t) + sizeof(SleepScanStatsV1)];
    buf[0] = (uint8_t)(version & 0xFF);
    buf[1] = (uint8_t)((version >> 8) & 0xFF);
    memcpy(buf + 2, &v1, sizeof(v1));
    prefs_.putBytes("slp_sum", buf, sizeof(buf));
  }

  void clearSleepScanStats() {
    prefs_.remove("slp_sum");
  }

  // Per-pack per-rule overrides (disabled rules within enabled packs).
  // Blob format:
  //  - v1: [u16 version=1][u16 count][Entry[count]]
  //        Entry := [char pack[16]][u64 sig]
  std::vector<PackRuleOverride> loadDisabledPackRules() {
    const size_t len = prefs_.getBytesLength("pk_rdis");
    if (len < (sizeof(uint16_t) * 2)) return {};

    std::vector<uint8_t> buf(len);
    const size_t got = prefs_.getBytes("pk_rdis", buf.data(), buf.size());
    if (got != len) return {};

    const uint16_t version = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    const uint16_t count = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    if (version != 1) return {};
    const size_t expected = (sizeof(uint16_t) * 2) + ((size_t)count * (16u + 8u));
    if (expected != len) return {};

    std::vector<PackRuleOverride> out;
    out.resize(count);

    const uint8_t* p = buf.data() + 4;
    for (uint16_t i = 0; i < count; i++) {
      PackRuleOverride e{};
      memcpy(e.pack, p + ((size_t)i * (16u + 8u)), 16u);
      e.pack[sizeof(e.pack) - 1] = 0;
      const uint8_t* q = p + ((size_t)i * (16u + 8u)) + 16u;
      uint64_t sig = 0;
      for (uint8_t b = 0; b < 8; b++) sig |= ((uint64_t)q[b] << (8u * (unsigned)b));
      e.sig = sig;
      out[i] = e;
    }
    return out;
  }

  void saveDisabledPackRules(const std::vector<PackRuleOverride>& rules) {
    const uint16_t version = 1;
    const uint16_t count = (rules.size() > 0xFFFFu) ? 0xFFFFu : (uint16_t)rules.size();
    const size_t len = (sizeof(uint16_t) * 2) + ((size_t)count * (16u + 8u));
    std::vector<uint8_t> buf(len);

    buf[0] = (uint8_t)(version & 0xFF);
    buf[1] = (uint8_t)((version >> 8) & 0xFF);
    buf[2] = (uint8_t)(count & 0xFF);
    buf[3] = (uint8_t)((count >> 8) & 0xFF);

    uint8_t* p = buf.data() + 4;
    for (uint16_t i = 0; i < count; i++) {
      char pack[16] = {0};
      if (rules[i].pack[0]) {
        strncpy(pack, rules[i].pack, sizeof(pack) - 1);
        pack[sizeof(pack) - 1] = 0;
      }
      memcpy(p + ((size_t)i * (16u + 8u)), pack, 16u);
      const uint64_t sig = rules[i].sig;
      uint8_t* q = p + ((size_t)i * (16u + 8u)) + 16u;
      for (uint8_t b = 0; b < 8; b++) q[b] = (uint8_t)((sig >> (8u * (unsigned)b)) & 0xFFu);
    }

    prefs_.putBytes("pk_rdis", buf.data(), buf.size());
  }

  void clearDisabledPackRulesForPack(const char* pack) {
    if (!pack || !*pack) return;
    std::vector<PackRuleOverride> v = loadDisabledPackRules();
    const size_t before = v.size();
    v.erase(
        std::remove_if(v.begin(), v.end(), [&](const PackRuleOverride& e) {
          return strncmp(e.pack, pack, sizeof(e.pack)) == 0;
        }),
        v.end());
    if (v.size() != before) saveDisabledPackRules(v);
  }

  void setPackRuleDisabled(const char* pack, uint64_t sig, bool disabled) {
    if (!pack || !*pack || sig == 0) return;
    std::vector<PackRuleOverride> v = loadDisabledPackRules();

    auto match = [&](const PackRuleOverride& e) {
      return (e.sig == sig) && (strncmp(e.pack, pack, sizeof(e.pack)) == 0);
    };

    const auto it = std::find_if(v.begin(), v.end(), match);
    if (disabled) {
      if (it == v.end()) {
        PackRuleOverride e{};
        strncpy(e.pack, pack, sizeof(e.pack) - 1);
        e.pack[sizeof(e.pack) - 1] = 0;
        e.sig = sig;
        v.push_back(e);
        saveDisabledPackRules(v);
      }
    } else {
      if (it != v.end()) {
        v.erase(it);
        saveDisabledPackRules(v);
      }
    }
  }

  // Disabled filesystem rule packs (by pack name). This lets the UI toggle packs on/off
  // without rewriting /rules/*.json or persisting pack rules into NVS.
  std::vector<std::string> loadDisabledPacks() {
    // Blob format:
    //  - v1: [u16 version=1][u16 count][char name[16] * count]
    const size_t len = prefs_.getBytesLength("pk_dis");
    if (len < (sizeof(uint16_t) * 2)) return {};

    std::vector<uint8_t> buf(len);
    const size_t got = prefs_.getBytes("pk_dis", buf.data(), buf.size());
    if (got != len) return {};

    const uint16_t version = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    const uint16_t count = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
    if (version != 1) return {};
    const size_t expected = (sizeof(uint16_t) * 2) + ((size_t)count * 16u);
    if (expected != len) return {};

    std::vector<std::string> out;
    out.reserve(count);
    const uint8_t* p = buf.data() + 4;
    for (uint16_t i = 0; i < count; i++) {
      char name[17];
      memcpy(name, p + ((size_t)i * 16u), 16u);
      name[16] = 0;
      // Trim at first NUL
      name[16] = 0;
      out.emplace_back(std::string(name));
    }
    return out;
  }

  void saveDisabledPacks(const std::vector<std::string>& packs) {
    const uint16_t version = 1;
    const uint16_t count = (packs.size() > 0xFFFFu) ? 0xFFFFu : (uint16_t)packs.size();
    const size_t len = (sizeof(uint16_t) * 2) + ((size_t)count * 16u);
    std::vector<uint8_t> buf(len);

    buf[0] = (uint8_t)(version & 0xFF);
    buf[1] = (uint8_t)((version >> 8) & 0xFF);
    buf[2] = (uint8_t)(count & 0xFF);
    buf[3] = (uint8_t)((count >> 8) & 0xFF);

    uint8_t* p = buf.data() + 4;
    for (uint16_t i = 0; i < count; i++) {
      char name[16] = {0};
      if (!packs[i].empty()) {
        // Truncate to fit.
        strncpy(name, packs[i].c_str(), sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
      }
      memcpy(p + ((size_t)i * 16u), name, 16u);
    }

    prefs_.putBytes("pk_dis", buf.data(), buf.size());
  }

private:
  Preferences prefs_;
};
