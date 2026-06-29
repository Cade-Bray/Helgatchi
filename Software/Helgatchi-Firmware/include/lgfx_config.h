#pragma once
#include <LovyanGFX.hpp>

// ST7789 240x280 on SPI2 (GPIO matrix) for XIAO ESP32-S3.
// Backlight (GPIO3) is NOT configured here — HAL drives it via LEDC PWM.
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789  _panel;
    lgfx::Bus_SPI       _bus;

public:
    LGFX() {
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = SPI2_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = 80000000;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    =  7;
            cfg.pin_mosi    =  9;
            cfg.pin_miso    = -1;
            cfg.pin_dc      =  1;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg         = _panel.config();
            cfg.pin_cs       = 44;
            cfg.pin_rst      =  8;
            cfg.pin_busy     = -1;
            cfg.panel_width  = 240;
            cfg.panel_height = 280;
            cfg.offset_x     =  0;
            cfg.offset_y     = 20;  // ST7789 GRAM starts 20 rows into 320-row array
            cfg.readable     = false;
            cfg.invert       = true;  // flip if colors are inverted on your panel
            cfg.rgb_order    = false;
            cfg.dlen_16bit   = false;
            cfg.bus_shared   = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};
