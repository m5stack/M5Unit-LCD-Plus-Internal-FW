//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/ledc.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cstring>

#include <Arduino.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_M5UnitLCD.hpp>
#include <lgfx/v1/panel/Panel_ST7789.hpp>
#include <lgfx/v1/platforms/esp32/Bus_SPI.hpp>

#include "common.hpp"
#include "board_config.hpp"
#include "cpu_clock.hpp"
#include "encoder_input.hpp"
#include "i2c_slave.hpp"
#include "unit_lcd_plus_protocol.hpp"
#include "update.hpp"
#include "command_processor.hpp"

namespace command_processor
{
  static constexpr char NVS_KEY_I2CADDR[] = "ADDR";
  /// Kept in DRAM because the IRAM-resident I2C ISR reads it. Left in flash rodata, a READ_ID
  /// arriving during an OTA flash write (cache disabled) would panic.
  static const DRAM_ATTR std::uint8_t read_id_data[] = { 0x77, 0x89, FIRMWARE_MAJOR_VERSION, FIRMWARE_MINOR_VERSION };

  static constexpr i2c_port_t I2C_PORT = (i2c_port_t)I2C_NUM_0;
  static constexpr std::uint8_t I2C_DEFAULT_ADDR = 0x3E;
  static constexpr std::uint8_t I2C_MIN_ADDR = 0x08;
  static constexpr std::uint8_t I2C_MAX_ADDR = 0x77;
  static constexpr std::size_t RX_BUFFER_MAX = 0x2000;
  static constexpr std::size_t PARAM_MAXLEN = 12;


  volatile std::size_t _rx_buffer_setpos = 0;
  volatile std::size_t _rx_buffer_getpos = 0;

  std::uint8_t _rx_buffer[RX_BUFFER_MAX][PARAM_MAXLEN];

  std::uint8_t* _params = _rx_buffer[0];
  std::size_t _param_index = 0;
  std::size_t _param_need_count = 1;
  std::size_t _param_resetindex = 0;
  std::size_t _rle_abs = 0;
  std::uint32_t _argb8888 = ~0u;
  std::uint8_t _i2c_addr = I2C_DEFAULT_ADDR;
  /// New address requested by CHANGE_ADDR. _i2c_addr itself is not changed until the NVS
  /// save result is known, so operation continues on the current address if the save fails.
  std::uint8_t _i2c_addr_request = 0;
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _spi_bus;
  LGFX_Device _lcd;
  LGFX_Sprite _canvas;
  bool _byteswap = false;

  bool _modified = true;
  bool _dirty_valid = false;
  std::uint_fast16_t _dirty_xs = 0;
  std::uint_fast16_t _dirty_ys = 0;
  std::uint_fast16_t _dirty_xe = 0;
  std::uint_fast16_t _dirty_ye = 0;
  bool _partial_flush_enabled = true;

  bool _nvs_push = false;

  static inline void mark_modified(void)
  {
    _modified = true;
  }

  static inline void mark_modified_rect(std::int32_t x, std::int32_t y,
                                        std::int32_t w, std::int32_t h)
  {
    if (!_partial_flush_enabled)
    {
      _dirty_xs = 0;
      _dirty_ys = 0;
      _dirty_xe = board_config::LCD_WIDTH - 1;
      _dirty_ye = board_config::LCD_HEIGHT - 1;
      _dirty_valid = true;
      mark_modified();
      return;
    }
    if (w <= 0 || h <= 0) return;
    const auto xe = x + w - 1;
    const auto ye = y + h - 1;
    x = std::max<std::int32_t>(0, x);
    y = std::max<std::int32_t>(0, y);
    const auto max_x = static_cast<std::int32_t>(board_config::LCD_WIDTH) - 1;
    const auto max_y = static_cast<std::int32_t>(board_config::LCD_HEIGHT) - 1;
    const auto clipped_xe = std::min<std::int32_t>(max_x, xe);
    const auto clipped_ye = std::min<std::int32_t>(max_y, ye);
    if (x > clipped_xe || y > clipped_ye) return;
    if (!_dirty_valid)
    {
      _dirty_xs = x; _dirty_ys = y;
      _dirty_xe = clipped_xe; _dirty_ye = clipped_ye;
      _dirty_valid = true;
    }
    else
    {
      _dirty_xs = std::min<std::uint_fast16_t>(_dirty_xs, x);
      _dirty_ys = std::min<std::uint_fast16_t>(_dirty_ys, y);
      _dirty_xe = std::max<std::uint_fast16_t>(_dirty_xe, clipped_xe);
      _dirty_ye = std::max<std::uint_fast16_t>(_dirty_ye, clipped_ye);
    }
    mark_modified();
  }

  static inline void mark_full_modified(void)
  {
    _dirty_xs = 0;
    _dirty_ys = 0;
    _dirty_xe = board_config::LCD_WIDTH - 1;
    _dirty_ye = board_config::LCD_HEIGHT - 1;
    _dirty_valid = true;
    mark_modified();
  }

  enum firmupdate_state_t
  {
    nothing ,       // コマンド未受信
    wait_data ,     // データ待機
    progress ,      // データ受信中
    sector_write ,  // セクタブロックのフラッシュ書き込み
    finish ,        // 全行程終了
  };
  firmupdate_state_t _firmupdate_state = nothing;
  std::size_t _firmupdate_index = 0;
  std::size_t _firmupdate_totalsize = 0;
  std::size_t _firmupdate_result = 0;
  std::size_t _last_command = 0;
  std::uint8_t _encoder_state_response[unit_lcd_plus_protocol::ENCODER_STATE_SIZE] = {};
  std::uint8_t _encoder_event_response[unit_lcd_plus_protocol::ENCODER_EVENT_SIZE] = {};
  std::uint8_t _button_config_response[unit_lcd_plus_protocol::BUTTON_CONFIG_SIZE] = {};

  std::uint_fast8_t _brightness = 128;
  bool _backlight_pwm_attached = false;

  static void set_backlight(std::uint8_t brightness)
  {
    constexpr ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE;
    constexpr ledc_timer_t timer = LEDC_TIMER_0;
    constexpr ledc_channel_t channel = LEDC_CHANNEL_0;

    if (!_backlight_pwm_attached)
    {
      ledc_timer_config_t timer_config = {};
      timer_config.speed_mode = speed_mode;
      timer_config.duty_resolution = LEDC_TIMER_8_BIT;
      timer_config.timer_num = timer;
      timer_config.freq_hz = 5000;
      timer_config.clk_cfg = LEDC_AUTO_CLK;

      ledc_channel_config_t channel_config = {};
      channel_config.gpio_num = board_config::PIN_LCD_BL;
      channel_config.speed_mode = speed_mode;
      channel_config.channel = channel;
      channel_config.intr_type = LEDC_INTR_DISABLE;
      channel_config.timer_sel = timer;
      channel_config.duty = brightness;
      channel_config.hpoint = 0;

      _backlight_pwm_attached = ledc_timer_config(&timer_config) == ESP_OK
                             && ledc_channel_config(&channel_config) == ESP_OK;
    }

    if (_backlight_pwm_attached
     && ledc_set_duty(speed_mode, channel, brightness) == ESP_OK
     && ledc_update_duty(speed_mode, channel) == ESP_OK)
    {
      return;
    }

    gpio_set_direction(board_config::PIN_LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(board_config::PIN_LCD_BL, brightness ? 1 : 0);
  }

  static void flush_canvas_to_lcd(void)
  {
    // Keep the original, known-good framebuffer commit path. The sprite
    // buffer is already in the panel's native 24-bit stream format.
    _lcd.startWrite();
    _lcd.setWindow(0, 0, board_config::LCD_WIDTH - 1,
                   board_config::LCD_HEIGHT - 1);
    _spi_bus.writeBytes(static_cast<const std::uint8_t*>(_canvas.getBuffer()),
                        _canvas.bufferLength(), true, false);
    _lcd.endWrite();
    _dirty_valid = false;
  }

  std::uint_fast16_t _xs = 0;
  std::uint_fast16_t _xe = 0;
  std::uint_fast16_t _ys = 0;
  std::uint_fast16_t _ye = 0;
  std::uint_fast16_t _xptr = 0;
  std::uint_fast16_t _yptr = 0;

  // read処理はISRで、write処理はメインスレッドで実行される。
  // 異なるタイミングで実行されるため、現在参照しているピクセル座標はwriteとreadで別々に管理する。
  std::uint_fast16_t _read_xs = 0;
  std::uint_fast16_t _read_ys = 0;
  std::uint_fast16_t _read_xe = 0;
  std::uint_fast16_t _read_ye = 0;
  std::uint_fast16_t _read_xptr = 0;
  std::uint_fast16_t _read_yptr = 0;


  #if DEBUG == 1
  std::uint8_t cmd_detect[256] = {0};
  #endif

  /// Saves the address to NVS. Returns true only if open / set / commit all succeed.
  static bool IRAM_ATTR save_nvs(std::uint8_t addr)
  {
    i2c_slave::stop_isr();
// #if DEBUG == 1
// lgfx::gpio_hi(0);
// #endif

    std::uint32_t handle = 0;
    bool result = (ESP_OK == nvs_open(LOGNAME, NVS_READWRITE, &handle));
    if (result)
    {
      result = (ESP_OK == nvs_set_u8(handle, NVS_KEY_I2CADDR, addr))
            && (ESP_OK == nvs_commit(handle));
      nvs_close(handle);
    }
    i2c_slave::start_isr();

// #if DEBUG == 1
// lgfx::gpio_lo(0);
// #endif
    return result;
  }

  static void IRAM_ATTR update_argb8888(const std::uint8_t* data, std::size_t len)
  {
    if (_byteswap)
    {
      switch (len)
      {
        default: return;
        case 1: _argb8888 =   0xFF  << 24 | lgfx::convert_to_rgb888((std::uint8_t ) data[0]);                          return;
        case 2: _argb8888 =   0xFF  << 24 | lgfx::convert_to_rgb888((std::uint16_t)(data[1]<< 8|data[0]));             return;
        case 3: _argb8888 =   0xFF  << 24 | lgfx::convert_to_rgb888((std::uint32_t)(data[2]<<16|data[1]<<8|data[0]));  return;
        case 4: _argb8888 = data[3] << 24 | lgfx::convert_to_rgb888((std::uint32_t)(data[2]<<16|data[1]<<8|data[0]));  return;
      }
    }
    else
    {
      switch (len)
      {
        default: return;
        case 1: _argb8888 =   0xFF  << 24 | lgfx::convert_to_rgb888((std::uint8_t ) data[0]);                          return;
        case 2: _argb8888 =   0xFF  << 24 | lgfx::convert_to_rgb888((std::uint16_t)(data[0]<< 8|data[1]));             return;
        case 3: _argb8888 =   0xFF  << 24 | lgfx::convert_to_rgb888((std::uint32_t)(data[0]<<16|data[1]<<8|data[2]));  return;
        case 4: _argb8888 = data[0] << 24 | lgfx::convert_to_rgb888((std::uint32_t)(data[1]<<16|data[2]<<8|data[3]));  return;
      }
    }
  }

  static void IRAM_ATTR load_nvs(void)
  {
    std::uint32_t handle;
    if (ESP_OK != nvs_open(LOGNAME, NVS_READONLY, &handle))
    {
      ESP_LOGI(LOGNAME, "nvs open error.  start nvs erase ...");
      esp_err_t init = nvs_flash_init();
      while ( init != ESP_OK )
      {
        taskYIELD();
        nvs_flash_erase();
        init = nvs_flash_init();
      }
      ESP_LOGI(LOGNAME, "done.");

      save_nvs(_i2c_addr);
    }
    else  
    {
      if (ESP_OK != nvs_get_u8(handle, NVS_KEY_I2CADDR, &_i2c_addr))
      {
        ESP_LOGE(LOGNAME, "nvs error: can't get address.");
      }
      nvs_close(handle);
      _i2c_addr = std::min<std::uint8_t>(I2C_MAX_ADDR, std::max<std::uint8_t>(I2C_MIN_ADDR, _i2c_addr));
    }
  }

  static void IRAM_ATTR set_power_mode(std::uint8_t mode)
  {
    switch (mode)
    {
    case 0:  cpu_clock::set_clock_limit(cpu_clock::clock_40MHz,  cpu_clock::clock_40MHz ); break;
    case 1:  cpu_clock::set_clock_limit(cpu_clock::clock_80MHz,  cpu_clock::clock_160MHz); break;
    case 2:  cpu_clock::set_clock_limit(cpu_clock::clock_160MHz, cpu_clock::clock_160MHz); break;
    }
  }

  std::size_t IRAM_ATTR getBufferFree(void)
  {
    std::uint32_t res = (_rx_buffer_getpos - _rx_buffer_setpos) & (RX_BUFFER_MAX - 1);
    return res ? res : RX_BUFFER_MAX;
  }

  static bool IRAM_ATTR command(void)
  {
    if (_rx_buffer_getpos == _rx_buffer_setpos)
    {
      return false;
    }
    const std::uint8_t* params = _rx_buffer[_rx_buffer_getpos];

  #if DEBUG == 1
    if (cmd_detect[params[0]] == 0)
    {
      cmd_detect[params[0]] = 1;
      ESP_LOGI(LOGNAME, "CMD:%02x", params[0]);
    }
  #endif

    switch (params[0])
    {
    default:
      ESP_LOGI(LOGNAME, "unknown CMD:%02x", params[0]);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_CHANGE_ADDR:
      /// Reboot only once the NVS save (through commit) is confirmed. If the save failed,
      /// keep running on the current address. _nvs_push must be cleared here, otherwise
      /// READ_BUFCOUNT stays stuck at 1.
      if (save_nvs(_i2c_addr_request))
      {
        esp_restart();
      }
      _nvs_push = false;
      break;

    case lgfx::Panel_M5UnitLCD::CMD_INVON:
      ESP_LOGI(LOGNAME, "CMD INV ON");
      _lcd.invertDisplay(true);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_INVOFF:
      ESP_LOGI(LOGNAME, "CMD INV OFF");
      _lcd.invertDisplay(false);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_SET_BYTESWAP:
      if (params[1] == 0)
      {
        _byteswap = false;
        ESP_LOGI(LOGNAME, "BYTESWAP OFF");
      }
      else
      {
        _byteswap = true;
        ESP_LOGI(LOGNAME, "BYTESWAP ON");
      }
      break;

    case lgfx::Panel_M5UnitLCD::CMD_SET_SLEEP:
      ESP_LOGI(LOGNAME, "CMD SLEEP:%d", params[1]);
      if (params[1])
      {
        _lcd.sleep();
        set_backlight(0);
      }
      else
      {
        _lcd.wakeup();
        set_backlight(_brightness);
      }
      break;

    case lgfx::Panel_M5UnitLCD::CMD_SET_POWER:
      ESP_LOGI(LOGNAME, "CMD SET POWER:%d", params[1]);
      set_power_mode(params[1]);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_BRIGHTNESS:
      _brightness = params[1];
      set_backlight(_brightness);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_ROTATE:
      {
        _canvas.setRotation(params[1]);
        // Sprite coordinate transforms change the backing-store layout. Keep
        // the legacy full-frame path for rotated modes; partial transfers are
        // used for the normal orientation where the buffer is row-contiguous.
        _partial_flush_enabled = (params[1] == 0);
        mark_full_modified();
      }
      break;

    case lgfx::Panel_M5UnitLCD::CMD_CASET:
      {
        std::uint_fast8_t xs = params[1];
        std::uint_fast8_t xe = params[2];
        if (xs > xe)
        {
          std::swap(xs, xe);
        }
        _xe = xe;
        _xptr = _xs = xs;
        _yptr = _ys;
      }
      break;

    case lgfx::Panel_M5UnitLCD::CMD_RASET:
      {
        std::uint_fast8_t ys = params[1];
        std::uint_fast8_t ye = params[2];
        if (ys > ye)
        {
          std::swap(ys, ye);
        }
        _ye = ye;
        _yptr = _ys = ys;
        _xptr = _xs;
      }
      break;

    case lgfx::Panel_M5UnitLCD::CMD_COPYRECT:
      _canvas.copyRect( params[5]
                , params[6]
                , params[3] - params[1] + 1
                , params[4] - params[2] + 1
                , params[1]
                , params[2]
                );
      mark_modified_rect(params[5], params[6],
                         params[3] - params[1] + 1,
                         params[4] - params[2] + 1);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_8:
    case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_16:
    case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_24:
    case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_32:
      update_argb8888(&params[1], params[0] & 7);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_8:
    case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_16:
    case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_24:
    case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_32:
      update_argb8888(&params[3], params[0] & 7);
      // don't break
    case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL:

      _xptr = _xs = _xe = params[1];
      _yptr = _ys = _ye = params[2];

      if ((_argb8888 >> 24) == 0xFF)
      {
        _canvas.drawPixel(_xs, _ys, _argb8888);
      }
      else
      {
        _canvas.fillRectAlpha(_xs, _ys, 1, 1, _argb8888 >> 24, _argb8888);
      }
      mark_modified_rect(_xs, _ys, 1, 1);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_8:
    case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_16:
    case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_24:
    case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_32:
      update_argb8888(&params[5], params[0] & 7);
      // don't break
    case lgfx::Panel_M5UnitLCD::CMD_FILLRECT:
      {
        std::uint_fast8_t xs = params[1];
        std::uint_fast8_t xe = params[3];
        if (xs > xe)
        {
          std::swap(xs, xe);
        }
        _xs = xs;
        _xe = xe;
        std::uint_fast8_t ys = params[2];
        std::uint_fast8_t ye = params[4];
        if (ys > ye)
        {
          std::swap(ys, ye);
        }
        _ys = ys;
        _ye = ye;
      }
      // don't break
    case lgfx::Panel_M5UnitLCD::CMD_RAM_FILL:
      _xptr = _xs;
      _yptr = _ys;

      if ((_argb8888 >> 24) == 0xFF)
      {
        _canvas.fillRect(_xs, _ys, _xe - _xs + 1, _ye - _ys + 1, _argb8888);
      }
      else
      {
        _canvas.fillRectAlpha(_xs, _ys, _xe - _xs + 1, _ye - _ys + 1, _argb8888 >> 24, _argb8888);
      }
      mark_modified_rect(_xs, _ys, _xe - _xs + 1, _ye - _ys + 1);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_8:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_16:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_24:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_32:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_A:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_8:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_16:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_24:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_32:
    case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_A:
      {
        bool rle = (params[0] & ~7) == lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE;
        std::uint8_t alpha;
        if ((params[0] == lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_A)
         || (params[0] == lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_A))
        { // アルファチャネルのみ
          alpha = params[rle + 1];
          _argb8888 = (_argb8888 & 0xFFFFFF) | alpha << 24;
        }
        else
        {
          update_argb8888(&params[rle + 1], params[0] & 7);
          alpha = _argb8888 >> 24;
        }
        if (_xs <= _xe && _ys <= _ye)
        {
          std::size_t length = rle ? params[1] : 1;
          std::uint_fast16_t xptr = _xptr;
          std::uint_fast16_t yptr = _yptr;
          do
          {
            auto len = std::min<std::uint32_t>(length, _xe + 1 - xptr);
            if (alpha)
            {
              if (alpha == 0xFF)
              {
                _canvas.fillRect(xptr, yptr, len, 1, _argb8888);
              }
              else
              {
                _canvas.fillRectAlpha(xptr, yptr, len, 1, alpha, _argb8888);
              }
            }
            xptr += len;
            if (xptr > _xe)
            {
              xptr = _xs;
              if (++yptr > _ye)
              {
                yptr = _ys;
              }
              _yptr = yptr;
            }
            length -= len;
          } while (length);
          _xptr = xptr;
        }
      }
      mark_modified_rect(_xs, _ys, _xe - _xs + 1, _ye - _ys + 1);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_UPDATE_BEGIN:
      _modified = false;
      cpu_clock::request_clock_up(cpu_clock::clock_160MHz);
      update::initCRCtable();
      _lcd.fillScreen(TFT_WHITE);
      _lcd.drawString("update", 0, 0);
      _lcd.fillRect(10, 112, _lcd.width() - 20, 17, TFT_BLACK);
      _lcd.fillCircle(                10, 120, 8, TFT_BLACK);
      _lcd.fillCircle( _lcd.width() - 10, 120, 8, TFT_BLACK);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_UPDATE_DATA:
      _firmupdate_result = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_BUSY;
      _modified = false;
      ESP_LOGI(LOGNAME, "flash:%d", _firmupdate_index);
      if (!update::writeBuffer(_firmupdate_index))
      {
        ESP_LOGE(LOGNAME, "OTA write fail");
        _firmupdate_result = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_ERROR;
      }
      else
      {
        _firmupdate_result = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_OK;
      }

      _firmupdate_state = firmupdate_state_t::wait_data;
      _firmupdate_index += update::BLOCK_SIZE;
      _lcd.fillCircle( 10 + (_lcd.width() - 20) * _firmupdate_index / _firmupdate_totalsize, 120, 4, TFT_GREEN );
      //_nvs_push = false;
      break;

    case lgfx::Panel_M5UnitLCD::CMD_UPDATE_END:
      if (update::end())
      {
        _lcd.drawString("success", 0, 144);
        ESP_LOGI(LOGNAME, "success! rebooting...");
        lgfx::delay(1000);
        esp_restart();
      }
      else
      {
        ESP_LOGE(LOGNAME, "OTA close fail");
      }
      break;
    }

    _rx_buffer_getpos = (_rx_buffer_getpos + 1) & (RX_BUFFER_MAX - 1);
    return true;
  }

  void setup(void)
  {
    set_backlight(_brightness);

    load_nvs();

#if DEBUG == 1
    lgfx::pinMode(0, lgfx::pin_mode_t::output);
    lgfx::gpio_hi(0);
#endif

    {
      auto cfg = _spi_bus.config();
      cfg.spi_host = SPI2_HOST;
      // C61's current generic GDMA path can stall during full-frame transfers.
      // Disable DMA so M5GFX uses its synchronous register conversion path.
      cfg.dma_channel = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 14000000;
      cfg.pin_mosi = board_config::PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_sclk = board_config::PIN_LCD_SCLK;
      cfg.pin_dc   = board_config::PIN_LCD_DC;
      cfg.spi_3wire = true;
      cfg.spi_mode = 3;
      _spi_bus.config(cfg);
    }
    {
      auto cfg = _panel.config();
      cfg.invert = true;

      cfg.pin_cs  = board_config::PIN_LCD_CS;
      cfg.pin_rst = board_config::PIN_LCD_RST;
      cfg.panel_width  = board_config::LCD_WIDTH;
      cfg.panel_height = board_config::LCD_HEIGHT;
      cfg.offset_x     = board_config::LCD_OFFSET_X;
      cfg.offset_y     = board_config::LCD_OFFSET_Y;
      _panel.config(cfg);
    }
    _panel.setBus(&_spi_bus);
    _lcd.setPanel(&_panel);
    const bool lcd_init_ok = _lcd.init();
    ESP_LOGI(LOGNAME, "LCD init=%d", lcd_init_ok);
    _lcd.startWrite();
    ESP_LOGI(LOGNAME, "LCD startWrite complete");
    _lcd.fillScreen(TFT_WHITE);
    ESP_LOGI(LOGNAME, "LCD fillScreen complete");
    _lcd.setTextColor(TFT_BLACK, TFT_WHITE);

    _lcd.setFont(&fonts::Font4);
    _lcd.setCursor(0,0);
    _lcd.printf("Addr : 0x%02x", _i2c_addr);
    _lcd.setCursor(0,216);
    _lcd.printf("Ver : %0d.%0d", FIRMWARE_MAJOR_VERSION, FIRMWARE_MINOR_VERSION);
    ESP_LOGI(LOGNAME, "LCD startup text complete");
    _lcd.setColorDepth(24);
    _canvas.setColorDepth (24);

    // Keep the address/version splash visible until the host sends a draw command.
    _modified = false;
    _lcd.setRotation(0);
    _lcd.setWindow(0, 0, _lcd.width()-1, _lcd.height()-1);
    _canvas.createSprite(_lcd.width(), _lcd.height());
    _canvas.setRotation(0);
    // The splash was drawn directly to the panel. Mirror it into the backing
    // store so the first partial host update starts from the same frame.
    _canvas.fillScreen(TFT_WHITE);
    _canvas.setTextColor(TFT_BLACK, TFT_WHITE);
    _canvas.setFont(&fonts::Font4);
    _canvas.setCursor(0, 0);
    _canvas.printf("Addr : 0x%02x", _i2c_addr);
    _canvas.setCursor(0, 216);
    _canvas.printf("Ver : %0d.%0d", FIRMWARE_MAJOR_VERSION, FIRMWARE_MINOR_VERSION);
    _dirty_valid = false;
    _lcd.endWrite();
    ESP_LOGI(LOGNAME, "LCD canvas complete");

    cpu_clock::init();
    cpu_clock::request_clock_down(cpu_clock::clock_80MHz);
    set_power_mode(1);
    ESP_LOGI(LOGNAME, "CPU clock setup complete");

    const bool encoder_init_ok = encoder_input::setup();
    ESP_LOGI(LOGNAME, "Encoder init=%d A=%d B=%d button=%d",
             encoder_init_ok,
             board_config::PIN_ENCODER_A,
             board_config::PIN_ENCODER_B,
             board_config::PIN_ENCODER_BUTTON);

    const bool i2c_init_ok = i2c_slave::init(I2C_PORT,
                                             board_config::PIN_I2C_SDA,
                                             board_config::PIN_I2C_SCL,
                                             _i2c_addr,
                                             xTaskGetCurrentTaskHandle(),
                                             ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3);
    ESP_LOGI(LOGNAME, "I2C slave init=%d addr=0x%02x SDA=%d(level=%d) SCL=%d(level=%d)",
             i2c_init_ok,
             _i2c_addr,
             board_config::PIN_I2C_SDA,
             gpio_get_level(board_config::PIN_I2C_SDA),
             board_config::PIN_I2C_SCL,
             gpio_get_level(board_config::PIN_I2C_SCL));
    if (!i2c_init_ok)
    {
      ESP_LOGE(LOGNAME, "I2C slave initialization failed");
    }
  /*
    auto ms = lgfx::millis();
    lgfx::pinMode(0, lgfx::pin_mode_t::input_pullup);
    lgfx::pinMode(1, lgfx::pin_mode_t::input_pullup);
    lgfx::pinMode(3, lgfx::pin_mode_t::input_pullup);
    while (command_processor::rxbuffer_ringpos == 0 && (lgfx::millis()-ms) < 5000)
    {
      if (!lgfx::gpio_in(0) || !lgfx::gpio_in(1) || !lgfx::gpio_in(3))
      {
        if (ESP_OK == nvs_open(LOGNAME, NVS_READWRITE, &handle)) {
          nvs_set_u8(handle, NVS_KEY_I2CADDR, i2c_default_addr);
          nvs_close(handle);
        }
        while (!lgfx::gpio_in(0) || !lgfx::gpio_in(1) || !lgfx::gpio_in(3)) lgfx::delay(10);
        esp_restart();
      }
      taskYIELD();
    }
    lgfx::pinMode(0, lgfx::pin_mode_t::input);
    lgfx::pinMode(1, lgfx::pin_mode_t::input);
    lgfx::pinMode(3, lgfx::pin_mode_t::input);
  //*/
  }

  /// メインループ処理 蓄積したコマンドの処理およびLCDへの出力処理
  void IRAM_ATTR loop(void)
  {
    ulTaskNotifyTake( pdTRUE, 0 );
    static std::uint32_t last_diagnostic_interrupts = 0;
    i2c_slave::diagnostics_t diagnostics;
    i2c_slave::get_diagnostics(&diagnostics);
    if (diagnostics.interrupts != last_diagnostic_interrupts)
    {
      last_diagnostic_interrupts = diagnostics.interrupts;
      ESP_LOGI(LOGNAME,
               "I2C diag irq=%u status=0x%08x rx=%u txevt=%u txbytes=%u stretch(addr=%u tx=%u other=%u) cmd=0x%02x",
               diagnostics.interrupts,
               diagnostics.last_status,
               diagnostics.rx_bytes,
               diagnostics.tx_fifo_events,
               diagnostics.tx_bytes,
               diagnostics.stretch_address,
               diagnostics.stretch_tx_empty,
               diagnostics.stretch_other,
               static_cast<unsigned>(_last_command));
    }
    bool processed_command = false;
    while (command())
    {
      processed_command = true;
    }
    if (!processed_command && !_modified && _firmupdate_state == firmupdate_state_t::nothing)
    {
      cpu_clock::request_clock_down(cpu_clock::clock_40MHz);
      // Address persistence and OTA deliberately suppress ISR task notifications
      // while flash is involved. Poll briefly so those queued commands cannot
      // leave the single-core C61 main task asleep indefinitely.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
      cpu_clock::request_clock_up(cpu_clock::clock_160MHz);
    }
    // The framebuffer is committed after the commands currently available in
    // the RX queue have been applied. Only the accumulated dirty rectangle is
    // transferred, so a local update never sends or clears the whole screen.
    if (_modified && !_spi_bus.busy())
    {
      _modified = false;
      // M5GFX's generic C61 GDMA path does not complete reliably. Send the
      // panel's native 24-bit stream through the synchronous register path.
      flush_canvas_to_lcd();
    }
  }

  /// I2C STOP時などのデータの区切りの処理
  void IRAM_ATTR closeData(void)
  {
    _param_index = 0;
    _param_need_count = 1;
    _param_resetindex = 0;
  }

  /// I2CペリフェラルISRから1Byteずつデータを受取る処理
  bool IRAM_ATTR addData(std::uint8_t value)
  {
    _params[_param_index] = value;

    if (++_param_index == 1)
    {
      _last_command = value;
      _param_resetindex = 0;
    
      switch (value)
      {
      default:
        // 未定義のコマンドを受取った場合は通信が切れるまで残りの受信データを全て無視する。
        _params[0] = lgfx::Panel_M5UnitLCD::CMD_NOP;
        _param_need_count = PARAM_MAXLEN;
        _param_resetindex = 1;
        break;

      case lgfx::Panel_M5UnitLCD::CMD_READ_ID:
      case lgfx::Panel_M5UnitLCD::CMD_READ_BUFCOUNT:
      case lgfx::Panel_M5UnitLCD::CMD_INVOFF:
      case lgfx::Panel_M5UnitLCD::CMD_INVON:
      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_8:
      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_16:
      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_24:
      case unit_lcd_plus_protocol::CMD_READ_ENCODER_STATE:
      case unit_lcd_plus_protocol::CMD_PEEK_ENCODER_EVENT:
      case unit_lcd_plus_protocol::CMD_READ_BUTTON_CONFIG:
        _param_need_count = 1;
        break;

      case lgfx::Panel_M5UnitLCD::CMD_BRIGHTNESS:
      case lgfx::Panel_M5UnitLCD::CMD_ROTATE:
      case lgfx::Panel_M5UnitLCD::CMD_SET_POWER:
      case lgfx::Panel_M5UnitLCD::CMD_SET_SLEEP:
      case lgfx::Panel_M5UnitLCD::CMD_SET_BYTESWAP:
        _param_need_count = 2;
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_CASET:
      case lgfx::Panel_M5UnitLCD::CMD_RASET:
        _param_need_count = 3;
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_RESET:
      case lgfx::Panel_M5UnitLCD::CMD_CHANGE_ADDR:
      case lgfx::Panel_M5UnitLCD::CMD_UPDATE_END:
        _param_need_count = 4;
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_8:
      case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_16:
      case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_24:
      case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_32:
        _param_need_count = 1 + (value & 7);
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_COPYRECT:
        _param_need_count = 7;
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_8:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_16:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_24:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_32:
        _param_need_count = 3 + (value & 7);
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_8:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_16:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_24:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_32:
        _param_need_count = 5 + (value & 7);
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_8:
      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_16:
      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_24:
      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_32:
      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RAW_A:
        _param_need_count = 2 + ((value - 1) & 3);
        _param_resetindex = 1;
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_8:
      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_16:
      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_24:
      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_32:
      case lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE_A:
        _param_need_count = 3 + ((value - 1) & 3);
        _param_resetindex = 1;
        _rle_abs = 0;
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_UPDATE_BEGIN:
      case lgfx::Panel_M5UnitLCD::CMD_UPDATE_DATA:
        _param_need_count = 8;
        return false;

      case unit_lcd_plus_protocol::CMD_ACK_ENCODER_EVENT:
      case unit_lcd_plus_protocol::CMD_SET_BUTTON_CONFIG:
        _param_need_count = 3;
        return false;

      case unit_lcd_plus_protocol::CMD_SET_ENCODER_POSITION:
        _param_need_count = 5;
        return false;
      }
    }
    else
    if (_param_index == 3 && (_params[0] & ~7) == lgfx::Panel_M5UnitLCD::CMD_WRITE_RLE)
    { // RLEエンコードされたピクセル情報の展開
      if (_rle_abs)
      {
        if (0 == --_rle_abs)
        {
          _param_resetindex = 1;
        }
      }
      else
      if (_params[1] == 0) // enter abs_mode
      {
        _params[1] = 1;    // rle len = 1
        _rle_abs = _params[2];
        _param_index--;
        _param_resetindex = 2;
      }
    }



    if (_param_index >= _param_need_count)
    {
      i2c_slave::clear_txdata();
      switch (_params[0])
      {
      default:
        break;

      case lgfx::Panel_M5UnitLCD::CMD_NOP:
        _param_index = _param_resetindex;
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_RESET:
        if ((_params[1] == 0x77)
         && (_params[2] == 0x89)
         && (_params[0] == _params[3])
        )
        {
          esp_restart();
          break;
        }

      /// ファームウェアアップデートの準備コマンド
      case lgfx::Panel_M5UnitLCD::CMD_UPDATE_BEGIN:
        if ((_params[1] == 0x77)
         && (_params[2] == 0x89)
         && (_params[0] == _params[3])
        )
        {
          _firmupdate_state = firmupdate_state_t::wait_data;
          _firmupdate_index = 0;
          _firmupdate_result = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_ERROR; /// 途中中断した時のためリード応答にはエラーステートを設定しておく
          _firmupdate_totalsize = _params[4] << 24 | _params[5] << 16 | _params[6] << 8 | _params[7];
          update::begin(_firmupdate_totalsize);
        }
        else
        {
          closeData();
          return false;
        }
        break;

      /// ファームウェアアップデートのデータ受信コマンド
      case lgfx::Panel_M5UnitLCD::CMD_UPDATE_DATA:
        if (_firmupdate_state == firmupdate_state_t::progress)
        {
          /// 受信したデータをupdateに蓄積
          if (update::addData(_params[1]))
          {
            _param_index = _param_resetindex;
            return false;
          }

          if (update::checkCRC32())
          {
            _firmupdate_result = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_BUSY;
          }
          else
          {
            _firmupdate_result = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_BROKEN;
          }
          prepareTxData();
          _firmupdate_state = sector_write;
          closeData();
        }
        else
        if ((_params[1] == 0x77)
         && (_params[2] == 0x89)
         && (_params[0] == _params[3])
        )
        {
          _nvs_push = true; // ファームウェア書き込み時はISRにイベントを起こさせない
          update::setBlockCRC32( _params[4] << 24 | _params[5] << 16 | _params[6] << 8 | _params[7] );
          _param_need_count = 2;
          _param_resetindex = 1;
          _param_index = _param_resetindex;
          _firmupdate_state = firmupdate_state_t::progress;
          _firmupdate_result = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_ERROR; /// 途中中断した時のためリード応答にはエラーステートを設定しておく
          return false;
        }
        else
        {
          closeData();
          return false;
        }
        break;

      /// Firmware update completion command
      case lgfx::Panel_M5UnitLCD::CMD_UPDATE_END:
        /// Discard without queueing when the magic mismatches or UPDATE_BEGIN has not run
        /// (invalid commands have no side effect). Previously update::end() ran unchecked and a
        /// unit without UPDATE_BEGIN rebooted on an esp_partition_write assert.
        if ((_params[1] != 0x77)
         || (_params[2] != 0x89)
         || (_params[0] != _params[3])
         || (_firmupdate_state == firmupdate_state_t::nothing)
        )
        {
          closeData();
          return false;
        }
        break;

      case lgfx::Panel_M5UnitLCD::CMD_CHANGE_ADDR:
        /// A request that fails the validity rules (section 13.1: range, complement, matching
        /// head and tail, different from the current address) is discarded without queueing
        /// (invalid commands have no side effect). Previously save_nvs + esp_restart ran even
        /// for invalid requests, rebooting without changing the address.
        if (_params[0] != _params[3]
         || _params[1] != (0xFF & ~_params[2])
         || _params[1] < I2C_MIN_ADDR
         || _params[1] > I2C_MAX_ADDR
         || _params[1] == _i2c_addr
        )
        {
          closeData();
          return false;
        }
        _i2c_addr_request = _params[1];
        _nvs_push = true;
        break;

      case lgfx::Panel_M5UnitLCD::CMD_CASET:
        _read_xs = std::max<std::uint_fast16_t>(_params[1], 0);
        _read_xe = std::min<std::uint_fast16_t>(_params[2], _canvas.width()-1);
        break;

      case lgfx::Panel_M5UnitLCD::CMD_RASET:
        _read_ys = std::max<std::uint_fast16_t>(_params[1], 0);
        _read_ye = std::min<std::uint_fast16_t>(_params[2], _canvas.height()-1);
        break;

      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_8:
      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_16:
      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_24:
        _read_xptr = _read_xs;
        _read_yptr = _read_ys;
        prepareTxData();
        closeData();
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_READ_ID:
      case lgfx::Panel_M5UnitLCD::CMD_READ_BUFCOUNT:
        prepareTxData();
        closeData();
        return false;

      case unit_lcd_plus_protocol::CMD_READ_ENCODER_STATE:
        encoder_input::read_state(_encoder_state_response);
        prepareTxData();
        closeData();
        return false;

      case unit_lcd_plus_protocol::CMD_PEEK_ENCODER_EVENT:
        encoder_input::peek_event(_encoder_event_response);
        prepareTxData();
        closeData();
        return false;

      case unit_lcd_plus_protocol::CMD_ACK_ENCODER_EVENT:
        encoder_input::acknowledge(unit_lcd_plus_protocol::read_u16_be(&_params[1]));
        closeData();
        return false;

      case unit_lcd_plus_protocol::CMD_SET_ENCODER_POSITION:
        encoder_input::set_position(unit_lcd_plus_protocol::read_i32_be(&_params[1]));
        closeData();
        return false;

      case unit_lcd_plus_protocol::CMD_READ_BUTTON_CONFIG:
        encoder_input::read_button_config(_button_config_response);
        prepareTxData();
        closeData();
        return false;

      case unit_lcd_plus_protocol::CMD_SET_BUTTON_CONFIG:
        encoder_input::set_button_config(unit_lcd_plus_protocol::read_u16_be(&_params[1]));
        closeData();
        return false;
      }

      auto new_setpos = (_rx_buffer_setpos + 1) & (RX_BUFFER_MAX - 1);
      _rx_buffer_setpos = new_setpos;
      _param_index = _param_resetindex;
      for (std::size_t i = 0; i < _param_resetindex; ++i)
      {
        _rx_buffer[new_setpos][i] = _params[i];
      }
      _params = _rx_buffer[new_setpos];


      // NVS領域やファームウェアへの書き込みはタスク通知を使うとクラッシュするのでfalseを返す
      return _nvs_push ? false : true;
    }
    return false;
  }

  void IRAM_ATTR prepareTxData(void)
  {
    /// Kept in DRAM because the ISR reads it (same reason as read_id_data).
    static const DRAM_ATTR std::uint8_t dummy[] = { 0xff, 0xff };
    switch (_last_command)
    {
    default:
      i2c_slave::add_txdata(dummy, 1);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_READ_ID:
      i2c_slave::add_txdata(read_id_data, sizeof(read_id_data));
      break;

    case lgfx::Panel_M5UnitLCD::CMD_UPDATE_DATA:
      i2c_slave::add_txdata(_firmupdate_result);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_READ_BUFCOUNT:
      {
        std::uint32_t res = 255;
        if (_nvs_push)
        {
          res = 1;
        }
        else
        {
          std::int32_t sp = _rx_buffer_setpos;
          std::int32_t gp = _rx_buffer_getpos;
          if (sp != gp)
          {
            //std::int32_t buf_free = ((int)(getBufferFree() + 512) - rxbuffer_max) >> 1;
            std::int32_t buf_free = gp - sp;
            if (buf_free > 0) buf_free -= RX_BUFFER_MAX;
            // 
            buf_free = (buf_free + 512) >> 1;
            res = std::max<std::int32_t>(1, std::min<std::int32_t>(254, buf_free));
          }
        }
        i2c_slave::add_txdata((std::uint8_t*)&res, 1);
      }
      break;

    case unit_lcd_plus_protocol::CMD_READ_ENCODER_STATE:
      i2c_slave::add_txdata(_encoder_state_response, sizeof(_encoder_state_response));
      break;

    case unit_lcd_plus_protocol::CMD_PEEK_ENCODER_EVENT:
      i2c_slave::add_txdata(_encoder_event_response, sizeof(_encoder_event_response));
      break;

    case unit_lcd_plus_protocol::CMD_READ_BUTTON_CONFIG:
      i2c_slave::add_txdata(_button_config_response, sizeof(_button_config_response));
      break;

    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_8:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_16:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_24:
      for (std::size_t i = 0; i < 8; ++i)
      {
        std::uint32_t res = _canvas.readPixelValue(_read_xptr, _read_yptr);
        std::size_t bytes = _last_command & 3;
        switch (bytes)
        {
          case 2: res = lgfx::color_convert<lgfx::swap565_t, lgfx::bgr888_t>(res); break;
          case 1: res = lgfx::color_convert<lgfx::rgb332_t, lgfx::bgr888_t>(res); break;
          default: break;
        }
        if (_byteswap)
        {
          switch (bytes)
          {
            case 3: res = lgfx::getSwap24(res); break;
            case 2: res = lgfx::getSwap16(res); break;
            default: break;
          }
        }
        i2c_slave::add_txdata((std::uint8_t*)&res, bytes);
        if (++_read_xptr > _read_xe)
        {
          _read_xptr = _read_xs;
          if (++_read_yptr > _read_ye)
          {
            _read_yptr = _read_ys;
          }
        }
      }
      break;
    }
  }
}
