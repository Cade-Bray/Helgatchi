#pragma once
#include <stdint.h>
#include <string>

class Display {
public:
  void begin();
  void clear();
  void fillRect(int x, int y, int w, int h, uint16_t rgb565 = 0x0000);
  void drawText(int x, int y, const char* text, int size = 2);
  void drawText(int x, int y, const std::string& text, int size = 2);

  // Draw text with a custom foreground/background (RGB565).
  void drawTextColored(int x, int y, const char* text, int size, uint16_t fgRgb565, uint16_t bgRgb565 = 0x0000);
  void drawTextColored(int x, int y, const std::string& text, int size, uint16_t fgRgb565, uint16_t bgRgb565 = 0x0000);
  int width() const;
  int height() const;
  
  static void setDebugPerformance(bool enabled);
  static bool debugPerformance();

  // TODO: expose primitive drawing used by screens
};