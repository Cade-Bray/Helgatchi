
#include "Display.h"

#include <Arduino.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

#include "../core/Config.h"

static Adafruit_ST7789* g_tft = nullptr;
static bool g_ready = false;
static int g_w = TFT_WIDTH;
static int g_h = TFT_HEIGHT;
static bool s_debugPerformance = false;

void Display::begin() {
	uint32_t _t0 = s_debugPerformance ? micros() : 0;
	
	if (PIN_TFT_CS < 0 || PIN_TFT_DC < 0) {
		Serial.println("[display] PIN_TFT_CS/PIN_TFT_DC not set; display disabled");
		g_ready = false;
		return;
	}

	// Route SPI to the pins we wired.
	SPI.begin(PIN_TFT_SCLK, PIN_TFT_MISO, PIN_TFT_MOSI, PIN_TFT_CS);

	if (!g_tft) {
		g_tft = new Adafruit_ST7789(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
	}

	// NOTE: Dimensions depend on your panel; adjust in Config.h if needed.
	g_tft->init(g_w, g_h);
	g_tft->setSPISpeed(TFT_SPI_HZ);
	g_tft->setRotation(TFT_ROTATION);

	// After rotation, the logical width/height may swap.
	g_w = g_tft->width();
	g_h = g_tft->height();

	g_tft->fillScreen(ST77XX_BLACK);
	g_tft->setTextWrap(false);
	g_ready = true;
	
	if (s_debugPerformance && _t0 > 0) {
		Serial.printf("[PERF] display_begin: %lu us\n", (unsigned long)(micros() - _t0));
	}
}

void Display::clear() {
	if (!g_ready) return;
	uint32_t _t0 = s_debugPerformance ? micros() : 0;
	g_tft->fillScreen(ST77XX_BLACK);
	if (s_debugPerformance && _t0 > 0) {
		Serial.printf("[PERF] display_clear: %lu us\n", (unsigned long)(micros() - _t0));
	}
}

void Display::fillRect(int x, int y, int w, int h, uint16_t rgb565) {
	if (!g_ready) return;
	if (w <= 0 || h <= 0) return;
	uint32_t _t0 = s_debugPerformance ? micros() : 0;
	g_tft->fillRect(x, y, w, h, rgb565);
	if (s_debugPerformance && _t0 > 0) {
		Serial.printf("[PERF] display_fillRect(%d,%d,%d,%d): %lu us\n", x, y, w, h, (unsigned long)(micros() - _t0));
	}
}

void Display::drawText(int x, int y, const char* text, int size) {
	if (!g_ready) {
		// Keep this quiet-ish; uncomment to debug rendering calls.
		// Serial.printf("[display] (%d,%d) %s\n", x, y, text);
		return;
	}

	uint32_t _t0 = s_debugPerformance ? micros() : 0;
	g_tft->setCursor(x, y);
	g_tft->setTextSize(size);
	g_tft->setTextColor(ST77XX_WHITE, ST77XX_BLACK);
	g_tft->print(text);
	if (s_debugPerformance && _t0 > 0) {
		Serial.printf("[PERF] display_drawText(%d,%d,sz=%d): %lu us\n", x, y, size, (unsigned long)(micros() - _t0));
	}
}

void Display::drawTextColored(int x, int y, const char* text, int size, uint16_t fgRgb565, uint16_t bgRgb565) {
	if (!g_ready) return;
	uint32_t _t0 = s_debugPerformance ? micros() : 0;
	g_tft->setCursor(x, y);
	g_tft->setTextSize(size);
	g_tft->setTextColor(fgRgb565, bgRgb565);
	g_tft->print(text);
	if (s_debugPerformance && _t0 > 0) {
		Serial.printf("[PERF] display_drawTextColored(%d,%d,sz=%d): %lu us\n", x, y, size, (unsigned long)(micros() - _t0));
	}
}

void Display::drawText(int x, int y, const std::string& text, int size) {
	drawText(x, y, text.c_str(), size);
}

void Display::drawTextColored(int x, int y, const std::string& text, int size, uint16_t fgRgb565, uint16_t bgRgb565) {
	drawTextColored(x, y, text.c_str(), size, fgRgb565, bgRgb565);
}

int Display::width() const {
	return g_w;
}

int Display::height() const {
	return g_h;
}

void Display::setDebugPerformance(bool enabled) {
	s_debugPerformance = enabled;
}

bool Display::debugPerformance() {
	return s_debugPerformance;
}

