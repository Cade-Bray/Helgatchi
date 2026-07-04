#include "rules_service.h"
#include "scan_service.h"
#include "vendor_lookup.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

RulesService g_rules;

// Disk paths
static constexpr const char* DIR_RULES         = "/rules";
static constexpr const char* DIR_FACTORY       = "/rules/factory";
static constexpr const char* DIR_USER          = "/rules/user";

// NVS namespace + key for the disabled-rules overlay. Single blob of
// newline-separated rule names; keeps key count fixed at 1 regardless of
// how many rules are toggled.
static constexpr const char* NVS_NAMESPACE     = "rules";
static constexpr const char* NVS_KEY_DISABLED  = "disabled";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Allocate string in PSRAM and copy. nullptr on OOM (shouldn't happen with
// 7+MB free at runtime; caller still checks).
static char* _psram_strdup(const char* s) {
    const size_t n = strlen(s) + 1;
    char* out = (char*)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out) memcpy(out, s, n);
    return out;
}

// Case-insensitive substring — folds both sides on the fly. Slightly slower
// than pre-lowercasing the needle, but lets us store the user's original
// casing in the rule and emit it verbatim when serializing back to JSON.
static const char* _icontains(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return haystack;
    const size_t nlen = strlen(needle);
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        for (; i < nlen; i++) {
            char a = p[i]; char b = needle[i];
            if (!a) return nullptr;
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (i == nlen) return p;
    }
    return nullptr;
}

// Parse "AA:BB:CC" into the low 24 bits of a uint32. Accepts hex with or
// without colons. Returns false on malformed input.
static bool _parseOuiPrefix(const char* s, uint32_t* out) {
    if (!s || !out) return false;
    unsigned int b[3] = {0};
    int n = sscanf(s, "%2x:%2x:%2x", &b[0], &b[1], &b[2]);
    if (n != 3) return false;
    *out = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | (uint32_t)b[2];
    return true;
}

static bool _parseMac6(const char* s, uint8_t out[6]) {
    if (!s) return false;
    unsigned int b[6] = {0};
    int n = sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
    return true;
}

static bool _parseMfgId(const char* s, uint16_t* out) {
    if (!s || !out) return false;
    char* end = nullptr;
    unsigned long v = strtoul(s, &end, 0);   // 0x prefix auto-detected
    if (end == s || v > 0xFFFF) return false;
    *out = (uint16_t)v;
    return true;
}

// Parses a service UUID — accepts short forms ("180F", "0x180F") promoted
// via the BLE base UUID, or a full 128-bit form ("0000180F-0000-1000-8000-
// 00805F9B34FB"). Output is stored in 16-byte little-endian wire order so
// it byte-compares to scan_uuids directly.
static bool _parseServiceUuid(const char* s, uint8_t out[16]) {
    if (!s) return false;

    // Short form: <=4 hex chars, optionally 0x-prefixed.
    if (strchr(s, '-') == nullptr) {
        char* end = nullptr;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s || v > 0xFFFFFFFF) return false;
        // BLE base UUID: 00000000-0000-1000-8000-00805F9B34FB
        // Bytes 12..15 (little-endian) hold the short value (big-endian).
        static const uint8_t base[16] = {
            0xFB, 0x9B, 0x34, 0x5F, 0x80, 0x00, 0x00, 0x80,
            0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        memcpy(out, base, 16);
        out[12] = (uint8_t)( v        & 0xFF);
        out[13] = (uint8_t)((v >>  8) & 0xFF);
        out[14] = (uint8_t)((v >> 16) & 0xFF);
        out[15] = (uint8_t)((v >> 24) & 0xFF);
        return true;
    }

    // Full UUID form. Read big-endian then byte-reverse to wire order.
    unsigned int u[16] = {0};
    int n = sscanf(s,
        "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
        &u[15], &u[14], &u[13], &u[12],
        &u[11], &u[10], &u[ 9], &u[ 8],
        &u[ 7], &u[ 6], &u[ 5], &u[ 4],
        &u[ 3], &u[ 2], &u[ 1], &u[ 0]);
    if (n != 16) return false;
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)u[i];
    return true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void RulesService::begin(EventBus& bus) {
    _bus = &bus;
    // Start draining the ring from "right now" — old scan entries from before
    // we existed aren't actionable. ScanService.publish() since boot will be
    // visible on the next tick.
    _ring_read_pos = g_scan.writePos();

    // LittleFS must already be mounted by main.cpp.
    _ensureUserDir();
    _loadDir(DIR_FACTORY, true);
    _loadDir(DIR_USER,    false);
    _applyEnabledOverlay();

    Serial.printf("[rules] loaded %u rule%s from filesystem\n",
                  (unsigned)_count, _count == 1 ? "" : "s");
}

uint16_t RulesService::reloadFromFs() {
    while (_count > 0) {
        _freeRuleContents(_rules[_count - 1]);
        memset(&_rules[_count - 1], 0, sizeof(Rule));
        _count--;
    }
    _ensureUserDir();
    _loadDir(DIR_FACTORY, true);
    _loadDir(DIR_USER,    false);
    _applyEnabledOverlay();
    return _count;
}

void RulesService::tick() {
    ScanResult batch[DRAIN_BATCH];
    uint32_t   lost = 0;
    const size_t got = g_scan.drain(&_ring_read_pos, batch, DRAIN_BATCH, &lost);
    _lost_scans += lost;
    for (size_t i = 0; i < got; i++) _matchScan(batch[i]);
}

void RulesService::onEvent(const Event& /*e*/) {
    // RulesService doesn't currently subscribe to anything — drain runs from
    // tick(). Hook reserved for future CMD_RULE_* events.
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

int RulesService::_findRuleIdx(const char* name) const {
    if (!name) return -1;
    for (uint16_t i = 0; i < _count; i++) {
        if (strcasecmp(_rules[i].name, name) == 0) return (int)i;
    }
    return -1;
}

const Rule* RulesService::find(const char* name) const {
    int i = _findRuleIdx(name);
    return i >= 0 ? &_rules[i] : nullptr;
}

const Rule* RulesService::get(uint16_t idx) const {
    if (idx >= _count) return nullptr;
    return &_rules[idx];
}

bool RulesService::_ensureCapacity(Rule& r, uint16_t need) {
    if (r.criterion_cap >= need) return true;
    uint16_t new_cap = r.criterion_cap ? r.criterion_cap : 4;
    while (new_cap < need) new_cap = (uint16_t)(new_cap * 2);
    if (new_cap > MAX_CRITERIA) new_cap = MAX_CRITERIA;
    if (new_cap < need) return false;
    Criterion* p = (Criterion*)heap_caps_realloc(
        r.criteria, sizeof(Criterion) * new_cap,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) return false;
    r.criteria     = p;
    r.criterion_cap = new_cap;
    return true;
}

bool RulesService::_appendCriterion(Rule& r, const Criterion& c) {
    if (!_ensureCapacity(r, (uint16_t)(r.criterion_count + 1))) return false;
    r.criteria[r.criterion_count++] = c;
    return true;
}

void RulesService::_freeCriterion(Criterion& c) {
    switch (c.kind) {
        case CRIT_NAME_EQUALS:
        case CRIT_NAME_CONTAINS:
        case CRIT_SSID_EQUALS:
        case CRIT_SSID_CONTAINS:
            if (c.v.str) heap_caps_free((void*)c.v.str);
            c.v.str = nullptr;
            break;
        default:
            break;
    }
}

void RulesService::_freeRuleContents(Rule& r) {
    if (r.criteria) {
        for (uint16_t i = 0; i < r.criterion_count; i++) _freeCriterion(r.criteria[i]);
        heap_caps_free(r.criteria);
    }
    r.criteria         = nullptr;
    r.criterion_count  = 0;
    r.criterion_cap    = 0;
}

bool RulesService::createRule(const char* name) {
    if (!name || !*name) return false;
    if (_findRuleIdx(name) >= 0) return false;
    if (_count >= MAX_RULES) return false;

    Rule& r = _rules[_count];
    memset(&r, 0, sizeof(Rule));
    strncpy(r.name, name, sizeof(r.name) - 1);
    strncpy(r.title, name, sizeof(r.title) - 1);
    r.vibe        = HAPTIC_PATTERN_COUNT;
    r.led         = LED_PATTERN_COUNT;
    r.alert_type  = ALERT_TYPE_COUNT;     // infer from scan domain
    r.action      = RULE_ACTION_ALERT;
    r.is_factory  = false;
    r.enabled     = true;
    _count++;
    _saveUserRule(r);   // persist immediately so create survives reboot even before criteria added
    return true;
}

bool RulesService::deleteRule(const char* name) {
    int idx = _findRuleIdx(name);
    if (idx < 0) return false;
    if (_rules[idx].is_factory) return false;   // factory rules are read-only
    char saved_name[24];
    strncpy(saved_name, _rules[idx].name, sizeof(saved_name));
    saved_name[sizeof(saved_name) - 1] = '\0';
    _freeRuleContents(_rules[idx]);
    for (uint16_t i = (uint16_t)idx; i + 1 < _count; i++) {
        _rules[i] = _rules[i + 1];
    }
    memset(&_rules[_count - 1], 0, sizeof(Rule));
    _count--;
    _deleteUserRuleFile(saved_name);
    return true;
}

bool RulesService::setRuleField(const char* name, const char* field, const char* value) {
    int idx = _findRuleIdx(name);
    if (idx < 0 || !field || !value) return false;
    Rule& r = _rules[idx];
    if (r.is_factory) return false;   // factory rule content is read-only

    if (strcasecmp(field, "title") == 0) {
        strncpy(r.title, value, sizeof(r.title) - 1);
        r.title[sizeof(r.title) - 1] = '\0';
        _saveUserRule(r);
        return true;
    }
    if (strcasecmp(field, "vibe") == 0) {
        HapticPatternId p = vibePatternByName(value);
        if (p == HAPTIC_PATTERN_COUNT) return false;
        r.vibe = p;
        _saveUserRule(r);
        return true;
    }
    if (strcasecmp(field, "led") == 0) {
        LedPatternId p = ledPatternByName(value);
        if (p == LED_PATTERN_COUNT) return false;
        r.led = p;
        _saveUserRule(r);
        return true;
    }
    if (strcasecmp(field, "type") == 0 || strcasecmp(field, "alert_type") == 0) {
        if (strcasecmp(value, "ble")    == 0 || strcasecmp(value, "bt") == 0) r.alert_type = ALERT_BLE;
        else if (strcasecmp(value, "wifi") == 0)                              r.alert_type = ALERT_WIFI;
        else if (strcasecmp(value, "sys")  == 0 ||
                 strcasecmp(value, "system") == 0)                            r.alert_type = ALERT_SYSTEM;
        else if (strcasecmp(value, "batt") == 0 ||
                 strcasecmp(value, "battery") == 0 ||
                 strcasecmp(value, "low") == 0)                               r.alert_type = ALERT_BATTERY_LOW;
        else if (strcasecmp(value, "auto") == 0 ||
                 strcasecmp(value, "infer") == 0)                             r.alert_type = ALERT_TYPE_COUNT;
        else return false;
        _saveUserRule(r);
        return true;
    }
    if (strcasecmp(field, "action") == 0) {
        if      (strcasecmp(value, "alert") == 0) r.action = RULE_ACTION_ALERT;
        else if (strcasecmp(value, "party") == 0) r.action = RULE_ACTION_PARTY;
        else return false;
        _saveUserRule(r);
        return true;
    }
    return false;
}

bool RulesService::setEnabled(const char* name, bool enabled) {
    int idx = _findRuleIdx(name);
    if (idx < 0) return false;
    _rules[idx].enabled = enabled;
    _persistEnabledOverlay();   // NVS overlay survives reboots + FS reflash
    return true;
}

bool RulesService::removeCriterion(const char* name, uint16_t idx) {
    int rIdx = _findRuleIdx(name);
    if (rIdx < 0) return false;
    Rule& r = _rules[rIdx];
    if (r.is_factory) return false;
    if (idx >= r.criterion_count) return false;
    _freeCriterion(r.criteria[idx]);
    for (uint16_t i = idx; i + 1 < r.criterion_count; i++) {
        r.criteria[i] = r.criteria[i + 1];
    }
    r.criterion_count--;
    _saveUserRule(r);
    return true;
}

// ---------------------------------------------------------------------------
// Criterion-adding: parse one field=values pair into one or more Criterion
// entries.
// ---------------------------------------------------------------------------

// Expand org_equals / org_contains into a set of CRIT_OUI or CRIT_MFG
// criteria by walking the vendor table. `equals` selects exact vs substring.
int RulesService::_expandOrgCriteria(Rule& r, CriterionKind kind, bool equals, const char* value) {
    if (!value || !*value) return 0;
    int added = 0;
    if (kind == CRIT_OUI) {
        const size_t N = vendor_oui_count();
        for (size_t k = 0; k < N; k++) {
            uint32_t   prefix;
            const char* name;
            vendor_oui_at(k, &prefix, &name);
            bool hit = equals ? (strcasecmp(name, value) == 0)
                              : (_icontains(name, value) != nullptr);
            if (!hit) continue;
            Criterion c{};
            c.kind = CRIT_OUI;
            c.v.oui_prefix = prefix;
            if (!_appendCriterion(r, c)) return added;   // hit MAX_CRITERIA
            added++;
        }
    } else {   // CRIT_MFG
        const size_t N = vendor_mfg_count();
        for (size_t k = 0; k < N; k++) {
            uint16_t   mfg_id;
            const char* name;
            vendor_mfg_at(k, &mfg_id, &name);
            bool hit = equals ? (strcasecmp(name, value) == 0)
                              : (_icontains(name, value) != nullptr);
            if (!hit) continue;
            Criterion c{};
            c.kind = CRIT_MFG;
            c.v.mfg_id = mfg_id;
            if (!_appendCriterion(r, c)) return added;
            added++;
        }
    }
    return added;
}

int RulesService::addCriteria(const char* name, const char* field, const char* values_csv) {
    int rIdx = _findRuleIdx(name);
    if (rIdx < 0 || !field || !values_csv) return -1;
    Rule& r = _rules[rIdx];
    if (r.is_factory) return -1;

    // Identify the field.
    enum FieldKind {
        F_OUI, F_MAC, F_MFG, F_SERVICE,
        F_NAME_EQ, F_NAME_CT, F_SSID_EQ, F_SSID_CT,
        F_OUI_ORG_EQ, F_OUI_ORG_CT, F_MFG_ORG_EQ, F_MFG_ORG_CT,
        F_UNKNOWN
    };
    FieldKind fk =
        (strcasecmp(field, "oui")              == 0) ? F_OUI        :
        (strcasecmp(field, "mac")              == 0) ? F_MAC        :
        (strcasecmp(field, "mfg")              == 0) ? F_MFG        :
        (strcasecmp(field, "service")          == 0) ? F_SERVICE    :
        (strcasecmp(field, "name_equals")      == 0) ? F_NAME_EQ    :
        (strcasecmp(field, "name_contains")    == 0) ? F_NAME_CT    :
        (strcasecmp(field, "ssid_equals")      == 0) ? F_SSID_EQ    :
        (strcasecmp(field, "ssid_contains")    == 0) ? F_SSID_CT    :
        (strcasecmp(field, "oui_org_equals")   == 0) ? F_OUI_ORG_EQ :
        (strcasecmp(field, "oui_org_contains") == 0) ? F_OUI_ORG_CT :
        (strcasecmp(field, "mfg_org_equals")   == 0) ? F_MFG_ORG_EQ :
        (strcasecmp(field, "mfg_org_contains") == 0) ? F_MFG_ORG_CT :
        F_UNKNOWN;
    if (fk == F_UNKNOWN) return -1;

    // Tokenize values by comma. Mutate the input copy.
    char* buf = strdup(values_csv);
    if (!buf) return -1;

    int added = 0;
    char* save = nullptr;
    for (char* tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(nullptr, ",", &save)) {
        // Trim leading/trailing whitespace.
        while (*tok == ' ' || *tok == '\t') tok++;
        char* end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (!*tok) continue;

        // Underscores stand in for spaces in stringy values.
        for (char* p = tok; *p; p++) if (*p == '_') *p = ' ';

        Criterion c{};
        switch (fk) {
            case F_OUI: {
                uint32_t pfx;
                if (!_parseOuiPrefix(tok, &pfx)) { free(buf); return -1; }
                c.kind = CRIT_OUI;
                c.v.oui_prefix = pfx;
                if (_appendCriterion(r, c)) added++;
                break;
            }
            case F_MAC: {
                uint8_t m[6];
                if (!_parseMac6(tok, m)) { free(buf); return -1; }
                c.kind = CRIT_MAC;
                memcpy(c.v.mac, m, 6);
                if (_appendCriterion(r, c)) added++;
                break;
            }
            case F_MFG: {
                uint16_t id;
                if (!_parseMfgId(tok, &id)) { free(buf); return -1; }
                c.kind = CRIT_MFG;
                c.v.mfg_id = id;
                if (_appendCriterion(r, c)) added++;
                break;
            }
            case F_SERVICE: {
                uint8_t u[16];
                if (!_parseServiceUuid(tok, u)) { free(buf); return -1; }
                c.kind = CRIT_SERVICE;
                memcpy(c.v.uuid, u, 16);
                if (_appendCriterion(r, c)) added++;
                break;
            }
            case F_NAME_EQ:
            case F_NAME_CT:
            case F_SSID_EQ:
            case F_SSID_CT: {
                // Store with original case; _icontains folds both sides at
                // match time. Preserves how the user typed it for `rules show`
                // and JSON round-trip.
                char* dup = _psram_strdup(tok);
                if (!dup) { free(buf); return -1; }
                c.kind = (fk == F_NAME_EQ) ? CRIT_NAME_EQUALS  :
                         (fk == F_NAME_CT) ? CRIT_NAME_CONTAINS:
                         (fk == F_SSID_EQ) ? CRIT_SSID_EQUALS  :
                                             CRIT_SSID_CONTAINS;
                c.v.str = dup;
                if (_appendCriterion(r, c)) added++;
                else                        heap_caps_free(dup);
                break;
            }
            case F_OUI_ORG_EQ:
                added += _expandOrgCriteria(r, CRIT_OUI, true,  tok);
                break;
            case F_OUI_ORG_CT:
                added += _expandOrgCriteria(r, CRIT_OUI, false, tok);
                break;
            case F_MFG_ORG_EQ:
                added += _expandOrgCriteria(r, CRIT_MFG, true,  tok);
                break;
            case F_MFG_ORG_CT:
                added += _expandOrgCriteria(r, CRIT_MFG, false, tok);
                break;
            default:
                break;
        }
    }

    free(buf);
    if (added > 0) _saveUserRule(r);
    return added;
}

// ---------------------------------------------------------------------------
// Match path
// ---------------------------------------------------------------------------

bool RulesService::_criterionMatches(const Criterion& c, const ScanResult& s) const {
    switch (c.kind) {
        case CRIT_OUI: {
            const uint32_t scan_prefix =
                ((uint32_t)s.mac[0] << 16) |
                ((uint32_t)s.mac[1] <<  8) |
                ((uint32_t)s.mac[2]);
            return scan_prefix == c.v.oui_prefix;
        }
        case CRIT_MAC:
            return memcmp(s.mac, c.v.mac, 6) == 0;
        case CRIT_MFG:
            return s.mfg_id != 0 && s.mfg_id == c.v.mfg_id;
        case CRIT_SERVICE:
            for (uint8_t i = 0; i < s.service_count && i < 4; i++) {
                if (memcmp(s.service_uuids[i], c.v.uuid, 16) == 0) return true;
            }
            return false;
        case CRIT_NAME_EQUALS:
            return c.v.str && strcmp(s.name, c.v.str) == 0;
        case CRIT_NAME_CONTAINS:
            return c.v.str && _icontains(s.name, c.v.str) != nullptr;
        case CRIT_SSID_EQUALS:
            return s.domain == SCAN_WIFI && c.v.str && strcmp(s.name, c.v.str) == 0;
        case CRIT_SSID_CONTAINS:
            return s.domain == SCAN_WIFI && c.v.str && _icontains(s.name, c.v.str) != nullptr;
        default:
            return false;
    }
}

void RulesService::_matchScan(const ScanResult& s) {
    for (uint16_t i = 0; i < _count; i++) {
        Rule& r = _rules[i];
        if (!r.enabled || r.criterion_count == 0) continue;
        for (uint16_t k = 0; k < r.criterion_count; k++) {
            if (_criterionMatches(r.criteria[k], s)) {
                _fire(r, s);
                _match_count++;
                r.match_count++;
                break;   // one match per (rule, scan) is enough
            }
        }
    }
}

void RulesService::_fire(Rule& r, const ScanResult& s) {
    if (r.action != RULE_ACTION_ALERT) {
        // RULE_ACTION_PARTY etc. — reserved for Phase 6.
        return;
    }

    // Dedup identifier: per (rule, MAC). AlertsService coalesces re-fires
    // into a last_seen update.
    char ident[24];
    snprintf(ident, sizeof(ident), "%s:%02X%02X%02X%02X%02X%02X",
             r.name,
             s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);

    AlertType type = r.alert_type;
    if (type == ALERT_TYPE_COUNT) {
        type = (s.domain == SCAN_BLE) ? ALERT_BLE : ALERT_WIFI;
    }
    HapticPatternId vibe = (r.vibe == HAPTIC_PATTERN_COUNT) ? HAPTIC_DOUBLE_TAP        : r.vibe;
    LedPatternId    led  = (r.led  == LED_PATTERN_COUNT)    ? LED_PATTERN_ALERT_DEFAULT : r.led;

    g_alerts.raise(r.title, type, vibe, led, ident, s.rssi);
}

// ---------------------------------------------------------------------------
// LittleFS persistence
// ---------------------------------------------------------------------------

void RulesService::_ensureUserDir() {
    if (!LittleFS.exists(DIR_RULES))   LittleFS.mkdir(DIR_RULES);
    if (!LittleFS.exists(DIR_FACTORY)) LittleFS.mkdir(DIR_FACTORY);
    if (!LittleFS.exists(DIR_USER))    LittleFS.mkdir(DIR_USER);
}

void RulesService::_loadDir(const char* dir_path, bool is_factory) {
    File dir = LittleFS.open(dir_path);
    if (!dir || !dir.isDirectory()) {
        Serial.printf("[rules] %s missing — skipping\n", dir_path);
        return;
    }
    File entry = dir.openNextFile();
    while (entry) {
        const char* fname = entry.name();
        // Skip non-JSON files and dotfiles. fname is the leaf — File::path()
        // returns the full LittleFS path which we need for open().
        if (!entry.isDirectory()
            && fname[0] != '.'
            && strstr(fname, ".json") != nullptr) {
            char path[64];
            snprintf(path, sizeof(path), "%s/%s", dir_path, fname);
            _loadRuleFromFile(path, is_factory);
        }
        entry = dir.openNextFile();
    }
}

// Translate an internal CriterionKind back into the on-disk field name.
// Used by _saveUserRule to group atomic criteria back into JSON entries.
static const char* _kindToField(CriterionKind k) {
    switch (k) {
        case CRIT_OUI:           return "oui";
        case CRIT_MAC:           return "mac";
        case CRIT_MFG:           return "mfg";
        case CRIT_SERVICE:       return "service";
        case CRIT_NAME_EQUALS:   return "name_equals";
        case CRIT_NAME_CONTAINS: return "name_contains";
        case CRIT_SSID_EQUALS:   return "ssid_equals";
        case CRIT_SSID_CONTAINS: return "ssid_contains";
        default:                 return nullptr;
    }
}

// Append `c`'s value to `arr` in whatever native form ArduinoJson handles
// for the kind. Strings are added directly; numerics formatted to a local
// buffer first so ArduinoJson copies them into its zone.
static void _appendCriterionValue(JsonArray arr, const Criterion& c) {
    char buf[40];
    switch (c.kind) {
        case CRIT_OUI:
            snprintf(buf, sizeof(buf), "%02X:%02X:%02X",
                     (unsigned)((c.v.oui_prefix >> 16) & 0xFF),
                     (unsigned)((c.v.oui_prefix >>  8) & 0xFF),
                     (unsigned)( c.v.oui_prefix        & 0xFF));
            arr.add(buf);
            break;
        case CRIT_MAC:
            snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     c.v.mac[0], c.v.mac[1], c.v.mac[2],
                     c.v.mac[3], c.v.mac[4], c.v.mac[5]);
            arr.add(buf);
            break;
        case CRIT_MFG:
            snprintf(buf, sizeof(buf), "0x%04X", (unsigned)c.v.mfg_id);
            arr.add(buf);
            break;
        case CRIT_SERVICE: {
            // Reverse the little-endian wire order back to big-endian display.
            int n = 0;
            for (int i = 15; i >= 0 && n < (int)sizeof(buf) - 1; i--) {
                n += snprintf(buf + n, sizeof(buf) - n, "%02X", c.v.uuid[i]);
                if (i == 12 || i == 10 || i == 8 || i == 6) {
                    if (n < (int)sizeof(buf) - 1) buf[n++] = '-';
                }
            }
            buf[n] = '\0';
            arr.add(buf);
            break;
        }
        case CRIT_NAME_EQUALS:
        case CRIT_NAME_CONTAINS:
        case CRIT_SSID_EQUALS:
        case CRIT_SSID_CONTAINS:
            arr.add(c.v.str ? c.v.str : "");
            break;
        default:
            break;
    }
}

bool RulesService::_saveUserRule(const Rule& r) {
    if (r.is_factory) return false;     // never overwrite factory files
    _ensureUserDir();

    char path[64];
    snprintf(path, sizeof(path), "%s/%s.json", DIR_USER, r.name);

    JsonDocument doc;
    doc["name"]  = r.name;
    doc["title"] = r.title;
    if (r.vibe != HAPTIC_PATTERN_COUNT) doc["vibe"] = vibePatternName(r.vibe);
    if (r.led  != LED_PATTERN_COUNT)    doc["led"]  = ledPatternName(r.led);
    if (r.alert_type != ALERT_TYPE_COUNT) {
        const char* t = "";
        switch (r.alert_type) {
            case ALERT_BLE:         t = "ble";  break;
            case ALERT_WIFI:        t = "wifi"; break;
            case ALERT_SYSTEM:      t = "sys";  break;
            case ALERT_BATTERY_LOW: t = "batt"; break;
            default: break;
        }
        doc["type"] = t;
    }
    if (r.action != RULE_ACTION_ALERT) {
        doc["action"] = (r.action == RULE_ACTION_PARTY) ? "party" : "alert";
    }

    // Group criteria by kind into per-kind JSON arrays. Order within each
    // kind preserves insertion order; kind order matches first-appearance.
    JsonArray criteria = doc["criteria"].to<JsonArray>();
    bool kind_emitted[CRIT_KIND_COUNT] = {false};
    for (uint16_t i = 0; i < r.criterion_count; i++) {
        const CriterionKind k = r.criteria[i].kind;
        if (k >= CRIT_KIND_COUNT || kind_emitted[k]) continue;
        const char* field = _kindToField(k);
        if (!field) continue;
        JsonObject obj = criteria.add<JsonObject>();
        JsonArray  arr = obj[field].to<JsonArray>();
        // Collect every criterion of this kind into one array.
        for (uint16_t j = i; j < r.criterion_count; j++) {
            if (r.criteria[j].kind == k) _appendCriterionValue(arr, r.criteria[j]);
        }
        kind_emitted[k] = true;
    }

    File f = LittleFS.open(path, "w");
    if (!f) {
        Serial.printf("[rules] FAIL: could not open %s for write\n", path);
        return false;
    }
    serializeJsonPretty(doc, f);
    f.close();
    return true;
}

bool RulesService::_deleteUserRuleFile(const char* name) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s.json", DIR_USER, name);
    if (!LittleFS.exists(path)) return false;
    return LittleFS.remove(path);
}

// ---------------------------------------------------------------------------
// Machine-readable I/O for the web companion
// ---------------------------------------------------------------------------

void RulesService::toJson(JsonArray rules) {
    for (uint16_t i = 0; i < _count; i++) {
        const Rule& r = _rules[i];
        JsonObject o = rules.add<JsonObject>();
        o["name"]    = r.name;
        o["title"]   = r.title;
        o["enabled"] = r.enabled;
        o["factory"] = r.is_factory;
        o["matches"] = r.match_count;
        if (r.vibe != HAPTIC_PATTERN_COUNT) o["vibe"] = vibePatternName(r.vibe);
        if (r.led  != LED_PATTERN_COUNT)    o["led"]  = ledPatternName(r.led);
        if (r.alert_type != ALERT_TYPE_COUNT) {
            const char* t = "";
            switch (r.alert_type) {
                case ALERT_BLE:         t = "ble";  break;
                case ALERT_WIFI:        t = "wifi"; break;
                case ALERT_SYSTEM:      t = "sys";  break;
                case ALERT_BATTERY_LOW: t = "batt"; break;
                default: break;
            }
            o["type"] = t;
        }
        if (r.action != RULE_ACTION_ALERT) {
            o["action"] = (r.action == RULE_ACTION_PARTY) ? "party" : "alert";
        }
        // Group atomic criteria back into per-kind arrays (same shape as files).
        JsonArray criteria = o["criteria"].to<JsonArray>();
        bool emitted[CRIT_KIND_COUNT] = {false};
        for (uint16_t a = 0; a < r.criterion_count; a++) {
            const CriterionKind k = r.criteria[a].kind;
            if (k >= CRIT_KIND_COUNT || emitted[k]) continue;
            const char* field = _kindToField(k);
            if (!field) continue;
            JsonArray arr = criteria.add<JsonObject>()[field].to<JsonArray>();
            for (uint16_t b = a; b < r.criterion_count; b++) {
                if (r.criteria[b].kind == k) _appendCriterionValue(arr, r.criteria[b]);
            }
            emitted[k] = true;
        }
    }
}

void RulesService::dumpJson(Print& out) {
    JsonDocument doc;
    toJson(doc.to<JsonArray>());
    serializeJson(doc, out);   // compact — one line
    out.println();
}

bool RulesService::saveRuleFromJson(const char* json) {
    if (!json) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;

    const char* name = doc["name"] | (const char*)nullptr;
    if (!name || !*name) return false;

    const int idx = _findRuleIdx(name);
    if (idx >= 0 && _rules[idx].is_factory) return false;   // never edit factory
    if (idx >= 0) deleteRule(name);                          // replace user rule

    if (!createRule(name)) return false;

    // Top-level fields. setRuleField no-ops on unknown values, so a bad value
    // just leaves the default rather than failing the whole save.
    const char* s;
    if ((s = doc["title"]  | (const char*)nullptr) && *s) setRuleField(name, "title",  s);
    if ((s = doc["vibe"]   | (const char*)nullptr) && *s) setRuleField(name, "vibe",   s);
    if ((s = doc["led"]    | (const char*)nullptr) && *s) setRuleField(name, "led",    s);
    if ((s = doc["type"]   | (const char*)nullptr) && *s) setRuleField(name, "type",   s);
    if ((s = doc["action"] | (const char*)nullptr) && *s) setRuleField(name, "action", s);

    // Criteria: each {field: [values...]} → CSV → addCriteria.
    for (JsonObject crit : doc["criteria"].as<JsonArray>()) {
        for (JsonPair kv : crit) {
            char csv[256];
            size_t n = 0;
            csv[0] = '\0';
            for (JsonVariant v : kv.value().as<JsonArray>()) {
                const char* val = v.as<const char*>();
                if (!val) continue;
                int w = snprintf(csv + n, sizeof(csv) - n, "%s%s", n ? "," : "", val);
                if (w < 0 || (size_t)w >= sizeof(csv) - n) break;   // safe truncate
                n += (size_t)w;
            }
            if (n) addCriteria(name, kv.key().c_str(), csv);
        }
    }

    if (doc["enabled"].is<bool>() && !doc["enabled"].as<bool>()) {
        setEnabled(name, false);
    }
    return true;
}


bool RulesService::_loadRuleFromFile(const char* path, bool is_factory) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[rules] %s: could not open\n", path);
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[rules] %s: parse error: %s\n", path, err.c_str());
        return false;
    }

    const char* name = doc["name"] | (const char*)nullptr;
    if (!name || !*name) {
        Serial.printf("[rules] %s: missing 'name'\n", path);
        return false;
    }
    if (_findRuleIdx(name) >= 0) {
        return false;   // already loaded (factory beats user on name collision)
    }
    if (_count >= MAX_RULES) {
        Serial.printf("[rules] %s: MAX_RULES reached\n", path);
        return false;
    }

    // Use createRule for the slot allocation, then patch up factory flag.
    // (createRule saves an empty file to /rules/user; if we're loading a
    // factory rule that path is harmless but a wasted write. Skip the save
    // by inlining instead.)
    Rule& r = _rules[_count];
    memset(&r, 0, sizeof(Rule));
    strncpy(r.name,  name, sizeof(r.name)  - 1);
    strncpy(r.title, name, sizeof(r.title) - 1);
    r.vibe       = HAPTIC_PATTERN_COUNT;
    r.led        = LED_PATTERN_COUNT;
    r.alert_type = ALERT_TYPE_COUNT;
    r.action     = RULE_ACTION_ALERT;
    r.is_factory = is_factory;
    r.enabled    = true;
    _count++;

    // Title
    if (const char* t = doc["title"] | (const char*)nullptr) {
        strncpy(r.title, t, sizeof(r.title) - 1);
        r.title[sizeof(r.title) - 1] = '\0';
    }
    // Vibe / LED / type / action via setRuleField path bypassed: we'd loop
    // back into _saveUserRule. Apply directly here.
    if (const char* v = doc["vibe"] | (const char*)nullptr) {
        HapticPatternId p = vibePatternByName(v);
        if (p != HAPTIC_PATTERN_COUNT) r.vibe = p;
    }
    if (const char* v = doc["led"] | (const char*)nullptr) {
        LedPatternId p = ledPatternByName(v);
        if (p != LED_PATTERN_COUNT) r.led = p;
    }
    if (const char* v = doc["type"] | (const char*)nullptr) {
        if      (strcasecmp(v, "ble")    == 0 || strcasecmp(v, "bt") == 0) r.alert_type = ALERT_BLE;
        else if (strcasecmp(v, "wifi")   == 0)                             r.alert_type = ALERT_WIFI;
        else if (strcasecmp(v, "sys")    == 0 ||
                 strcasecmp(v, "system") == 0)                             r.alert_type = ALERT_SYSTEM;
        else if (strcasecmp(v, "batt")   == 0 ||
                 strcasecmp(v, "battery")== 0 ||
                 strcasecmp(v, "low")    == 0)                             r.alert_type = ALERT_BATTERY_LOW;
    }
    if (const char* v = doc["action"] | (const char*)nullptr) {
        if      (strcasecmp(v, "alert") == 0) r.action = RULE_ACTION_ALERT;
        else if (strcasecmp(v, "party") == 0) r.action = RULE_ACTION_PARTY;
    }

    // Criteria — each entry is { field: [values...] }. Iterate fields,
    // build a CSV, and call addCriteria as if the user had typed it.
    // BUT addCriteria has the is_factory guard that we'd then trip — bypass
    // by temporarily clearing the flag, then restoring.
    JsonArray criteria = doc["criteria"].as<JsonArray>();
    const bool save_factory = r.is_factory;
    r.is_factory = false;
    for (JsonObject crit : criteria) {
        for (JsonPair kv : crit) {
            const char* field = kv.key().c_str();
            JsonArray   vals  = kv.value().as<JsonArray>();
            String csv;
            for (JsonVariant v : vals) {
                if (csv.length() > 0) csv += ",";
                csv += v.as<const char*>();
            }
            const int n = addCriteria(name, field, csv.c_str());
            if (n < 0) {
                Serial.printf("[rules] %s: bad criterion field '%s'\n", path, field);
            }
        }
    }
    r.is_factory = save_factory;
    return true;
}

// ---------------------------------------------------------------------------
// NVS enabled-overlay
//
// Single packed blob containing newline-separated names of rules the user
// has disabled. On boot we read it and clear `enabled` for any matching
// rule; on every toggle we rewrite it. Avoids per-rule keys (NVS key length
// would constrain rule names) and survives FS reflash.
// ---------------------------------------------------------------------------

void RulesService::_applyEnabledOverlay() {
    Preferences prefs;
    // Open RW so the namespace is created on first boot. isKey() check
    // dodges the [E] log line that getString emits when the key is absent.
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly*/ false)) return;
    if (!prefs.isKey(NVS_KEY_DISABLED)) { prefs.end(); return; }
    String blob = prefs.getString(NVS_KEY_DISABLED, "");
    prefs.end();
    if (blob.isEmpty()) return;

    // Parse blob: \n-separated names. For each, if a matching rule exists,
    // flip its enabled flag off.
    int start = 0;
    while (start < (int)blob.length()) {
        int nl = blob.indexOf('\n', start);
        if (nl < 0) nl = blob.length();
        String name = blob.substring(start, nl);
        name.trim();
        if (name.length() > 0) {
            int idx = _findRuleIdx(name.c_str());
            if (idx >= 0) _rules[idx].enabled = false;
        }
        start = nl + 1;
    }
}

void RulesService::_persistEnabledOverlay() {
    String blob;
    for (uint16_t i = 0; i < _count; i++) {
        if (!_rules[i].enabled) {
            if (blob.length() > 0) blob += "\n";
            blob += _rules[i].name;
        }
    }
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, /*readOnly*/ false)) return;
    if (blob.isEmpty()) prefs.remove(NVS_KEY_DISABLED);
    else                prefs.putString(NVS_KEY_DISABLED, blob);
    prefs.end();
}
