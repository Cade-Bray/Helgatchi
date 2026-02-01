#pragma once

// ============================================================================
// Pin configuration (mirrors Helgatchi Core)
// ============================================================================

// Buttons
#ifndef PIN_BTN_LEFT
#define PIN_BTN_LEFT 43
#endif

#ifndef PIN_BTN_RIGHT
#define PIN_BTN_RIGHT 5
#endif

#ifndef PIN_BTN_CENTER
#define PIN_BTN_CENTER 6
#endif

// Button electrical configuration
// If your button connects the GPIO to GND when pressed, use:
//   BUTTON_ACTIVE_LOW=1 and BUTTON_PULL_MODE=1 (internal pull-up)
#ifndef BUTTON_ACTIVE_LOW
#define BUTTON_ACTIVE_LOW 1
#endif

// 0=None, 1=PullUp, 2=PullDown
#ifndef BUTTON_PULL_MODE
#define BUTTON_PULL_MODE 1
#endif

// Backlight
#ifndef PIN_BACKLIGHT
#define PIN_BACKLIGHT 3
#endif
