#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace pins {
constexpr int LCD_MOSI = 11;
constexpr int LCD_MISO = 13;
constexpr int LCD_SCLK = 12;
constexpr int LCD_CS = 10;
constexpr int LCD_DC = 46;
constexpr int LCD_BL = 45;
constexpr int TP_SDA = 16;
constexpr int TP_SCL = 15;
constexpr int TP_INT = 17;
constexpr int TP_RST = 18;
constexpr int AMP_EN = 1;
constexpr int BOOT_BTN = 0;
}  // namespace pins

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_FT5x06 touch_;

 public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = pins::LCD_SCLK;
      cfg.pin_mosi = pins::LCD_MOSI;
      cfg.pin_miso = pins::LCD_MISO;
      cfg.pin_dc = pins::LCD_DC;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = pins::LCD_CS;
      cfg.pin_rst = -1;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.readable = true;
      cfg.bus_shared = false;
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = pins::LCD_BL;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    {
      auto cfg = touch_.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = 0x38;
      cfg.pin_sda = pins::TP_SDA;
      cfg.pin_scl = pins::TP_SCL;
      cfg.pin_int = pins::TP_INT;
      cfg.pin_rst = pins::TP_RST;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.x_max = 239;
      cfg.y_min = 0;
      cfg.y_max = 319;
      cfg.offset_rotation = 2;
      cfg.bus_shared = false;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};
