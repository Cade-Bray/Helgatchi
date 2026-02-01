#pragma once

#include <Arduino.h>
#include <cstdint>

// Simple debounced button driver with short/long press events.
// Mirrors Helgatchi Core behavior but keeps the interface minimal.

class Buttons {
public:
  enum class Pull : uint8_t {
    None = 0,
    PullUp = 1,
    PullDown = 2,
  };

  enum class ButtonId : uint8_t { Left, Right, Center };
  enum class ButtonAction : uint8_t { Press, LongPress };

  struct ButtonEvent {
    ButtonId id;
    ButtonAction action;
  };

  Buttons(int leftPin, int rightPin, int centerPin, Pull pull, bool activeLow);

  void begin();
  void poll();

  // Returns true if an event was popped.
  bool popEvent(ButtonEvent &out);

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

  static uint8_t pinModeForPull_(Pull pull);
  bool isPressed_(bool readLevel) const;
  void handleOne_(BtnState &s, int pin, ButtonId id, uint32_t nowMs);
  void pushEvent_(ButtonId id, ButtonAction action);

  bool queuePush_(const ButtonEvent &e);

  int leftPin_;
  int rightPin_;
  int centerPin_;
  Pull pull_;
  bool activeLow_;

  BtnState left_{};
  BtnState right_{};
  BtnState center_{};

  static constexpr uint8_t kQueueSize = 8;
  ButtonEvent queue_[kQueueSize]{};
  uint8_t qHead_ = 0;
  uint8_t qTail_ = 0;
};
