
#include "VibrationMotor.h"

#include <Arduino.h>

const uint16_t VibrationMotor::kBoot_[] = {120, 80, 220, 0};
const uint16_t VibrationMotor::kAlert_[] = {140, 70, 140, 70, 140, 0};
const uint16_t VibrationMotor::kHeartbeat_[] = {80, 80, 80, 0};

void VibrationMotor::begin() {
  if (pin_ < 0) {
    pattern_ = nullptr;
    idx_ = 0;
    onPhase_ = false;
    phaseStartMs_ = millis();
    return;
  }

  pinMode(pin_, OUTPUT);
  stop_();
}

void VibrationMotor::setOn_(bool on) {
  if (pin_ < 0) return;
  const bool level = activeHigh_ ? on : !on;
  digitalWrite(pin_, level ? HIGH : LOW);
}

void VibrationMotor::stop_() {
  pattern_ = nullptr;
  idx_ = 0;
  onPhase_ = false;
  phaseStartMs_ = millis();
  setOn_(false);
}

void VibrationMotor::startPattern_(const uint16_t* pattern) {
  pattern_ = pattern;
  idx_ = 0;
  onPhase_ = true;
  phaseStartMs_ = millis();
  setOn_(true);
}

void VibrationMotor::playBootPattern() { startPattern_(kBoot_); }
void VibrationMotor::playAlertPattern() { startPattern_(kAlert_); }
void VibrationMotor::playHeartbeatPattern() { startPattern_(kHeartbeat_); }

void VibrationMotor::tick(uint32_t nowMs) {
  if (!pattern_) return;

  const uint16_t dur = pattern_[idx_];
  if (dur == 0) {
    stop_();
    return;
  }

  if ((nowMs - phaseStartMs_) < dur) return;

  phaseStartMs_ = nowMs;
  idx_++;
  onPhase_ = !onPhase_;
  setOn_(onPhase_);
}
