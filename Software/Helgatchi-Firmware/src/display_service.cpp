#include "display_service.h"
#include "hal.h"
#include "power_manager.h"
#include "settings_service.h"
#include <LovyanGFX.hpp>

DisplayService g_display;

// ---------------------------------------------------------------------------
// Layout — landscape after rotation: logical 280 x 240
// Two columns split at DIVIDER_X.
// ---------------------------------------------------------------------------
static constexpr int32_t SCR_W     = 280;
static constexpr int32_t DIVIDER_X = 140;
static constexpr int32_t CX        = SCR_W / 2;   // full-width centre (title only)
static constexpr int32_t CX_L      =  70;          // left column centre
static constexpr int32_t CX_R      = 210;          // right column centre

// Left column Y positions
static constexpr int32_t Y_TITLE    =   8;
static constexpr int32_t Y_DIV_1    =  40;
static constexpr int32_t Y_BATT_LBL =  48;
static constexpr int32_t Y_BATT_MV  =  66;
static constexpr int32_t Y_BATT_STS =  97;
static constexpr int32_t Y_DIV_2    = 120;
static constexpr int32_t Y_STAT_LBL = 128;
static constexpr int32_t Y_STAT_VAL = 146;
static constexpr int32_t Y_DIV_3    = 180;
static constexpr int32_t Y_CDWN_LBL = 188;
static constexpr int32_t Y_CDWN_VAL = 206;

// Right column Y positions (4 items, evenly spaced, no horizontal dividers)
static constexpr int32_t Y_USB_LBL    =  48;
static constexpr int32_t Y_USB_VAL    =  66;
static constexpr int32_t Y_SER_LBL    =  96;
static constexpr int32_t Y_SER_VAL    = 114;
static constexpr int32_t Y_SLPUSB_LBL = 144;
static constexpr int32_t Y_SLPUSB_VAL = 162;
static constexpr int32_t Y_SLPSER_LBL = 192;
static constexpr int32_t Y_SLPSER_VAL = 210;

// Clear-region heights
static constexpr int32_t H_LG = 28;  // Font4 value rows
static constexpr int32_t H_SM = 22;  // Font2 status rows

// Colors (RGB888)
static constexpr uint32_t C_BG     = 0x000000;
static constexpr uint32_t C_FG     = 0xFFFFFF;
static constexpr uint32_t C_LABEL  = 0x888888;
static constexpr uint32_t C_GREEN  = 0x00CC44;
static constexpr uint32_t C_YELLOW = 0xCCCC00;
static constexpr uint32_t C_RED    = 0xDD2222;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static inline void clearRowL(LGFX& tft, int32_t y, int32_t h) {
    tft.fillRect(0, y, DIVIDER_X, h, C_BG);
}
static inline void clearRowR(LGFX& tft, int32_t y, int32_t h) {
    tft.fillRect(DIVIDER_X + 1, y, SCR_W - DIVIDER_X - 1, h, C_BG);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DisplayService::begin(EventBus& bus) {
    _bus = &bus;
    bus.subscribe(EV_BATTERY_UPDATED,         this);
    bus.subscribe(EV_POWER_STATE_CHANGED,     this);
    bus.subscribe(EV_SLEEP_COUNTDOWN_UPDATED, this);
    bus.subscribe(EV_TICK_1S,                 this);
    bus.subscribe(EV_SETTINGS_CHANGED,        this);

    _usb_attached          = g_hal.usbAttached();
    _serial_connected      = (bool)Serial;
    _sleep_while_usb       = g_settings.getBool(SKEY_SLEEP_WHILE_USB);
    _no_sleep_while_serial = !g_settings.getBool(SKEY_DEBUG_SLEEP_WITH_SERIAL);

    _drawAll();
}

// ---------------------------------------------------------------------------
// IEventHandler
// ---------------------------------------------------------------------------

void DisplayService::onEvent(const Event& e) {
    switch (e.id) {
        case EV_BATTERY_UPDATED:
            _batt_mv  = e.data.battery.mv;
            _batt_pct = e.data.battery.pct;
            _drawBattery();
            {
                bool usb = g_hal.usbAttached();
                if (usb != _usb_attached) {
                    _usb_attached = usb;
                    _drawRightColumn();
                }
            }
            break;

        case EV_POWER_STATE_CHANGED:
            _pwr_state = e.data.power.state;
            _drawPowerState();
            break;

        case EV_SLEEP_COUNTDOWN_UPDATED:
            _countdown_s = e.data.sleep_count.seconds;
            _drawCountdown();
            break;

        case EV_TICK_1S: {
            bool ser = (bool)Serial;
            bool usb = g_hal.usbAttached();
            if (ser != _serial_connected || usb != _usb_attached) {
                _serial_connected = ser;
                _usb_attached     = usb;
                _drawRightColumn();
            }
            break;
        }

        case EV_SETTINGS_CHANGED:
            if (e.data.settings.mask & SMASK_POWER) {
                _sleep_while_usb       = g_settings.getBool(SKEY_SLEEP_WHILE_USB);
                _no_sleep_while_serial = !g_settings.getBool(SKEY_DEBUG_SLEEP_WITH_SERIAL);
                _drawRightColumn();
            }
            break;

        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void DisplayService::_drawAll() {
    LGFX& tft = g_hal.tft();
    tft.fillScreen(C_BG);

    // Title — full width centre
    tft.setTextColor(C_FG);
    tft.drawCentreString("HELGATCHI", CX, Y_TITLE, &lgfx::fonts::Font4);

    // Horizontal dividers — left column only (right column is unbroken)
    tft.drawFastHLine(10, Y_DIV_1, DIVIDER_X - 10, C_LABEL);
    tft.drawFastHLine(10, Y_DIV_2, DIVIDER_X - 10, C_LABEL);
    tft.drawFastHLine(10, Y_DIV_3, DIVIDER_X - 10, C_LABEL);

    // Vertical column divider
    tft.drawFastVLine(DIVIDER_X, Y_DIV_1, 240 - Y_DIV_1, C_LABEL);

    // Left column static labels
    tft.setTextColor(C_LABEL);
    tft.drawCentreString("BATTERY",  CX_L, Y_BATT_LBL, &lgfx::fonts::Font2);
    tft.drawCentreString("STATUS",   CX_L, Y_STAT_LBL, &lgfx::fonts::Font2);
    tft.drawCentreString("SLEEP IN", CX_L, Y_CDWN_LBL, &lgfx::fonts::Font2);

    // Right column static labels
    tft.drawCentreString("USB",     CX_R, Y_USB_LBL,    &lgfx::fonts::Font2);
    tft.drawCentreString("SERIAL",  CX_R, Y_SER_LBL,    &lgfx::fonts::Font2);
    tft.drawCentreString("SLP USB", CX_R, Y_SLPUSB_LBL, &lgfx::fonts::Font2);
    tft.drawCentreString("SLP SER", CX_R, Y_SLPSER_LBL, &lgfx::fonts::Font2);

    _drawBattery();
    _drawPowerState();
    _drawCountdown();
    _drawRightColumn();
}

void DisplayService::_drawBattery() {
    LGFX& tft = g_hal.tft();
    char buf[20];

    // mV value row
    clearRowL(tft, Y_BATT_MV, H_LG);
    if (_batt_pct == BATT_PCT_MISSING) {
        tft.setTextColor(C_YELLOW);
        tft.drawCentreString("NO BATT", CX_L, Y_BATT_MV, &lgfx::fonts::Font4);
    } else {
        tft.setTextColor(C_FG);
        snprintf(buf, sizeof(buf), "%u mV", _batt_mv);
        tft.drawCentreString(buf, CX_L, Y_BATT_MV, &lgfx::fonts::Font4);
    }

    // Charging / percentage status row
    clearRowL(tft, Y_BATT_STS, H_SM);
    if (_batt_pct == BATT_PCT_CHARGING) {
        tft.setTextColor(C_YELLOW);
        tft.drawCentreString("CHARGING", CX_L, Y_BATT_STS, &lgfx::fonts::Font2);
    } else if (_batt_pct == BATT_PCT_CHARGED) {
        tft.setTextColor(C_GREEN);
        tft.drawCentreString("CHARGED",  CX_L, Y_BATT_STS, &lgfx::fonts::Font2);
    } else if (_batt_pct == BATT_PCT_MISSING) {
        tft.setTextColor(C_YELLOW);
        tft.drawCentreString("USB ONLY", CX_L, Y_BATT_STS, &lgfx::fonts::Font2);
    } else {
        uint32_t col = (_batt_pct < 20) ? C_RED
                     : (_batt_pct < 50) ? C_YELLOW
                     : C_GREEN;
        tft.setTextColor(col);
        snprintf(buf, sizeof(buf), "%u%%", _batt_pct);
        tft.drawCentreString(buf, CX_L, Y_BATT_STS, &lgfx::fonts::Font2);
    }
}

void DisplayService::_drawPowerState() {
    LGFX& tft = g_hal.tft();
    clearRowL(tft, Y_STAT_VAL, H_LG);
    if (_pwr_state == POWER_AWAKE) {
        tft.setTextColor(C_GREEN);
        tft.drawCentreString("AWAKE",    CX_L, Y_STAT_VAL, &lgfx::fonts::Font4);
    } else {
        tft.setTextColor(C_RED);
        tft.drawCentreString("SLEEPING", CX_L, Y_STAT_VAL, &lgfx::fonts::Font4);
    }
}

void DisplayService::_drawCountdown() {
    LGFX& tft = g_hal.tft();
    clearRowL(tft, Y_CDWN_VAL, H_LG);
    tft.setTextColor(C_FG);
    char buf[12];
    snprintf(buf, sizeof(buf), "%u s", _countdown_s);
    tft.drawCentreString(buf, CX_L, Y_CDWN_VAL, &lgfx::fonts::Font4);
}

void DisplayService::_drawRightColumn() {
    LGFX& tft = g_hal.tft();

    // USB attached — green YES / red NO
    clearRowR(tft, Y_USB_VAL, H_LG);
    tft.setTextColor(_usb_attached ? C_GREEN : C_RED);
    tft.drawCentreString(_usb_attached ? "YES" : "NO", CX_R, Y_USB_VAL, &lgfx::fonts::Font4);

    // Serial connected — green YES / red NO
    clearRowR(tft, Y_SER_VAL, H_LG);
    tft.setTextColor(_serial_connected ? C_GREEN : C_RED);
    tft.drawCentreString(_serial_connected ? "YES" : "NO", CX_R, Y_SER_VAL, &lgfx::fonts::Font4);

    // Sleep when USB — green NO (inhibited) / red YES (sleep allowed)
    clearRowR(tft, Y_SLPUSB_VAL, H_LG);
    tft.setTextColor(_sleep_while_usb ? C_RED : C_GREEN);
    tft.drawCentreString(_sleep_while_usb ? "YES" : "NO", CX_R, Y_SLPUSB_VAL, &lgfx::fonts::Font4);

    // Sleep when serial — green NO (inhibited) / red YES (sleep allowed)
    clearRowR(tft, Y_SLPSER_VAL, H_LG);
    tft.setTextColor(_no_sleep_while_serial ? C_GREEN : C_RED);
    tft.drawCentreString(_no_sleep_while_serial ? "NO" : "YES", CX_R, Y_SLPSER_VAL, &lgfx::fonts::Font4);
}
