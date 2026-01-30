
#pragma once

#include <stdint.h>

class VibrationMotor {
public:
	explicit VibrationMotor(int pin, bool activeHigh = true) : pin_(pin), activeHigh_(activeHigh) {}

	void begin();

	void playBootPattern();
	void playAlertPattern();
	void playHeartbeatPattern();

	void tick(uint32_t nowMs);

private:
	void setOn_(bool on);
	void startPattern_(const uint16_t* pattern);
	void stop_();

	int pin_;
	bool activeHigh_;

	const uint16_t* pattern_ = nullptr;
	uint8_t idx_ = 0;
	bool onPhase_ = false;
	uint32_t phaseStartMs_ = 0;

	static const uint16_t kBoot_[];
	static const uint16_t kAlert_[];
	static const uint16_t kHeartbeat_[];
};

