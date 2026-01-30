#include "RulesManager.h"

#include <algorithm>
#include <string.h>

#include <string>
#include <vector>

#include <Arduino.h>

#include <LittleFS.h>
#include <ArduinoJson.h>

#include "../data/Store.h"
#include "../core/Config.h"

static bool s_debugPerformance = false;

// Forward declarations for helpers defined later in this file.
static bool containsNoCase_(const char* haystack, const char* needle);
static bool eqStrNoCase_(const char* a, const char* b);
static uint16_t parseU16_(const char* s);

static bool readLine_(File& f, char* out, size_t outLen) {
	if (!out || outLen < 2) return false;
	out[0] = 0;
	if (!f) return false;
	if (!f.available()) return false;
	const size_t n = f.readBytesUntil('\n', out, outLen - 1);
	out[n] = 0;
	return true;
}

static bool parseJsonStringValue_(const char* line, const char* key, char* out, size_t outLen) {
	if (!line || !key || !out || outLen < 2) return false;
	out[0] = 0;
	const char* k = strstr(line, key);
	if (!k) return false;
	const char* colon = strchr(k, ':');
	if (!colon) return false;
	const char* q1 = strchr(colon, '"');
	if (!q1) return false;
	const char* q2 = strchr(q1 + 1, '"');
	if (!q2) return false;
	const size_t n = (size_t)(q2 - (q1 + 1));
	if (n == 0) return false;
	const size_t copyN = std::min(n, outLen - 1);
	memcpy(out, q1 + 1, copyN);
	out[copyN] = 0;
	return true;
}

static uint16_t btCompanyIdFromName_(const char* query) {
	if (!query || !*query) return 0;

	// Tiny cache to avoid rescanning the big file repeatedly when packs contain multiple company lookups.
	struct CacheEntry {
		char q[24];
		uint16_t id;
	};
	static CacheEntry cache[8]{};
	static uint8_t cacheCount = 0;

	for (uint8_t i = 0; i < cacheCount; i++) {
		if (eqStrNoCase_(cache[i].q, query)) return cache[i].id;
	}

	uint16_t found = 0;
	File f = LittleFS.open("/bt_company_ids.json", "r");
	if (f) {
		char line[128];
		char lastValue[24];
		lastValue[0] = 0;
		while (readLine_(f, line, sizeof(line))) {
			if (strstr(line, "\"value\"") != nullptr) {
				char tmp[24];
				if (parseJsonStringValue_(line, "\"value\"", tmp, sizeof(tmp))) {
					strncpy(lastValue, tmp, sizeof(lastValue) - 1);
					lastValue[sizeof(lastValue) - 1] = 0;
				}
				continue;
			}
			if (strstr(line, "\"name\"") != nullptr) {
				char name[64];
				if (!parseJsonStringValue_(line, "\"name\"", name, sizeof(name))) continue;
				if (!containsNoCase_(name, query)) continue;
				// lastValue should look like 0xXXXX.
				found = parseU16_(lastValue);
				if (found != 0) break;
			}
		}
		f.close();
	}

	if (cacheCount < (uint8_t)(sizeof(cache) / sizeof(cache[0]))) {
		CacheEntry e{};
		strncpy(e.q, query, sizeof(e.q) - 1);
		e.q[sizeof(e.q) - 1] = 0;
		e.id = found;
		cache[cacheCount++] = e;
	}
	return found;
}

static uint32_t parseOuiHex_(const char* s) {
	if (!s) return 0;
	// Accept "AABBCC", "0xAABBCC", or separator forms like "AA:BB:CC".
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

	char hex[7];
	int n = 0;
	for (; *s && n < 6; s++) {
		const char c = *s;
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			hex[n++] = c;
		}
	}
	if (n != 6) return 0;
	hex[6] = 0;

	char* end = nullptr;
	unsigned long v = strtoul(hex, &end, 16);
	if (end == hex) return 0;
	return ((uint32_t)v) & 0xFFFFFFu;
}

static uint64_t parseMac_(const char* s) {
	if (!s) return 0;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;

	char hex[13];
	int n = 0;
	for (; *s && n < 12; s++) {
		const char c = *s;
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
			hex[n++] = c;
		}
	}
	if (n != 12) return 0;
	hex[12] = 0;

	char* end = nullptr;
	unsigned long long v = strtoull(hex, &end, 16);
	if (end == hex) return 0;
	return ((uint64_t)v) & 0xFFFFFFFFFFFFULL;
}

static bool eqNoCase_(char a, char b) {
	if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
	if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
	return a == b;
}

static bool eqStrNoCase_(const char* a, const char* b) {
	if (!a || !b) return false;
	while (*a && *b) {
		if (!eqNoCase_(*a, *b)) return false;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

static bool containsNoCase_(const char* haystack, const char* needle) {
	if (!haystack || !needle) return false;
	if (!*needle) return false;

	for (const char* h = haystack; *h; h++) {
		const char* h2 = h;
		const char* n = needle;
		while (*h2 && *n && eqNoCase_(*h2, *n)) {
			h2++;
			n++;
		}
		if (!*n) return true;
	}
	return false;
}

static uint16_t parseU16_(const char* s) {
	if (!s) return 0;
	while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
	char* end = nullptr;
	unsigned long v = strtoul(s, &end, 0);
	if (end == s) return 0;
	return (uint16_t)(v & 0xFFFFu);
}

static int hexNibble_(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
	return -1;
}

static bool parseUuid128FromStr_(const char* s, uint8_t out[16]) {
	if (!s || !out) return false;

	// Extract hex digits only.
	char hex[33];
	int n = 0;
	while (*s) {
		const int h = hexNibble_(*s);
		if (h >= 0) {
			if (n >= 32) break;
			hex[n++] = *s;
		}
		s++;
	}

	// Support 16-bit + 32-bit UUIDs by expanding to Bluetooth base UUID.
	// Base UUID: 00000000-0000-1000-8000-00805F9B34FB
	if (n == 4 || n == 8) {
		char full[33] = {
			'0','0','0','0','0','0','0','0',
			'0','0','0','0','1','0','0','0',
			'8','0','0','0','0','0','8','0',
			'5','F','9','B','3','4','F','B'
		};
		// Insert into the leading 8 hex chars.
		const int start = (n == 4) ? 4 : 0;
		for (int i = 0; i < n; i++) {
			full[start + i] = hex[i];
		}
		memcpy(hex, full, 32);
		n = 32;
	}

	if (n != 32) return false;

	for (int i = 0; i < 16; i++) {
		const int hi = hexNibble_(hex[i * 2]);
		const int lo = hexNibble_(hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) return false;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return true;
}

static bool eqUuid128_(const uint8_t a[16], const uint8_t b[16]) {
	return memcmp(a, b, 16) == 0;
}

const OuiRule* RulesManager::findByOui_(uint32_t oui24) const {
	oui24 &= 0xFFFFFFu;
	for (const auto& r : userRules_) {
		if (r.addr48 == 0 && r.oui24 == oui24) return &r;
	}
	for (const auto& r : packRules_) {
		if (r.addr48 == 0 && r.oui24 == oui24) return &r;
	}
	return nullptr;
}

const OuiRule* RulesManager::findByAddr_(uint64_t addr48) const {
	addr48 &= 0xFFFFFFFFFFFFULL;
	for (const auto& r : userRules_) {
		if (r.addr48 == addr48 && addr48 != 0) return &r;
	}
	for (const auto& r : packRules_) {
		if (r.addr48 == addr48 && addr48 != 0) return &r;
	}
	return nullptr;
}

const OuiRule* RulesManager::findByName_(const char* nameContains) const {
	if (!nameContains || !*nameContains) return nullptr;
	for (const auto& r : userRules_) {
		if (r.nameContains[0] == 0) continue;
		if (eqStrNoCase_(r.nameContains, nameContains)) return &r;
	}
	for (const auto& r : packRules_) {
		if (r.nameContains[0] == 0) continue;
		if (eqStrNoCase_(r.nameContains, nameContains)) return &r;
	}
	return nullptr;
}

const OuiRule* RulesManager::findByService_(const uint8_t uuid128[16]) const {
	if (!uuid128) return nullptr;
	for (const auto& r : userRules_) {
		if (!r.hasServiceUuid) continue;
		if (eqUuid128_(r.serviceUuid128, uuid128)) return &r;
	}
	for (const auto& r : packRules_) {
		if (!r.hasServiceUuid) continue;
		if (eqUuid128_(r.serviceUuid128, uuid128)) return &r;
	}
	return nullptr;
}

const OuiRule* RulesManager::findByCompany_(uint16_t companyId) const {
	if (companyId == 0) return nullptr;
	for (const auto& r : userRules_) {
		if (!r.hasCompanyId) continue;
		if (r.companyId == companyId) return &r;
	}
	for (const auto& r : packRules_) {
		if (!r.hasCompanyId) continue;
		if (r.companyId == companyId) return &r;
	}
	return nullptr;
}

bool RulesManager::hasOui_(uint32_t oui24) const {
	return findByOui_(oui24) != nullptr;
}

bool RulesManager::hasAddr_(uint64_t addr48) const {
	return findByAddr_(addr48) != nullptr;
}

bool RulesManager::hasName_(const char* nameContains) const {
	return findByName_(nameContains) != nullptr;
}

bool RulesManager::hasService_(const uint8_t uuid128[16]) const {
	return findByService_(uuid128) != nullptr;
}

bool RulesManager::hasCompany_(uint16_t companyId) const {
	return findByCompany_(companyId) != nullptr;
}

void RulesManager::loadFromStore(Store& store) {
	userRules_ = store.loadRules();

	// Recompute nextId_ (monotonic)
	uint32_t maxId = 0;
	for (const auto& r : userRules_) {
		if (r.id > maxId) maxId = r.id;
	}
	nextId_ = maxId + 1;
	if (nextId_ == 0) nextId_ = 1;
}

static bool isPackDisabled_(const char* pack, const std::vector<std::string>& disabled) {
	if (!pack || !*pack) return false;
	for (const auto& s : disabled) {
		if (s == pack) return true;
	}
	return false;
}

static uint64_t fnv1a64_(const void* data, size_t len, uint64_t h = 14695981039346656037ULL) {
	const uint8_t* p = (const uint8_t*)data;
	for (size_t i = 0; i < len; i++) {
		h ^= (uint64_t)p[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static uint64_t fnv1a64_str_(const char* s, uint64_t h = 14695981039346656037ULL) {
	if (!s) return h;
	while (*s) {
		h ^= (uint64_t)(uint8_t)(*s++);
		h *= 1099511628211ULL;
	}
	return h;
}

uint64_t RulesManager::packRuleSignature(const OuiRule& r) {
	// Signature includes the pack name + the match criterion. Does not include label or enabled.
	uint64_t h = 14695981039346656037ULL;
	h = fnv1a64_str_(r.pack, h);

	// Match precedence: MAC > service UUID > company ID > name > OUI.
	const uint64_t addr48 = r.addr48 & 0xFFFFFFFFFFFFULL;
	if (addr48 != 0) {
		const uint8_t kind = 1;
		h = fnv1a64_(&kind, 1, h);
		h = fnv1a64_(&addr48, sizeof(addr48), h);
		return h;
	}
	if (r.hasServiceUuid) {
		const uint8_t kind = 2;
		h = fnv1a64_(&kind, 1, h);
		h = fnv1a64_(r.serviceUuid128, sizeof(r.serviceUuid128), h);
		return h;
	}
	if (r.hasCompanyId && r.companyId != 0) {
		const uint8_t kind = 3;
		h = fnv1a64_(&kind, 1, h);
		const uint16_t cid = r.companyId;
		h = fnv1a64_(&cid, sizeof(cid), h);
		return h;
	}
	if (r.nameContains[0] != 0) {
		const uint8_t kind = 4;
		h = fnv1a64_(&kind, 1, h);
		h = fnv1a64_str_(r.nameContains, h);
		return h;
	}
	const uint32_t oui24 = r.oui24 & 0xFFFFFFu;
	if (oui24 != 0) {
		const uint8_t kind = 5;
		h = fnv1a64_(&kind, 1, h);
		h = fnv1a64_(&oui24, sizeof(oui24), h);
		return h;
	}

	return h;
}

void RulesManager::loadPacksFromFs(const std::vector<std::string>& disabledPacks) {
	packRules_.clear();
	disabledPacks_ = disabledPacks;

	if (!LittleFS.begin(false)) {
		// No filesystem mounted (or not uploaded yet). Safe to ignore.
		return;
	}

	File dir = LittleFS.open("/rules");
	if (!dir || !dir.isDirectory()) {
		return;
	}

	File f = dir.openNextFile();
	while (f) {
		if (!f.isDirectory()) {
			String name = f.name();
			if (name.endsWith(".json")) {
				JsonDocument doc;
				const DeserializationError err = deserializeJson(doc, f);
				if (!err) {
					const char* pack = doc["pack"] | doc["name"] | "PACK";
					const bool enabledByDefault = !isPackDisabled_(pack, disabledPacks);
					JsonArray rules = doc["rules"].as<JsonArray>();
					for (JsonVariant v : rules) {
						uint32_t oui24 = 0;
						uint64_t addr48 = 0;
						const char* nameContains = nullptr;
						const char* serviceUuid = nullptr;
						uint8_t serviceUuid128[16];
						bool hasService = false;
						bool hasCompany = false;
						uint16_t companyId = 0;
						const char* label = nullptr;

						if (v.is<const char*>()) {
							// Bare string defaults to OUI.
							oui24 = parseOuiHex_(v.as<const char*>());
							label = pack;
						} else if (v.is<JsonObject>()) {
							JsonObject o = v.as<JsonObject>();
							if (o["mac"].is<const char*>()) addr48 = parseMac_(o["mac"].as<const char*>());
							else if (o["addr"].is<const char*>()) addr48 = parseMac_(o["addr"].as<const char*>());
							if (o["company_id"].is<const char*>()) {
								companyId = parseU16_(o["company_id"].as<const char*>());
								hasCompany = (companyId != 0);
							} else if (o["company_id"].is<uint16_t>() || o["company_id"].is<int>()) {
								companyId = (uint16_t)(o["company_id"].as<unsigned int>() & 0xFFFFu);
								hasCompany = (companyId != 0);
							} else if (o["msd_company_id"].is<const char*>()) {
								companyId = parseU16_(o["msd_company_id"].as<const char*>());
								hasCompany = (companyId != 0);
							} else if (o["msd_company_id"].is<uint16_t>() || o["msd_company_id"].is<int>()) {
								companyId = (uint16_t)(o["msd_company_id"].as<unsigned int>() & 0xFFFFu);
								hasCompany = (companyId != 0);
							}
							if (!hasCompany) {
								const char* companyName = nullptr;
								if (o["company"].is<const char*>()) companyName = o["company"].as<const char*>();
								else if (o["company_name"].is<const char*>()) companyName = o["company_name"].as<const char*>();
								if (companyName && *companyName) {
									companyId = btCompanyIdFromName_(companyName);
									hasCompany = (companyId != 0);
								}
							}
							if (o["name_contains"].is<const char*>()) nameContains = o["name_contains"].as<const char*>();
							else if (o["name"].is<const char*>()) nameContains = o["name"].as<const char*>();
							if (o["service_uuid"].is<const char*>()) serviceUuid = o["service_uuid"].as<const char*>();
							else if (o["service"].is<const char*>()) serviceUuid = o["service"].as<const char*>();
							else if (o["uuid"].is<const char*>()) serviceUuid = o["uuid"].as<const char*>();
							if (serviceUuid && *serviceUuid) {
								hasService = parseUuid128FromStr_(serviceUuid, serviceUuid128);
							}

							if (addr48 == 0) {
								if (o["oui"].is<const char*>()) oui24 = parseOuiHex_(o["oui"].as<const char*>());
								else if (o["oui"].is<uint32_t>()) oui24 = (uint32_t)o["oui"];
							}
							label = o["label"] | pack;
						}

						oui24 &= 0xFFFFFFu;
						addr48 &= 0xFFFFFFFFFFFFULL;
						const bool hasName = (nameContains && *nameContains);
						if (addr48 == 0 && oui24 == 0 && !hasName && !hasService && !hasCompany) continue;
						// Don't let pack rules override user rules (or other pack rules).
						if (addr48 != 0) {
							if (hasAddr_(addr48)) continue;
						} else if (hasService) {
							if (hasService_(serviceUuid128)) continue;
						} else if (hasCompany) {
							if (hasCompany_(companyId)) continue;
						} else if (oui24 != 0) {
							if (hasOui_(oui24)) continue;
						} else {
							if (hasName_(nameContains)) continue;
						}

						OuiRule r{};
						r.id = nextId_++;
						if (nextId_ == 0) nextId_ = 1;
						r.enabled = enabledByDefault;
						r.oui24 = oui24;
						r.addr48 = addr48;
						r.hasCompanyId = hasCompany;
						r.companyId = hasCompany ? companyId : 0;
						if (hasName) {
							strncpy(r.nameContains, nameContains, sizeof(r.nameContains) - 1);
							r.nameContains[sizeof(r.nameContains) - 1] = 0;
						} else {
							r.nameContains[0] = 0;
						}
						r.hasServiceUuid = hasService;
						if (hasService) memcpy(r.serviceUuid128, serviceUuid128, sizeof(r.serviceUuid128));
						else memset(r.serviceUuid128, 0, sizeof(r.serviceUuid128));
						strncpy(r.pack, pack, sizeof(r.pack) - 1);
						r.pack[sizeof(r.pack) - 1] = 0;
						if (label) {
							strncpy(r.label, label, sizeof(r.label) - 1);
							r.label[sizeof(r.label) - 1] = 0;
						}
						packRules_.push_back(r);
					}
				}
			}
		}
		f = dir.openNextFile();
	}
}

void RulesManager::applyDisabledPackRules(const std::vector<PackRuleOverride>& disabled) {
	if (disabled.empty()) return;
	for (auto& r : packRules_) {
		if (!r.pack[0]) continue;
		// If the master pack is disabled, all rules are already disabled.
		if (!isPackEnabled(r.pack)) {
			r.enabled = false;
			continue;
		}
		const uint64_t sig = packRuleSignature(r);
		bool isDisabled = false;
		for (const auto& d : disabled) {
			if (d.sig != sig) continue;
			if (strncmp(d.pack, r.pack, sizeof(r.pack)) != 0) continue;
			isDisabled = true;
			break;
		}
		if (isDisabled) r.enabled = false;
	}
}

void RulesManager::saveToStore(Store& store) const {
	store.saveRules(userRules_);
}

size_t RulesManager::enabledRuleCount() const {
	size_t n = 0;
	for (const auto& r : userRules_) {
		if (r.enabled) n++;
	}
	for (const auto& r : packRules_) {
		if (r.enabled) n++;
	}
	return n;
}

RulesManager::RuleTypeCounts RulesManager::countRuleTypes(bool enabledOnly) const {
	RuleTypeCounts out{};

	auto addOne = [&](const OuiRule& r) {
		if (enabledOnly && !r.enabled) return;
		// Match precedence (see matchBle): MAC > service UUID > company ID > name > OUI.
		if ((r.addr48 & 0xFFFFFFFFFFFFULL) != 0) {
			out.mac++;
			return;
		}
		if (r.hasServiceUuid) {
			out.serviceUuid++;
			return;
		}
		if (r.hasCompanyId && r.companyId != 0) {
			out.companyId++;
			return;
		}
		if (r.nameContains[0] != 0) {
			out.nameContains++;
			return;
		}
		if ((r.oui24 & 0xFFFFFFu) != 0) {
			out.oui++;
			return;
		}
	};

	for (const auto& r : userRules_) addOne(r);
	for (const auto& r : packRules_) addOne(r);
	return out;
}

uint32_t RulesManager::addOui(uint32_t oui24, const char* label, bool enabled) {
	oui24 &= 0xFFFFFFu;
	for (const auto& existing : userRules_) {
		if (existing.addr48 == 0 && existing.oui24 == oui24) return existing.id;
	}

	OuiRule r{};
	r.id = nextId_++;
	if (nextId_ == 0) nextId_ = 1;

	r.enabled = enabled;
	r.oui24 = oui24;
	r.addr48 = 0;
	r.nameContains[0] = 0;
	strncpy(r.pack, "USER", sizeof(r.pack) - 1);
	r.pack[sizeof(r.pack) - 1] = 0;

	if (label) {
		strncpy(r.label, label, sizeof(r.label) - 1);
		r.label[sizeof(r.label) - 1] = 0;
	}

	userRules_.push_back(r);
	return r.id;
}

uint32_t RulesManager::addMac(uint64_t addr48, const char* label, bool enabled) {
	addr48 &= 0xFFFFFFFFFFFFULL;
	for (const auto& existing : userRules_) {
		if (existing.addr48 == addr48 && addr48 != 0) return existing.id;
	}

	OuiRule r{};
	r.id = nextId_++;
	if (nextId_ == 0) nextId_ = 1;

	r.enabled = enabled;
	r.addr48 = addr48;
	r.oui24 = 0;
	r.nameContains[0] = 0;
	strncpy(r.pack, "USER", sizeof(r.pack) - 1);
	r.pack[sizeof(r.pack) - 1] = 0;

	if (label) {
		strncpy(r.label, label, sizeof(r.label) - 1);
		r.label[sizeof(r.label) - 1] = 0;
	}

	userRules_.push_back(r);
	return r.id;
}

uint32_t RulesManager::addCompany(uint16_t companyId, const char* label, bool enabled) {
	for (const auto& existing : userRules_) {
		if (existing.hasCompanyId && existing.companyId == companyId) return existing.id;
	}

	OuiRule r{};
	r.id = nextId_++;
	if (nextId_ == 0) nextId_ = 1;

	r.enabled = enabled;
	r.hasCompanyId = true;
	r.companyId = companyId;
	r.oui24 = 0;
	r.addr48 = 0;
	r.nameContains[0] = 0;
	strncpy(r.pack, "USER", sizeof(r.pack) - 1);
	r.pack[sizeof(r.pack) - 1] = 0;

	if (label) {
		strncpy(r.label, label, sizeof(r.label) - 1);
		r.label[sizeof(r.label) - 1] = 0;
	}

	userRules_.push_back(r);
	return r.id;
}

uint32_t RulesManager::addName(const char* nameContains, const char* label, bool enabled) {
	if (!nameContains || !nameContains[0]) return 0;
	
	for (const auto& existing : userRules_) {
		if (existing.nameContains[0] && strcmp(existing.nameContains, nameContains) == 0) return existing.id;
	}

	OuiRule r{};
	r.id = nextId_++;
	if (nextId_ == 0) nextId_ = 1;

	r.enabled = enabled;
	r.oui24 = 0;
	r.addr48 = 0;
	r.hasCompanyId = false;
	strncpy(r.nameContains, nameContains, sizeof(r.nameContains) - 1);
	r.nameContains[sizeof(r.nameContains) - 1] = 0;
	strncpy(r.pack, "USER", sizeof(r.pack) - 1);
	r.pack[sizeof(r.pack) - 1] = 0;

	if (label) {
		strncpy(r.label, label, sizeof(r.label) - 1);
		r.label[sizeof(r.label) - 1] = 0;
	}

	userRules_.push_back(r);
	return r.id;
}

bool RulesManager::toggleRule(uint32_t id, bool enabled) {
	for (auto& r : userRules_) {
		if (r.id == id) {
			r.enabled = enabled;
			return true;
		}
	}
	for (auto& r : packRules_) {
		if (r.id == id) {
			r.enabled = enabled;
			return true;
		}
	}
	return false;
}

const OuiRule* RulesManager::matchOui(uint32_t oui24) const {
	uint32_t _t0 = s_debugPerformance ? micros() : 0;
	oui24 &= 0xFFFFFFu;
	for (const auto& r : userRules_) {
		if (!r.enabled) continue;
		if (r.addr48 == 0 && r.oui24 == oui24) {
			if (s_debugPerformance && _t0 > 0) {
				Serial.printf("[PERF] matchOui(user): %lu us\n", (unsigned long)(micros() - _t0));
			}
			return &r;
		}
	}
	for (const auto& r : packRules_) {
		if (!r.enabled) continue;
		if (r.addr48 == 0 && r.oui24 == oui24) {
			if (s_debugPerformance && _t0 > 0) {
				Serial.printf("[PERF] matchOui(pack): %lu us\n", (unsigned long)(micros() - _t0));
			}
			return &r;
		}
	}
	if (s_debugPerformance && _t0 > 0) {
		Serial.printf("[PERF] matchOui(nomatch): %lu us\n", (unsigned long)(micros() - _t0));
	}
	return nullptr;
}

const OuiRule* RulesManager::matchBle(uint64_t addr48,
											uint32_t oui24,
											const char* name,
											uint8_t hasMsdCompanyId,
											uint16_t msdCompanyId,
											const uint8_t serviceUuids[][16],
											uint8_t serviceUuidCount) const {
	uint32_t _t0 = s_debugPerformance ? micros() : 0;
	addr48 &= 0xFFFFFFFFFFFFULL;
	if (addr48 != 0) {
		for (const auto& r : userRules_) {
			if (!r.enabled) continue;
			if (r.addr48 != 0 && r.addr48 == addr48) {
				if (s_debugPerformance && _t0 > 0) {
					Serial.printf("[PERF] matchBle(mac_user): %lu us\n", (unsigned long)(micros() - _t0));
				}
				return &r;
			}
		}
		for (const auto& r : packRules_) {
			if (!r.enabled) continue;
			if (r.addr48 != 0 && r.addr48 == addr48) {
				if (s_debugPerformance && _t0 > 0) {
					Serial.printf("[PERF] matchBle(mac_pack): %lu us\n", (unsigned long)(micros() - _t0));
				}
				return &r;
			}
		}
	}

	if (serviceUuids && serviceUuidCount > 0) {
		for (uint8_t i = 0; i < serviceUuidCount; i++) {
			const uint8_t* uuid = serviceUuids[i];
			for (const auto& r : userRules_) {
				if (!r.enabled) continue;
				if (!r.hasServiceUuid) continue;
				if (eqUuid128_(r.serviceUuid128, uuid)) {
					if (s_debugPerformance && _t0 > 0) {
						Serial.printf("[PERF] matchBle(uuid_user): %lu us\n", (unsigned long)(micros() - _t0));
					}
					return &r;
				}
			}
			for (const auto& r : packRules_) {
				if (!r.enabled) continue;
				if (!r.hasServiceUuid) continue;
				if (eqUuid128_(r.serviceUuid128, uuid)) {
					if (s_debugPerformance && _t0 > 0) {
						Serial.printf("[PERF] matchBle(uuid_pack): %lu us\n", (unsigned long)(micros() - _t0));
					}
					return &r;
				}
			}
		}
	}

	if (hasMsdCompanyId && msdCompanyId != 0) {
		for (const auto& r : userRules_) {
			if (!r.enabled) continue;
			if (!r.hasCompanyId) continue;
			if (r.companyId == msdCompanyId) {
				if (s_debugPerformance && _t0 > 0) {
					Serial.printf("[PERF] matchBle(cid_user): %lu us\n", (unsigned long)(micros() - _t0));
				}
				return &r;
			}
		}
		for (const auto& r : packRules_) {
			if (!r.enabled) continue;
			if (!r.hasCompanyId) continue;
			if (r.companyId == msdCompanyId) {
				if (s_debugPerformance && _t0 > 0) {
					Serial.printf("[PERF] matchBle(cid_pack): %lu us\n", (unsigned long)(micros() - _t0));
				}
				return &r;
			}
		}
	}

	if (name && *name) {
		for (const auto& r : userRules_) {
			if (!r.enabled) continue;
			if (r.nameContains[0] == 0) continue;
			if (containsNoCase_(name, r.nameContains)) {
				if (s_debugPerformance && _t0 > 0) {
					Serial.printf("[PERF] matchBle(name_user): %lu us\n", (unsigned long)(micros() - _t0));
				}
				return &r;
			}
		}
		for (const auto& r : packRules_) {
			if (!r.enabled) continue;
			if (r.nameContains[0] == 0) continue;
			if (containsNoCase_(name, r.nameContains)) {
				if (s_debugPerformance && _t0 > 0) {
					Serial.printf("[PERF] matchBle(name_pack): %lu us\n", (unsigned long)(micros() - _t0));
				}
				return &r;
			}
		}
	}
	const OuiRule* result = matchOui(oui24);
	if (s_debugPerformance && _t0 > 0) {
		Serial.printf("[PERF] matchBle(total): %lu us\n", (unsigned long)(micros() - _t0));
	}
	return result;
}

const OuiRule* RulesManager::findById(uint32_t id) const {
	bool unused = false;
	return findById(id, &unused);
}

const OuiRule* RulesManager::findById(uint32_t id, bool* outIsPackRule) const {
	if (outIsPackRule) *outIsPackRule = false;
	if (id == 0) return nullptr;
	for (const auto& r : userRules_) {
		if (r.id == id) {
			if (outIsPackRule) *outIsPackRule = false;
			return &r;
		}
	}
	for (const auto& r : packRules_) {
		if (r.id == id) {
			if (outIsPackRule) *outIsPackRule = true;
			return &r;
		}
	}
	return nullptr;
}

size_t RulesManager::getPackInfo(PackInfo* out, size_t max) const {
	if (!out || max == 0) return 0;
	size_t count = 0;
	for (const auto& r : packRules_) {
		if (!r.pack[0]) continue;
		bool exists = false;
		for (size_t i = 0; i < count; i++) {
			if (strncmp(out[i].name, r.pack, sizeof(out[i].name)) == 0) {
				exists = true;
				break;
			}
		}
		if (exists) continue;
		if (count >= max) break;
		strncpy(out[count].name, r.pack, sizeof(out[count].name) - 1);
		out[count].name[sizeof(out[count].name) - 1] = 0;
		out[count].enabled = isPackEnabled(out[count].name);
		count++;
	}
	return count;
}

bool RulesManager::setPackEnabled(const char* pack, bool enabled) {
	if (!pack || !*pack) return false;
	// Update master pack state (stored here for UI/queries).
	const std::string p(pack);
	if (!enabled) {
		if (!isPackDisabled_(pack, disabledPacks_)) disabledPacks_.push_back(p);
	} else {
		disabledPacks_.erase(std::remove(disabledPacks_.begin(), disabledPacks_.end(), p), disabledPacks_.end());
	}

	bool found = false;
	for (auto& r : packRules_) {
		if (strncmp(r.pack, pack, sizeof(r.pack)) == 0) {
			r.enabled = enabled;
			found = true;
		}
	}
	return found;
}

bool RulesManager::isPackEnabled(const char* pack) const {
	return !isPackDisabled_(pack, disabledPacks_);
}

void RulesManager::setDebugPerformance(bool enabled) {
	s_debugPerformance = enabled;
}

bool RulesManager::debugPerformance() {
	return s_debugPerformance;
}
