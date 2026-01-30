
#include "Backlight.h"

#include <Arduino.h>

void Backlight::begin() {
	pinMode(pin_, OUTPUT);
	// Default to OFF so timer-wake scan bursts stay dark.
	// The Core will explicitly enable the backlight in interactive/idle modes.
	setOn(false);
}

void Backlight::write_(bool on) {
	on_ = on;
	if (!on) {
		digitalWrite(pin_, activeHigh_ ? LOW : HIGH);
	} else {
		applyLevel_();
	}
}

void Backlight::applyLevel_() {
	if (!on_) {
		digitalWrite(pin_, activeHigh_ ? LOW : HIGH);
		return;
	}
	uint8_t pwm = 0;
	switch (brightness_) {
		case 0: pwm = 26; break;    // Min (10%)
		case 1: pwm = 85; break;    // Low (~33%)
		case 2: pwm = 170; break;   // Medium (~67%)
		case 3: pwm = 255; break;   // High (100%)
		default: pwm = 255; break;
	}
	if (!activeHigh_) {
		pwm = 255 - pwm;
	}
	analogWrite(pin_, pwm);
}

void Backlight::setOn(bool on) {
	dimmed_ = false;
	write_(on);
}

void Backlight::setDimmed(bool dim) {
	// Minimal: treat "dim" as ON for now (PWM can be added later).
	dimmed_ = dim;
	write_(true);
}

void Backlight::setBrightness(uint8_t level) {
	brightness_ = level;
	if (on_) {
		applyLevel_();
	}
}

