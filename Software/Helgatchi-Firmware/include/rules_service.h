#pragma once
#include "event_bus.h"
#include "scan_types.h"
#include "led_service.h"
#include "vibe_service.h"
#include "alerts_service.h"
#include <stdint.h>
#include <stddef.h>

class Print;   // Arduino Print (Serial) — for dumpJson

// ---------------------------------------------------------------------------
// Rules engine
//
// A Rule is a named bundle of match criteria plus the alert config to fire
// when any criterion hits. Every scan result drained from the scan ring is
// tested against every enabled rule; matching rules raise (or update) an
// alert via AlertsService with per-(rule, MAC) dedup.
//
// Phase 4 scope: rules live in PSRAM, created via serial commands. Phase 5
// adds LittleFS persistence (load on boot, save on mutation).
//
// Match semantics (decided in planning):
//   - Within a rule, all criteria are OR'd (any hit fires the rule).
//   - Each rule can carry many values per criterion; each value is its own
//     atomic match.
//   - Across rules, each match raises its own alert.
//   - Dedup is per-(rule, MAC): re-firing on the same device updates the
//     existing alert's last_seen instead of stacking new ones.
//
// `oui_org_*` and `mfg_org_*` source fields don't have runtime kinds — they
// get expanded at criterion-add time into the matching set of CRIT_OUI /
// CRIT_MFG entries by walking the vendor table once. So the hot path stays
// O(criteria) with O(1) per criterion.
// ---------------------------------------------------------------------------

enum CriterionKind : uint8_t {
    CRIT_OUI,              // 24-bit prefix match against scan.mac[0..2]
    CRIT_MAC,              // exact 6-byte MAC
    CRIT_MFG,              // 16-bit BT SIG company id (scan.mfg_id)
    CRIT_SERVICE,          // 128-bit BLE service UUID, matched against any of scan.service_uuids
    CRIT_NAME_EQUALS,      // strcmp against scan.name
    CRIT_NAME_CONTAINS,    // case-insensitive substring
    CRIT_SSID_EQUALS,      // same as NAME_EQUALS but gated to SCAN_WIFI
    CRIT_SSID_CONTAINS,    // same as NAME_CONTAINS but gated to SCAN_WIFI
    CRIT_KIND_COUNT,
};

struct Criterion {
    CriterionKind kind;
    union {
        uint32_t    oui_prefix;
        uint16_t    mfg_id;
        uint8_t     mac[6];
        uint8_t     uuid[16];
        const char* str;             // owned by the criterion; heap_caps_malloc PSRAM
    } v;
};

enum RuleAction : uint8_t {
    RULE_ACTION_ALERT = 0,           // raise/update via AlertsService
    RULE_ACTION_PARTY,               // stretch goal — emits EV_PARTY_MODE (Phase 6)
    RULE_ACTION_COUNT,
};

struct Rule {
    char            name[24];        // unique identifier, lowercase a-z 0-9 underscore
    char            title[40];       // alert title shown in UI
    HapticPatternId vibe;             // HAPTIC_PATTERN_COUNT = service default
    LedPatternId    led;              // LED_PATTERN_COUNT    = service default
    AlertType       alert_type;       // ALERT_TYPE_COUNT     = infer from scan domain
    RuleAction      action;
    bool            is_factory;       // false = user-editable; Phase 5 distinguishes
    bool            enabled;
    Criterion*      criteria;         // PSRAM, realloc'd as needed
    uint16_t        criterion_count;
    uint16_t        criterion_cap;
    uint32_t        match_count;      // per-rule firings since boot (informational)
};

class RulesService : public IEventHandler {
public:
    static constexpr uint16_t MAX_RULES        = 32;
    static constexpr uint16_t MAX_CRITERIA     = 256;   // hard cap per rule, protects against runaway org expansion
    static constexpr size_t   DRAIN_BATCH      = 16;    // scans processed per tick()

    void begin(EventBus& bus);
    void tick();
    void onEvent(const Event& e) override;

    // Wipe in-memory state and re-read /rules/factory + /rules/user from
    // LittleFS. Preserves NVS enable overlay (user disabled-state survives).
    // Returns number of rules loaded.
    uint16_t reloadFromFs();

    // --- Mutation API (serial commands today; JSON parser uses these in Phase 5) ---

    // Create an empty rule with the given name. Returns false on duplicate
    // name or if MAX_RULES is full. Defaults: title=name, alert_type=infer,
    // vibe/led=service default, action=alert, enabled=true.
    bool createRule(const char* name);

    // Set one of the top-level fields on an existing rule. `field` is one of:
    // title, vibe, led, type, action. `value` is interpreted per field.
    bool setRuleField(const char* name, const char* field, const char* value);

    // Add criteria. `field` is the rule-file field name (oui, mac, mfg,
    // service, name_equals, name_contains, ssid_equals, ssid_contains,
    // oui_org_equals, oui_org_contains, mfg_org_equals, mfg_org_contains).
    // `values_csv` is one or more comma-separated values for that field;
    // each becomes its own atomic criterion (with org_* fields expanding to
    // many atomic CRIT_OUI / CRIT_MFG entries). Returns the count of
    // criteria added, or -1 on parse error.
    int addCriteria(const char* name, const char* field, const char* values_csv);

    // Remove the Nth criterion (0-indexed in arrival order).
    bool removeCriterion(const char* name, uint16_t idx);

    // Delete a rule entirely, freeing its strings + criterion array.
    bool deleteRule(const char* name);

    // Enable / disable. (NVS persistence wired in Phase 5.)
    bool setEnabled(const char* name, bool enabled);

    // --- Machine-readable I/O for the web companion ---

    // Serialize every rule as a compact JSON array (one line) to `out`. Adds
    // runtime state (enabled / factory / matches) on top of the file fields.
    void dumpJson(Print& out);

    // Create or replace a USER rule from a full rule JSON object (same shape as
    // a rule file). Rejects editing a factory rule. Auto-persists. Returns
    // false on parse error, missing name, or a factory-name collision.
    bool saveRuleFromJson(const char* json);

    // --- Read API ---

    uint16_t    count() const { return _count; }
    const Rule* get(uint16_t idx) const;
    const Rule* find(const char* name) const;

    uint32_t totalMatches() const { return _match_count; }
    uint32_t lostScans()    const { return _lost_scans; }
    uint32_t ringReadPos()  const { return _ring_read_pos; }

private:
    EventBus* _bus = nullptr;
    Rule      _rules[MAX_RULES] = {};
    uint16_t  _count            = 0;

    uint32_t  _ring_read_pos    = 0;   // ScanService monotonic counter
    uint32_t  _match_count      = 0;
    uint32_t  _lost_scans       = 0;

    // Mutation helpers
    int      _findRuleIdx(const char* name) const;
    bool     _appendCriterion(Rule& r, const Criterion& c);
    bool     _ensureCapacity(Rule& r, uint16_t need);
    void     _freeCriterion(Criterion& c);
    void     _freeRuleContents(Rule& r);
    int      _expandOrgCriteria(Rule& r, CriterionKind kind, bool equals, const char* value);

    // Match path
    void     _matchScan(const ScanResult& scan);
    bool     _criterionMatches(const Criterion& c, const ScanResult& s) const;
    void     _fire(Rule& r, const ScanResult& s);

    // Persistence (FS + NVS overlay)
    void     _loadDir(const char* dir_path, bool is_factory);
    bool     _loadRuleFromFile(const char* path, bool is_factory);
    bool     _saveUserRule(const Rule& r);                 // writes /rules/user/<name>.json
    bool     _deleteUserRuleFile(const char* name);
    void     _ensureUserDir();                              // mkdir /rules /rules/user if missing
    void     _applyEnabledOverlay();                        // read NVS blob, mark rules disabled
    void     _persistEnabledOverlay();                      // rewrite NVS blob from current state
};

extern RulesService g_rules;
