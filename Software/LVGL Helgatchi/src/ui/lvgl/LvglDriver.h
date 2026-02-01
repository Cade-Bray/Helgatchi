#pragma once

#include <cstdint>

class Buttons;

class LvglDriver {
public:
  void begin(Buttons& buttons);
  void tick();
};
