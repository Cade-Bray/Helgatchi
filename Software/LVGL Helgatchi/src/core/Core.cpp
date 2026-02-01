#include "Core.h"

#include <Arduino.h>

#include "Config.h"
#include "../input/Buttons.h"
#include "../ui/lvgl/LvglDriver.h"

static Buttons::Pull pullFromConfig() {
  switch (BUTTON_PULL_MODE) {
    case 1:
      return Buttons::Pull::PullUp;
    case 2:
      return Buttons::Pull::PullDown;
    default:
      return Buttons::Pull::None;
  }
}

Core::Core() : lvgl_(nullptr), buttons_(nullptr) {}

void Core::setup() {
  Serial.println();
  Serial.println("[core] setup");

  static LvglDriver lvgl_driver;
  static Buttons buttons_driver(PIN_BTN_LEFT, PIN_BTN_RIGHT, PIN_BTN_CENTER, pullFromConfig(), BUTTON_ACTIVE_LOW);

  lvgl_ = &lvgl_driver;
  buttons_ = &buttons_driver;

  buttons_->begin();
  lvgl_->begin(*buttons_);
}

void Core::loop() {
  if (buttons_) {
    buttons_->poll();
  }
  if (lvgl_) {
    lvgl_->tick();
  }
}
