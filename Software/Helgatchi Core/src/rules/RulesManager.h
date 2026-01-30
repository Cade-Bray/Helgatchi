
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>
#include <string>

#include "../data/Models.h"

class Store;

class RulesManager {
public:
  void loadFromStore(Store& store);
  // Loads built-in rule packs from the on-device filesystem (/rules/*.json).
  // These are not persisted back into NVS so they can be updated via uploadfs.
	void loadPacksFromFs(const std::vector<std::string>& disabledPacks);

  // Applies persisted per-rule overrides (disabled rules within enabled packs).
  void applyDisabledPackRules(const std::vector<PackRuleOverride>& disabled);
  void saveToStore(Store& store) const;

  // Persisted rules (user-added).
  const std::vector<OuiRule>& rules() const { return userRules_; }
  // Filesystem rules (from /rules packs).
  const std::vector<OuiRule>& packRules() const { return packRules_; }
  size_t packRuleCount() const { return packRules_.size(); }
  size_t ruleCount() const { return userRules_.size() + packRules_.size(); }
  size_t enabledRuleCount() const;

  struct RuleTypeCounts {
    uint16_t oui = 0;
    uint16_t companyId = 0;
    uint16_t nameContains = 0;
    uint16_t mac = 0;
    uint16_t serviceUuid = 0;
  };

  // Counts rule types across user + pack rules.
  // When enabledOnly=true, only rules with r.enabled==true are counted.
  RuleTypeCounts countRuleTypes(bool enabledOnly = true) const;

  uint32_t addOui(uint32_t oui24, const char* label, bool enabled = true);
  uint32_t addMac(uint64_t addr48, const char* label, bool enabled = true);
  uint32_t addCompany(uint16_t companyId, const char* label, bool enabled = true);
  uint32_t addName(const char* nameContains, const char* label, bool enabled = true);
  bool toggleRule(uint32_t id, bool enabled);

  // Returns the first enabled rule matching this OUI, else nullptr.
  const OuiRule* matchOui(uint32_t oui24) const;
  // BLE matching precedence:
  // 1) exact MAC rules
  // 2) service_uuid rules
  // 3) MSD company_id rules
  // 4) name substring rules
  // 5) OUI rules
  const OuiRule* matchBle(uint64_t addr48,
											uint32_t oui24,
											const char* name,
											uint8_t hasMsdCompanyId,
											uint16_t msdCompanyId,
											const uint8_t serviceUuids[][16],
											uint8_t serviceUuidCount) const;

	// Returns rule by id (enabled or not), searching user + pack rules.
	const OuiRule* findById(uint32_t id) const;
  // Same as findById, but also reports whether the rule came from a filesystem pack.
  const OuiRule* findById(uint32_t id, bool* outIsPackRule) const;

  // Stable per-rule signature for filesystem pack rules.
  // Used for persisting per-rule disable overrides across boots (since ids are assigned at load time).
  static uint64_t packRuleSignature(const OuiRule& r);
  
  static void setDebugPerformance(bool enabled);
  static bool debugPerformance();

  struct PackInfo {
    char name[16] = {0};
    bool enabled = true;
  };
  // Fills up to max pack entries, returns count.
  size_t getPackInfo(PackInfo* out, size_t max) const;
  // Enables/disables all rules belonging to a given pack name. Returns true if any rule was found.
  bool setPackEnabled(const char* pack, bool enabled);

  bool isPackEnabled(const char* pack) const;

private:
  std::vector<OuiRule> userRules_{};
  std::vector<OuiRule> packRules_{};
  std::vector<std::string> disabledPacks_{};
  uint32_t nextId_ = 1;

  const OuiRule* findByOui_(uint32_t oui24) const;
  const OuiRule* findByAddr_(uint64_t addr48) const;
	const OuiRule* findByName_(const char* nameContains) const;
	const OuiRule* findByService_(const uint8_t uuid128[16]) const;
  const OuiRule* findByCompany_(uint16_t companyId) const;
  bool hasOui_(uint32_t oui24) const;
  bool hasAddr_(uint64_t addr48) const;
	bool hasName_(const char* nameContains) const;
	bool hasService_(const uint8_t uuid128[16]) const;
  bool hasCompany_(uint16_t companyId) const;
};
