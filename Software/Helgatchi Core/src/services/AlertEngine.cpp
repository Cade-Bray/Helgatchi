#include "AlertEngine.h"

#include <Arduino.h>
#include <stdio.h>

#include "../rules/RulesManager.h"

static void formatOui_(uint32_t oui24, char* out, size_t outLen) {
	if (!out || outLen < 9) return;
	oui24 &= 0xFFFFFFu;
	const uint8_t b0 = (uint8_t)((oui24 >> 16) & 0xFFu);
	const uint8_t b1 = (uint8_t)((oui24 >> 8) & 0xFFu);
	const uint8_t b2 = (uint8_t)(oui24 & 0xFFu);
	snprintf(out, outLen, "%02X:%02X:%02X", (unsigned)b0, (unsigned)b1, (unsigned)b2);
}

static void formatMac48_(uint64_t addr48, char* out, size_t outLen) {
	if (!out || outLen < 18) return;
	addr48 &= 0xFFFFFFFFFFFFULL;
	const uint8_t b0 = (uint8_t)((addr48 >> 40) & 0xFFu);
	const uint8_t b1 = (uint8_t)((addr48 >> 32) & 0xFFu);
	const uint8_t b2 = (uint8_t)((addr48 >> 24) & 0xFFu);
	const uint8_t b3 = (uint8_t)((addr48 >> 16) & 0xFFu);
	const uint8_t b4 = (uint8_t)((addr48 >> 8) & 0xFFu);
	const uint8_t b5 = (uint8_t)(addr48 & 0xFFu);
	snprintf(out, outLen, "%02X:%02X:%02X:%02X:%02X:%02X", (unsigned)b0, (unsigned)b1, (unsigned)b2,
	         (unsigned)b3, (unsigned)b4, (unsigned)b5);
}

static const char* classifyBleAddr48_(uint64_t addr48) {
	addr48 &= 0xFFFFFFFFFFFFULL;
	if (addr48 == 0) return "--";
	const uint8_t first = (uint8_t)((addr48 >> 40) & 0xFFu);
	const uint8_t tag = (uint8_t)(first >> 6);
	switch (tag) {
		case 0:
			return "NRPA?";
		case 1:
			return "RPA?";
		case 3:
			return "STATIC?";
		default:
			return "RESV?";
	}
}

bool AlertEngine::shouldFire_(uint32_t ruleId, uint32_t nowMs) {
	// Linear search over a small fixed cache.
	for (uint8_t i = 0; i < lastCount_; i++) {
		if (last_[i].ruleId == ruleId) {
			if ((nowMs - last_[i].tsMs) < minIntervalMs_) return false;
			last_[i].tsMs = nowMs;
			return true;
		}
	}

	// Not found: insert/replace
	if (lastCount_ < (sizeof(last_) / sizeof(last_[0]))) {
		last_[lastCount_++] = {ruleId, nowMs};
		return true;
	}

	// Replace oldest.
	uint8_t oldest = 0;
	for (uint8_t i = 1; i < lastCount_; i++) {
		if (last_[i].tsMs < last_[oldest].tsMs) oldest = i;
	}
	last_[oldest] = {ruleId, nowMs};
	return true;
}

void AlertEngine::onWifiSighting(const WifiSightingEvent& e, const RulesManager& rules, CoreState& state, EventBus& bus) {
	const OuiRule* match = rules.matchOui(e.oui24);
	if (!match) return;
	if (state.settings.debugLevel > 1) {
		Serial.print("[alert] wifi match rule=");
		Serial.print(match->id);
		Serial.print(" oui=");
		Serial.print(e.oui24, HEX);
		Serial.print(" rssi=");
		Serial.println((int)e.rssi);
	}

	state.matches++;
	if (!shouldFire_(match->id, e.tsMs)) {
		if (state.settings.debugLevel > 1) Serial.println("[alert] rate-limited");
		return;
	}

	Event out{};
	out.type = EventType::AlertFired;
	out.alert.tsMs = e.tsMs;
	out.alert.ruleId = match->id;
	out.alert.rssi = e.rssi;
	bus.push(out);
}

void AlertEngine::onBleSighting(const BleSightingEvent& e, const RulesManager& rules, CoreState& state, EventBus& bus) {
	const OuiRule* match = rules.matchBle(e.addr48, e.oui24, e.name, e.hasMsdCompanyId, e.msdCompanyId, e.serviceUuids, e.serviceUuidCount);
	if (!match) return;
	if (state.settings.debugLevel > 1) {
		char mac[18];
		char oui[10];
		formatMac48_(e.addr48, mac, sizeof(mac));
		formatOui_(e.oui24, oui, sizeof(oui));
		Serial.print("[alert] ble  match rule=");
		Serial.print(match->id);
		Serial.print(" mac=");
		Serial.print(mac);
		Serial.print(" (");
		Serial.print(classifyBleAddr48_(e.addr48));
		Serial.print(")");
		Serial.print(" oui=");
		Serial.print(oui);
		Serial.print(" rssi=");
		Serial.println((int)e.rssi);
	}

	state.matches++;
	if (!shouldFire_(match->id, e.tsMs)) {
		if (state.settings.debugLevel > 1) Serial.println("[alert] rate-limited");
		return;
	}

	Event out{};
	out.type = EventType::AlertFired;
	out.alert.tsMs = e.tsMs;
	out.alert.ruleId = match->id;
	out.alert.rssi = e.rssi;
	bus.push(out);
}
