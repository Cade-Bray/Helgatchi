#include "Buttons.h"

Buttons::Buttons(int leftPin, int rightPin, int centerPin, Pull pull, bool activeLow)
    : leftPin_(leftPin), rightPin_(rightPin), centerPin_(centerPin), pull_(pull), activeLow_(activeLow) {}

void Buttons::begin() {
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

void Buttons::poll() {
  const uint32_t now = millis();
  handleOne_(left_, leftPin_, ButtonId::Left, now);
  handleOne_(right_, rightPin_, ButtonId::Right, now);
  handleOne_(center_, centerPin_, ButtonId::Center, now);
}

bool Buttons::popEvent(ButtonEvent &out) {
  if (qHead_ == qTail_) return false;
  out = queue_[qTail_];
  qTail_ = (uint8_t)((qTail_ + 1) % kQueueSize);
  return true;
}

uint8_t Buttons::pinModeForPull_(Pull pull) {
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

bool Buttons::isPressed_(bool readLevel) const {
  return activeLow_ ? (readLevel == LOW) : (readLevel == HIGH);
}

void Buttons::pushEvent_(ButtonId id, ButtonAction action) {
  queuePush_({id, action});
}

bool Buttons::queuePush_(const ButtonEvent &e) {
  uint8_t next = (uint8_t)((qHead_ + 1) % kQueueSize);
  if (next == qTail_) {
    // Queue full, drop the event (simple policy for now).
    return false;
  }
  queue_[qHead_] = e;
  qHead_ = next;
  return true;
}

void Buttons::handleOne_(BtnState &s, int pin, ButtonId id, uint32_t nowMs) {
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
    if (pressed) {
      s.downMs = nowMs;
      s.longFired = false;
    } else {
      if (!s.longFired) {
        pushEvent_(id, ButtonAction::Press);
      }
    }
  }

  const bool pressedStable = isPressed_(s.lastStable);
  if (pressedStable && !s.longFired && (nowMs - s.downMs) >= kLongPressMs) {
    s.longFired = true;
    pushEvent_(id, ButtonAction::LongPress);
  }
}
