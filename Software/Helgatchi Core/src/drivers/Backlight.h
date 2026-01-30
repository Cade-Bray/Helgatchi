
#pragma once

#include <cstdint>

class Backlight {
public:
	explicit Backlight(int pin, bool activeHigh = true) : pin_(pin), activeHigh_(activeHigh) {}

	void begin();
	void setOn(bool on);
	void setDimmed(bool dim);
	void setBrightness(uint8_t level); // 0=Min(10%), 1=Low, 2=Med, 3=High

private:
	void write_(bool on);
	void applyLevel_();

	int pin_;
	bool activeHigh_;
	bool on_ = false;
	bool dimmed_ = false;
	uint8_t brightness_ = 3; // 0=Min(26/10%), 1=Low(85), 2=Med(170), 3=High(255)
};

