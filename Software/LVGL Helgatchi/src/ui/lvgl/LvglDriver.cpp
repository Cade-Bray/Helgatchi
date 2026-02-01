#include "LvglDriver.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>

#include "../../input/Buttons.h"
#include "LvglContext.h"
#include "../screens/MainMenuScreen.h"
#include "../xml/XmlSupport.h"
#include "../xml/XmlFs.h"

// --- Display pin definitions (SPI) ---
// These match the wiring used in the earlier ST7789 SPI test project.
// Update them if your wiring differs.
static constexpr int TFT_CS   = 44;
static constexpr int TFT_DC   = 1;
static constexpr int TFT_RST  = 8;
static constexpr int TFT_MOSI = 9;
static constexpr int TFT_SCLK = 7;
static constexpr int TFT_BL   = 3;

// --- Panel settings ---
// This panel is 240x280 and is rotated once (90 degrees).
// LVGL uses the rotated width/height as the display resolution.
static constexpr int PANEL_WIDTH  = 240;
static constexpr int PANEL_HEIGHT = 280;
static constexpr int PANEL_ROTATION = 3; // rotated 180 degrees
// ST7789 240x280 panels often have a 20px hidden area.
// When rotated, that hidden area shows up as a white bar unless we offset it.
// If the bar is on the right, shift the Y offset when rotated.
// If you still see a bar, adjust these offsets by +/-20.
static constexpr int PANEL_OFFSET_X = 0;
static constexpr int PANEL_OFFSET_Y = 20;
static constexpr int DISP_HOR = (PANEL_ROTATION % 2 == 0) ? PANEL_WIDTH : PANEL_HEIGHT;
static constexpr int DISP_VER = (PANEL_ROTATION % 2 == 0) ? PANEL_HEIGHT : PANEL_WIDTH;

// LovyanGFX device wrapper for an SPI ST7789 panel.
// This is a minimal setup following the LovyanGFX docs for ESP32 + SPI panels.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ST7789 _panel;

public:
  LGFX() {
    {
      // SPI bus configuration (ESP32-S3 uses SPI2_HOST by default).
      // Keep this simple and explicit to match typical LVGL/LovyanGFX examples.
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = TFT_SCLK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc   = TFT_DC;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      // Panel configuration for ST7789.
      // These are the minimum fields needed for a basic SPI panel.
      auto pcfg = _panel.config();
      pcfg.pin_cs = TFT_CS;
      pcfg.pin_rst = TFT_RST;
      pcfg.pin_busy = -1;

      pcfg.panel_width = PANEL_WIDTH;
      pcfg.panel_height = PANEL_HEIGHT;
      // Apply panel offsets to hide the non-visible strip.
      pcfg.offset_x = PANEL_OFFSET_X;
      pcfg.offset_y = PANEL_OFFSET_Y;
      pcfg.offset_rotation = 0;

      pcfg.readable = false;
      pcfg.invert = false;
      pcfg.rgb_order = false;
      pcfg.dlen_16bit = false;
      pcfg.bus_shared = false;

      _panel.config(pcfg);
    }

    setPanel(&_panel);
  }
};

static LGFX tft;

// Two small line buffers for LVGL partial rendering.
// Keeping the buffers modest helps RAM usage on microcontrollers.
static lv_color_t buf1[DISP_HOR * 40];
static lv_color_t buf2[DISP_HOR * 40];

// LVGL flush callback: copy a rendered area to the display.
// LVGL calls this whenever a region needs to be updated.
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
  const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
  const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

  // Push the rendered pixels to the display.
  tft.startWrite();
  tft.setAddrWindow((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)w, (uint16_t)h);
  tft.pushPixels(reinterpret_cast<lgfx::rgb565_t *>(color_p), w * h);
  tft.endWrite();

  // Tell LVGL we are done with this flush.
  lv_display_flush_ready(disp);
}

// Button driver pointer used by the LVGL input read callback.
static Buttons *g_buttons = nullptr;

// Track a pending key release so LVGL sees a full press/release cycle.
static bool g_pending_release = false;
static uint32_t g_pending_key = 0;

static uint32_t mapButtonToKey(const Buttons::ButtonEvent &ev) {
  switch (ev.id) {
    case Buttons::ButtonId::Left:
      return LV_KEY_LEFT;
    case Buttons::ButtonId::Right:
      return LV_KEY_RIGHT;
    case Buttons::ButtonId::Center:
      return (ev.action == Buttons::ButtonAction::LongPress) ? LV_KEY_ESC : LV_KEY_ENTER;
    default:
      return LV_KEY_ENTER;
  }
}

// LVGL keypad input callback.
static void lvgl_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;

  if (g_pending_release) {
    data->key = g_pending_key;
    data->state = LV_INDEV_STATE_RELEASED;
    g_pending_release = false;
    return;
  }

  if (!g_buttons) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  Buttons::ButtonEvent ev{};
  if (g_buttons->popEvent(ev)) {
    data->key = mapButtonToKey(ev);
    data->state = LV_INDEV_STATE_PRESSED;
    g_pending_key = data->key;
    g_pending_release = true;
    return;
  }

  data->state = LV_INDEV_STATE_RELEASED;
}

void LvglDriver::begin(Buttons& buttons) {
  Serial.println("[lvgl] init");

  g_buttons = &buttons;

  // Backlight control (simple on/off).
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  // Initialize the display driver and set the rotation.
  tft.init();
  tft.setRotation(PANEL_ROTATION);
  tft.fillScreen(TFT_BLACK);

  // Initialize LVGL core.
  lv_init();

  // Initialize LVGL XML support and load XML assets if present.
  if (xml_support_init()) {
    char xml_path[16];
    snprintf(xml_path, sizeof(xml_path), "%c:/ui", xml_fs_drive_letter());
    xml_support_load_all(xml_path);
  }

  // Create the LVGL display instance and attach the flush callback.
  lv_display_t *disp = lv_display_create(DISP_HOR, DISP_VER);
  lv_display_set_flush_cb(disp, lvgl_flush_cb);
  // Provide LVGL with two draw buffers for partial rendering.
  lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);

  // Create a focus group for keypad navigation.
  lv_group_t *group = lv_group_create();
  lv_group_set_default(group);
  lv_group_set_wrap(group, true);
  lvgl_set_input_group(group);

  // Create a keypad input device and attach our read callback.
  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(indev, lvgl_indev_read_cb);
  lv_indev_set_group(indev, group);

  // Create and load the initial screen (XML overrides C if available).
  lv_obj_t *screen = xml_support_try_create_screen("main_menu");
  if (!screen) {
    screen = create_main_menu_screen();
  }
  lv_scr_load(screen);
}

void LvglDriver::tick() {
  // Drive LVGL's internal time base and task handler.
  // This is the minimal loop recommended by the LVGL docs.
  static uint32_t last_tick = 0;
  const uint32_t now = millis();
  if (last_tick == 0) last_tick = now;
  lv_tick_inc(now - last_tick);
  last_tick = now;

  lv_timer_handler();
  delay(5);
}

