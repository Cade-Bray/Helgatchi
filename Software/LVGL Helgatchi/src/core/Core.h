#pragma once

class LvglDriver;
class Buttons;

class Core {
public:
  Core();

  void setup();
  void loop();

private:
  LvglDriver* lvgl_;
  Buttons* buttons_;
};
