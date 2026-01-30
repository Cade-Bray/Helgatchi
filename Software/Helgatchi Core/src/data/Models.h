#pragma once
#include <stdint.h>

struct OuiRule {
  uint32_t id = 0;
  bool enabled = true;
  // Match fields:
  // - If addr48 != 0: exact MAC match (lower 48 bits).
  // - Else if hasCompanyId: BLE manufacturer data (MSD) company identifier match.
  // - Else if oui24 != 0: OUI match (0xAABBCC).
  // - Else if nameContains[0] != 0: BLE name substring match (case-insensitive).
  // - Else if hasServiceUuid: BLE advertised service UUID match.
  uint32_t oui24 = 0;
  uint64_t addr48 = 0;
	bool hasCompanyId = false;
	uint16_t companyId = 0;
  char nameContains[20] = {0};
	bool hasServiceUuid = false;
	uint8_t serviceUuid128[16] = {0};
  // Rule pack/source name (e.g. from /rules/*.json). For user-added rules, this is typically "USER".
  char pack[16] = {0};
  char label[16] = {0};
};

// Persisted per-pack rule overrides.
// Each entry identifies a specific rule inside a filesystem pack by (pack name + signature hash).
// When present, the rule is forced disabled even if the pack is enabled.
struct PackRuleOverride {
  char pack[16] = {0};
  uint64_t sig = 0;
};
