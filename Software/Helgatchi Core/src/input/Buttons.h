
#pragma once

#include <Arduino.h>

#include "../core/EventBus.h"
#include "../core/Events.h"

class Buttons {
public:
	enum class Pull : uint8_t {
		None = 0,
		PullUp = 1,
		PullDown = 2,
	};

	Buttons(int leftPin, int rightPin, int centerPin, Pull pull = Pull::PullUp,
	        bool activeLow = true)
		: leftPin_(leftPin), rightPin_(rightPin), centerPin_(centerPin), pull_(pull),
		  activeLow_(activeLow) {}

	void setDebug(bool enabled) { debug_ = enabled; }

	void begin() {
		const uint8_t mode = pinModeForPull_(pull_);
		pinMode(leftPin_, mode);
		pinMode(rightPin_, mode);
		pinMode(centerPin_, mode);

		left_.lastStable = digitalRead(leftPin_);
		right_.lastStable = digitalRead(rightPin_);
		center_.lastStable = digitalRead(centerPin_);
		left_.lastRead = left_.lastStable;
		right_.lastRead = right_.lastStable;
		center_.lastRead = center_.lastStable;

		const uint32_t now = millis();
		left_.lastChangeMs = right_.lastChangeMs = center_.lastChangeMs = now;
	}

	void poll(EventBus& bus) {
		const uint32_t now = millis();
		handleOne_(bus, left_, leftPin_, ButtonId::Left, now);
		handleOne_(bus, right_, rightPin_, ButtonId::Right, now);
		handleOne_(bus, center_, centerPin_, ButtonId::Center, now);
	}

private:
	struct BtnState {
		bool lastStable = true;
		bool lastRead = true;
		uint32_t lastChangeMs = 0;
		uint32_t downMs = 0;
		bool longFired = false;
	};

	static constexpr uint32_t kDebounceMs = 30;
	static constexpr uint32_t kLongPressMs = 1200;

	static uint8_t pinModeForPull_(Pull pull) {
		switch (pull) {
		case Pull::PullUp:
			return INPUT_PULLUP;
		case Pull::PullDown:
			#ifdef INPUT_PULLDOWN
			return INPUT_PULLDOWN;
			#else
			return INPUT;
			#endif
		case Pull::None:
		default:
			return INPUT;
		}
	}

	bool isPressed_(bool readLevel) const {
		return activeLow_ ? (readLevel == LOW) : (readLevel == HIGH);
	}

	void pushButton_(EventBus& bus, ButtonId id, ButtonAction action) {
		Event e{};
		e.type = EventType::Button;
		e.button = {id, action};
		bus.push(e);
	}

	void handleOne_(EventBus& bus, BtnState& s, int pin, ButtonId id, uint32_t nowMs) {
		if (pin < 0) return;
		const bool read = digitalRead(pin);
		if (read != s.lastRead) {
			s.lastRead = read;
			s.lastChangeMs = nowMs;
		}

		if ((nowMs - s.lastChangeMs) < kDebounceMs) return;

		if (read != s.lastStable) {
			s.lastStable = read;
			const bool pressed = isPressed_(read);
			if (debug_) {
				Serial.print("[btn] ");
				Serial.print(buttonName_(id));
				Serial.print(" pin=");
				Serial.print(pin);
				Serial.print(" ");
				Serial.println(pressed ? "DOWN" : "UP");
			}
			if (pressed) {
				s.downMs = nowMs;
				s.longFired = false;
			} else {
				if (!s.longFired) {
					pushButton_(bus, id, ButtonAction::Press);
				}
			}
		}

		const bool pressedStable = isPressed_(s.lastStable);
		if (pressedStable && !s.longFired && (nowMs - s.downMs) >= kLongPressMs) {
			s.longFired = true;
			if (debug_) {
				Serial.print("[btn] ");
				Serial.print(buttonName_(id));
				Serial.println(" LONG");
			}
			pushButton_(bus, id, ButtonAction::LongPress);
		}
	}

	static const char* buttonName_(ButtonId id) {
		switch (id) {
		case ButtonId::Left:
			return "L";
		case ButtonId::Right:
			return "R";
		case ButtonId::Center:
			return "C";
		default:
			return "?";
		}
	}

private:
	int leftPin_;
	int rightPin_;
	int centerPin_;
	Pull pull_;
	bool activeLow_;
	bool debug_ = false;

	BtnState left_{};
	BtnState right_{};
	BtnState center_{};
};
