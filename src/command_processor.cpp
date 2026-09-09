//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/ledc.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <soc/soc_caps.h>
#include <cstdlib>
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
#if defined(CONFIG_IDF_TARGET_ESP32C61)
#include "local_ui.hpp"
#endif

namespace command_processor
{
  static constexpr char NVS_KEY_I2CADDR[] = "ADDR";
  // Local UI palette index (unit_lcd_ui::variant_t). Utility is the shipping
  // default; the key only exists once the user changes the theme on device.
  static constexpr char NVS_KEY_UI_THEME[] = "UI_THEME";
  static constexpr std::uint8_t UI_THEME_DEFAULT = 1;
  /// Kept in DRAM because the IRAM-resident I2C ISR reads it. Left in flash rodata, a READ_ID
  /// arriving during an OTA flash write (cache disabled) would panic.
  static const DRAM_ATTR std::uint8_t read_id_data[] = {
    0x77, 0x89, static_cast<std::uint8_t>(board_config::MODEL_ID), READ_ID_FIRMWARE_VERSION
  };

  static constexpr i2c_port_t I2C_PORT = (i2c_port_t)I2C_NUM_0;
  static constexpr std::uint8_t I2C_DEFAULT_ADDR = 0x3E;
  static constexpr std::uint8_t I2C_MIN_ADDR = 0x08;
  static constexpr std::uint8_t I2C_MAX_ADDR = 0x77;
  static constexpr std::size_t RX_BUFFER_MAX = board_config::RX_QUEUE_CAPACITY;
  static constexpr std::size_t ADVERTISED_FLOW_WINDOW = board_config::ADVERTISED_FLOW_WINDOW;
  static constexpr std::size_t PARAM_MAXLEN = 12;
  static_assert((RX_BUFFER_MAX & (RX_BUFFER_MAX - 1)) == 0,
                "RX queue capacity must remain a power of two for ring masks");
  static_assert(ADVERTISED_FLOW_WINDOW <= RX_BUFFER_MAX,
                "Advertised flow window must fit in the physical RX queue");
#if defined(CONFIG_IDF_TARGET_ESP32C61)
  // Reserve the FIFO worst case (32 one-byte commands) plus the current parser slot at
  // the tail of the queue so a read address match under backpressure can parse the whole hardware RX FIFO.
  static constexpr std::size_t RX_DRAIN_PARSER_SLOTS = 1;
  static constexpr std::size_t RX_BACKPRESSURE_RESERVE = SOC_I2C_FIFO_LEN + 2;
  static_assert(SOC_I2C_FIFO_LEN + RX_DRAIN_PARSER_SLOTS
                  == RX_BACKPRESSURE_RESERVE - 1,
                "Backpressure reserve must admit a worst-case FIFO drain");
  static_assert(RX_BUFFER_MAX > RX_BACKPRESSURE_RESERVE,
                "RX queue must reserve one hardware FIFO for read freshness");
  static_assert(ADVERTISED_FLOW_WINDOW + RX_BACKPRESSURE_RESERVE
                  >= RX_BUFFER_MAX + 4,
                "Backpressure must begin before READ_BUFCOUNT clamps to one");
#endif


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
  std::uint8_t _ui_theme = UI_THEME_DEFAULT;
  /// New address requested by CHANGE_ADDR. _i2c_addr itself is not changed until the NVS
  /// save result is known, so operation continues on the current address if the save fails.
  std::uint8_t _i2c_addr_request = 0;
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _spi_bus;
  LGFX_Device _lcd;
  LGFX_Sprite _canvas;
  bool _byteswap = false;

  bool _modified = true;
  // Guard so the ISR never reads the unallocated sprite when READ_RAW arrives before the canvas exists.
  // The window exists because the I2C slave is armed before the LCD is initialized.
  volatile bool _canvas_ready = false;
  bool _dirty_valid = false;
  std::uint_fast16_t _dirty_xs = 0;
  std::uint_fast16_t _dirty_ys = 0;
  std::uint_fast16_t _dirty_xe = 0;
  std::uint_fast16_t _dirty_ye = 0;
  bool _partial_flush_enabled = true;

  // Display ownership starts with the local utility UI.  It is latched to the
  // host on the first pixel-writing command and remains there until reset.  A
  // firmware update temporarily supersedes both producers; the update path
  // always reboots on completion, so no reverse transition is needed.
  volatile std::uint8_t _display_owner =
    static_cast<std::uint8_t>(display_owner_t::local_ui);

  bool _nvs_push = false;
  volatile std::uint8_t _local_address_status =
    static_cast<std::uint8_t>(local_address_status_t::idle);
  std::uint8_t _local_address_pending = I2C_DEFAULT_ADDR;

  /// Total commands dropped because the RX queue was full (incremented in the ISR, shown in diagnostics)
  volatile std::uint32_t _rx_dropped_count = 0;
#if defined(CONFIG_IDF_TARGET_ESP32C61)
  // Normally the transport stops RX at the high-water mark that reserves the whole hardware
  // FIFO and the parser slot. Even if the queue does fill up, a completed command is never dropped.
  volatile bool _rx_pending_enqueue = false;
#endif

  static inline void mark_modified(void)
  {
    _modified = true;
  }

  static inline bool is_pixel_write_command(std::uint8_t command_value)
  {
    switch (command_value)
    {
      // These session/display preparation commands are part of a host display
      // transaction too.  They are classified here (rather than in the I2C
      // ISR) so ownership changes only after a complete, valid command has
      // been queued and is about to execute.
      case lgfx::Panel_M5UnitLCD::CMD_INVOFF:
      case lgfx::Panel_M5UnitLCD::CMD_INVON:
      case lgfx::Panel_M5UnitLCD::CMD_BRIGHTNESS:
      case lgfx::Panel_M5UnitLCD::CMD_CASET:
      case lgfx::Panel_M5UnitLCD::CMD_RASET:
      case lgfx::Panel_M5UnitLCD::CMD_ROTATE:
      case lgfx::Panel_M5UnitLCD::CMD_SET_POWER:
      case lgfx::Panel_M5UnitLCD::CMD_SET_SLEEP:
      case lgfx::Panel_M5UnitLCD::CMD_SET_BYTESWAP:
      case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_8:
      case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_16:
      case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_24:
      case lgfx::Panel_M5UnitLCD::CMD_SET_COLOR_32:
      case lgfx::Panel_M5UnitLCD::CMD_COPYRECT:
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
      case lgfx::Panel_M5UnitLCD::CMD_RAM_FILL:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_8:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_16:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_24:
      case lgfx::Panel_M5UnitLCD::CMD_DRAWPIXEL_32:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_8:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_16:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_24:
      case lgfx::Panel_M5UnitLCD::CMD_FILLRECT_32:
        return true;
      default:
        return false;
    }
  }

  static inline void claim_host_display(std::uint8_t command_value)
  {
    if (is_pixel_write_command(command_value)
     && _display_owner == static_cast<std::uint8_t>(display_owner_t::local_ui))
    {
      _display_owner = static_cast<std::uint8_t>(display_owner_t::host_owned);
    }
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
  static constexpr TickType_t FIRMUPDATE_INACTIVITY_TIMEOUT_TICKS = pdMS_TO_TICKS(5000);
  volatile TickType_t _firmupdate_last_activity_tick = 0;
  std::size_t _firmupdate_index = 0;
  std::size_t _firmupdate_totalsize = 0;
  std::size_t _firmupdate_result = 0;
  std::size_t _last_command = 0;
  static constexpr std::size_t TX_RESPONSE_SNAPSHOT_CAPACITY = 10;
  static_assert(sizeof(read_id_data) <= TX_RESPONSE_SNAPSHOT_CAPACITY,
                "READ_ID response must fit in the TX snapshot");
#if UNIT_LCD_HAS_ENCODER
  static_assert(unit_lcd_plus_protocol::ENCODER_STATE_SIZE <= TX_RESPONSE_SNAPSHOT_CAPACITY,
                "encoder state response must fit in the TX snapshot");
  static_assert(unit_lcd_plus_protocol::ENCODER_EVENT_SIZE <= TX_RESPONSE_SNAPSHOT_CAPACITY,
                "encoder event response must fit in the TX snapshot");
  static_assert(unit_lcd_plus_protocol::BUTTON_CONFIG_SIZE <= TX_RESPONSE_SNAPSHOT_CAPACITY,
                "button config response must fit in the TX snapshot");
#endif
  std::uint8_t _tx_response_snapshot[TX_RESPONSE_SNAPSHOT_CAPACITY] = { 0xff };
  std::size_t _tx_response_length = 1;
  std::size_t _tx_response_cursor = 0;
  std::uint8_t _tx_response_command = lgfx::Panel_M5UnitLCD::CMD_NOP;
#if UNIT_LCD_HAS_ENCODER
  std::uint8_t _encoder_state_response[unit_lcd_plus_protocol::ENCODER_STATE_SIZE] = {};
  std::uint8_t _encoder_event_response[unit_lcd_plus_protocol::ENCODER_EVENT_SIZE] = {};
  std::uint8_t _button_config_response[unit_lcd_plus_protocol::BUTTON_CONFIG_SIZE] = {};
#endif

  std::uint_fast8_t _brightness = 128;
  bool _backlight_pwm_attached = false;

  static void set_backlight(std::uint8_t brightness)
  {
    constexpr ledc_mode_t speed_mode = LEDC_LOW_SPEED_MODE;
    constexpr ledc_timer_t timer = LEDC_TIMER_0;
    constexpr ledc_channel_t channel = LEDC_CHANNEL_0;

    const std::uint32_t duty = board_config::LCD_BACKLIGHT_INVERTED
                             ? 255u - brightness
                             : brightness;

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
      channel_config.duty = duty;
      channel_config.hpoint = 0;

      _backlight_pwm_attached = ledc_timer_config(&timer_config) == ESP_OK
                             && ledc_channel_config(&channel_config) == ESP_OK;
    }

    if (_backlight_pwm_attached
     && ledc_set_duty(speed_mode, channel, duty) == ESP_OK
     && ledc_update_duty(speed_mode, channel) == ESP_OK)
    {
      return;
    }

    gpio_set_direction(board_config::PIN_LCD_BL, GPIO_MODE_OUTPUT);
    const bool enabled_level = board_config::LCD_BACKLIGHT_INVERTED ? false : true;
    gpio_set_level(board_config::PIN_LCD_BL, brightness ? enabled_level : !enabled_level);
  }

  static void flush_canvas_to_lcd(void)
  {
    if constexpr (!board_config::LCD_ASYNC_DMA)
    {
      _lcd.startWrite();
    }
    _lcd.setWindow(0, 0, board_config::LCD_WIDTH - 1,
                   board_config::LCD_HEIGHT - 1);
    _spi_bus.writeBytes(static_cast<const std::uint8_t*>(_canvas.getBuffer()),
                        _canvas.bufferLength(), true, true);
    if constexpr (!board_config::LCD_ASYNC_DMA)
    {
      // The original ESP32 M5GFX path releases its DMA workaround here.
      _lcd.endWrite();
    }
    _dirty_valid = false;
  }

#if defined(CONFIG_IDF_TARGET_ESP32)
  static void setup_i2c_on_core0(void* main_handle)
  {
    const bool i2c_init_ok = i2c_slave::init(I2C_PORT,
                                             board_config::PIN_I2C_SDA,
                                             board_config::PIN_I2C_SCL,
                                             _i2c_addr,
                                             main_handle,
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
    vTaskDelete(nullptr);
  }
#endif

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

  /// Saves one key to NVS. Returns true only if open / set / commit all succeed.
  static bool IRAM_ATTR save_nvs_u8(const char* key, std::uint8_t value)
  {
    i2c_slave::stop_isr();
// #if DEBUG == 1
// lgfx::gpio_hi(0);
// #endif

    bool result = false;
    {
#if UNIT_LCD_HAS_ENCODER
      // An NVS commit also disables the cache. Drain the running ISR with the shared guard and
      // re-anchor to the GPIO state afterwards so transitions before and after are not joined wrongly.
      encoder_input::flash_guard_t encoder_flash_guard;
#endif
      std::uint32_t handle = 0;
      result = (ESP_OK == nvs_open(LOGNAME, NVS_READWRITE, &handle));
      if (result)
      {
        result = (ESP_OK == nvs_set_u8(handle, key, value))
              && (ESP_OK == nvs_commit(handle));
        nvs_close(handle);
      }
    }
    i2c_slave::start_isr();

// #if DEBUG == 1
// lgfx::gpio_lo(0);
// #endif
    return result;
  }

  /// Saves the address to NVS. Returns true only if open / set / commit all succeed.
  static bool IRAM_ATTR save_nvs(std::uint8_t addr)
  {
    return save_nvs_u8(NVS_KEY_I2CADDR, addr);
  }

  bool request_local_address_change(std::uint8_t address)
  {
    const auto status = static_cast<local_address_status_t>(_local_address_status);
    if (status == local_address_status_t::pending
     || _display_owner != static_cast<std::uint8_t>(display_owner_t::local_ui)
     || _firmupdate_state != firmupdate_state_t::nothing
     || _nvs_push
     || address < I2C_MIN_ADDR
     || address > I2C_MAX_ADDR
     || address == _i2c_addr)
    {
      return false;
    }

    _local_address_pending = address;
    _local_address_status = static_cast<std::uint8_t>(local_address_status_t::pending);
    // Make READ_BUFCOUNT advertise a blocked queue while the main task commits
    // NVS, matching the existing host CHANGE_ADDR backpressure contract.
    _nvs_push = true;
    return true;
  }

  local_address_status_t local_address_change_status(void)
  {
    return static_cast<local_address_status_t>(_local_address_status);
  }

  void acknowledge_local_address_change(void)
  {
    if (local_address_change_status() != local_address_status_t::pending)
    {
      _local_address_status = static_cast<std::uint8_t>(local_address_status_t::idle);
    }
  }

  std::uint8_t current_i2c_address(void)
  {
    return _i2c_addr;
  }

  std::uint8_t local_ui_theme(void)
  {
    return _ui_theme;
  }

  bool save_local_ui_theme(std::uint8_t theme)
  {
    // Called only from the local UI (loop task) after a live theme switch;
    // unlike the address path it needs no reboot, so a failed commit just
    // keeps this boot's palette non-persistent.
    if (theme > 2) return false;
    if (!save_nvs_u8(NVS_KEY_UI_THEME, theme)) return false;
    _ui_theme = theme;
    return true;
  }

  bool present_local_ui_tile(std::int32_t x,
                             std::int32_t y,
                             std::int32_t width,
                             std::int32_t height,
                             const std::uint8_t* pixels)
  {
    if (pixels == nullptr || width <= 0 || height <= 0
     || !_canvas_ready
     || display_owner() != display_owner_t::local_ui)
    {
      return false;
    }

    const auto max_x = static_cast<std::int32_t>(board_config::LCD_WIDTH);
    const auto max_y = static_cast<std::int32_t>(board_config::LCD_HEIGHT);
    if (x < 0 || y < 0 || x + width > max_x || y + height > max_y)
    {
      return false;
    }

    // The canvas is also the source of the host READ_RAW path. M5GFX's async
    // SPI transfer retains that source until DMA completes, so wait before the
    // LVGL tile is converted into the canvas.
    //
    // LVGL's buffer is native little-endian RGB565. Passing it as uint16_t*
    // would select M5GFX's swap565_t (big-endian byte array) interpretation
    // and swap the bytes of every pixel, visible as a magenta/purple cast;
    // the rgb565_t overload keeps the channel layout intact.
    _lcd.waitDMA();
    _canvas.pushImage(x, y, width, height,
                      reinterpret_cast<const lgfx::rgb565_t*>(pixels));
    mark_modified_rect(x, y, width, height);
    return true;
  }

  void restart_after_local_address_change(void)
  {
    if (local_address_change_status() != local_address_status_t::saved) return;
    i2c_slave::shutdown();
    esp_restart();
  }

  static void service_local_address_change(void)
  {
    if (local_address_change_status() != local_address_status_t::pending) return;

    // A host command may have been drained in the same loop turn after the UI
    // queued its request. Host/IAP ownership wins that hand-off; do not commit
    // an address change after the local producer has lost ownership.
    if (_display_owner != static_cast<std::uint8_t>(display_owner_t::local_ui)
     || _firmupdate_state != firmupdate_state_t::nothing)
    {
      _nvs_push = false;
      _local_address_status = static_cast<std::uint8_t>(local_address_status_t::failed);
      return;
    }

    const auto requested = _local_address_pending;
    const bool saved = save_nvs(requested);
    _nvs_push = false;
    if (saved)
    {
      _i2c_addr = requested;
      _local_address_status = static_cast<std::uint8_t>(local_address_status_t::saved);
    }
    else
    {
      _local_address_status = static_cast<std::uint8_t>(local_address_status_t::failed);
    }
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
      // A missing theme key just keeps the shipping default; only clamp
      // out-of-range values from older or corrupted namespaces.
      if (ESP_OK == nvs_get_u8(handle, NVS_KEY_UI_THEME, &_ui_theme) && _ui_theme > 2)
      {
        _ui_theme = UI_THEME_DEFAULT;
      }
      nvs_close(handle);
      _i2c_addr = std::min<std::uint8_t>(I2C_MAX_ADDR, std::max<std::uint8_t>(I2C_MIN_ADDR, _i2c_addr));
    }
  }

  static void IRAM_ATTR set_power_mode(std::uint8_t mode)
  {
    constexpr std::size_t mode_count = sizeof(board_config::CPU_POWER_MIN_MHZ)
                                     / sizeof(board_config::CPU_POWER_MIN_MHZ[0]);
    if (mode < mode_count)
    {
      cpu_clock::set_clock_limit(
        static_cast<cpu_clock::cpu_clock_t>(board_config::CPU_POWER_MIN_MHZ[mode]),
        static_cast<cpu_clock::cpu_clock_t>(board_config::CPU_POWER_MAX_MHZ[mode]));
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

    // The queue is drained by the main task, so this transition is ordered with
    // the LVGL frame producer. Read/configuration commands deliberately do not
    // claim the panel: hosts may probe the Unit while the local UI is visible.
    claim_host_display(params[0]);

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
        i2c_slave::shutdown();
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
      _display_owner = static_cast<std::uint8_t>(display_owner_t::firmware_update);
      _modified = false;
      cpu_clock::request_clock_up(cpu_clock::clock_160MHz);
      _lcd.fillScreen(TFT_WHITE);
      _lcd.drawString("update", 0, 0);
      _lcd.fillRect(10, 112, _lcd.width() - 20, 17, TFT_BLACK);
      _lcd.fillCircle(                10, 120, 8, TFT_BLACK);
      _lcd.fillCircle( _lcd.width() - 10, 120, 8, TFT_BLACK);
      break;

    case lgfx::Panel_M5UnitLCD::CMD_UPDATE_DATA:
      // A block with a CRC mismatch is discarded in the ISR and never enters sector_write.
      // Even if that command is still in the queue, do not overwrite BROKEN with BUSY/ERROR.
      if (_firmupdate_state != firmupdate_state_t::sector_write)
      {
        break;
      }
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
        i2c_slave::shutdown();
        esp_restart();
      }
      else
      {
        ESP_LOGE(LOGNAME, "OTA close fail");
      }
      break;
    }

    _rx_buffer_getpos = (_rx_buffer_getpos + 1) & (RX_BUFFER_MAX - 1);
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    // The C61 ISR stops the RX watermark at the queue high-water mark. Notify the transport
    // right after freeing one entry so FIFO processing and SCL resume if they were held.
    i2c_slave::resume_rx();
#endif
    return true;
  }

  /// Orientation of the splash. The panel is a 135 x 240 portrait, so the model name only
  /// fits when drawn in landscape (240 x 135). The host coordinate system stays at rotation 0.
  static constexpr std::uint8_t SPLASH_ROTATION = 1;

  /// Splash screen. Takes a base-class reference so the same picture is drawn on both the panel and the canvas.
  static void drawSplash(lgfx::LovyanGFX& gfx)
  {
    gfx.setFont(&fonts::Font4);
    gfx.setCursor(0, 0);
    gfx.printf("Addr : 0x%02x", _i2c_addr);
    gfx.setCursor(0, 45);
    gfx.printf("%s", board_config::MODEL_NAME);
    gfx.setCursor(0, 90);
    gfx.printf("FW : %u%s", static_cast<unsigned>(FIRMWARE_VERSION),
               FIRMWARE_DEBUG ? " debug" : "");
  }

  void setup(void)
  {
    set_backlight(_brightness);

    load_nvs();

    // UPDATE_BEGIN runs in the main task and the following UPDATE_DATA in the ISR, so building
    // the table between commands would race. Build it once at boot before the I2C interrupt is installed.
    update::initCRCtable();

    // Arm the I2C slave before initializing the LCD. The released driver gives up its probe
    // 300 ms + 8 x 32 ms after CMD_RESET, so the later the slave is armed the closer existing
    // hosts get to that timeout. Draw commands that arrive before the LCD is ready stay in
    // the RX queue and are processed once loop() starts. READ_RAW before the canvas exists
    // returns 0 through the _canvas_ready guard.
#if defined(CONFIG_IDF_TARGET_ESP32)
    // On the original, keep the I2C ISR on core 0 as before, separate from the Arduino loop on core 1.
    if (xTaskCreatePinnedToCore(setup_i2c_on_core0,
                               "i2cSetup",
                               8192,
                               xTaskGetCurrentTaskHandle(),
                               0,
                               nullptr,
                               0) != pdPASS)
    {
      ESP_LOGE(LOGNAME, "I2C setup task creation failed");
    }
#else
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
#endif


#if DEBUG == 1
    lgfx::pinMode(0, lgfx::pin_mode_t::output);
    lgfx::gpio_hi(0);
#endif

    {
      auto cfg = _spi_bus.config();
      cfg.spi_host = board_config::LCD_SPI_HOST;
      // The C61 uses the AHB_DMA fixed in M5GFX 0.2.27; the original ESP32 keeps
      // DMA channel 2 as shipped.
      cfg.dma_channel = board_config::LCD_DMA_CHANNEL;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 14000000;
      cfg.pin_mosi = board_config::PIN_LCD_MOSI;
      cfg.pin_miso = board_config::PIN_LCD_MISO;
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
    // Only the splash is drawn in landscape. The panel is a 135 x 240 portrait and the model
    // name in Font4 would be cut off. The coordinate system exposed to the host stays at
    // rotation 0, so restore it afterwards (the default orientation seen by the protocol is unchanged).
    _lcd.setRotation(SPLASH_ROTATION);
    _lcd.fillScreen(TFT_WHITE);
    ESP_LOGI(LOGNAME, "LCD fillScreen complete");
    _lcd.setTextColor(TFT_BLACK, TFT_WHITE);

    drawSplash(_lcd);
    ESP_LOGI(LOGNAME, "LCD startup text complete");
    _lcd.setColorDepth(24);
    _canvas.setColorDepth (24);

    // Keep the address/version splash visible until the host sends a draw command.
    _modified = false;
    _lcd.setRotation(0);
    _lcd.setWindow(0, 0, _lcd.width()-1, _lcd.height()-1);
    const auto canvas_width = _lcd.width();
    const auto canvas_height = _lcd.height();
    if (_canvas.createSprite(canvas_width, canvas_height) == nullptr)
    {
      ESP_LOGE(LOGNAME, "LCD canvas allocation failed: %d x %d x 3 bytes",
               static_cast<int>(canvas_width), static_cast<int>(canvas_height));
      abort();
    }
    // The splash was drawn directly to the panel. Mirror it into the backing
    // store so the first partial host update starts from the same frame.
    // Draw in the same orientation as the panel, then return to rotation 0. The sprite rotation
    // only changes the coordinate transform while drawing, so the picture stays after restoring it.
    _canvas.setRotation(SPLASH_ROTATION);
    _canvas.fillScreen(TFT_WHITE);
    _canvas.setTextColor(TFT_BLACK, TFT_WHITE);
    // drawSplash(_canvas);
    _canvas.setRotation(0);
    _dirty_valid = false;
    _canvas_ready = true;
    if constexpr (!board_config::LCD_ASYNC_DMA)
    {
      _lcd.endWrite();
    }
    ESP_LOGI(LOGNAME, "LCD canvas complete; asynchronous DMA=%d",
             board_config::LCD_ASYNC_DMA);

    cpu_clock::init();
    cpu_clock::request_clock_down(
      static_cast<cpu_clock::cpu_clock_t>(board_config::CPU_POWER_MIN_MHZ[1]));
    set_power_mode(1);
    ESP_LOGI(LOGNAME, "CPU clock setup complete");

#if UNIT_LCD_HAS_ENCODER
    const bool encoder_init_ok = encoder_input::setup();
    ESP_LOGI(LOGNAME, "Encoder init=%d A=%d B=%d button=%d",
             encoder_init_ok,
             board_config::PIN_ENCODER_A,
             board_config::PIN_ENCODER_B,
             board_config::PIN_ENCODER_BUTTON);
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C61)
    const bool local_ui_init_ok = local_ui::init(_lcd,
                                                 _i2c_addr,
                                                 FIRMWARE_VERSION,
                                                 FIRMWARE_DEBUG);
    ESP_LOGI(LOGNAME, "Local utility UI init=%d", local_ui_init_ok);
#endif

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
        i2c_slave::shutdown();
        esp_restart();
      }
      taskYIELD();
    }
    lgfx::pinMode(0, lgfx::pin_mode_t::input);
    lgfx::pinMode(1, lgfx::pin_mode_t::input);
    lgfx::pinMode(3, lgfx::pin_mode_t::input);
  //*/
  }

  display_owner_t display_owner(void)
  {
    return static_cast<display_owner_t>(_display_owner);
  }

  bool host_owns_display(void)
  {
    return display_owner() == display_owner_t::host_owned;
  }

  bool firmware_update_owns_display(void)
  {
    return display_owner() == display_owner_t::firmware_update;
  }

  /// メインループ処理 蓄積したコマンドの処理およびLCDへの出力処理
  void loop(void)
  {
    ulTaskNotifyTake( pdTRUE, 0 );
    static std::uint32_t last_diagnostic_interrupts = 0;
    i2c_slave::diagnostics_t diagnostics;
    i2c_slave::get_diagnostics(&diagnostics);
    if (diagnostics.interrupts != last_diagnostic_interrupts)
    {
      last_diagnostic_interrupts = diagnostics.interrupts;
      ESP_LOGI(LOGNAME,
               "I2C diag irq=%u status=0x%08x rx=%u txevt=%u txbytes=%u txcap=%u stretch(addr=%u tx=%u other=%u) backpressure(hold=%u timeout=%u) cmd=0x%02x dropped=%u",
               diagnostics.interrupts,
               diagnostics.last_status,
               diagnostics.rx_bytes,
               diagnostics.tx_fifo_events,
               diagnostics.tx_bytes,
               diagnostics.tx_capacity_violations,
               diagnostics.stretch_address,
               diagnostics.stretch_tx_empty,
               diagnostics.stretch_other,
               diagnostics.rx_backpressure_holds,
               diagnostics.rx_backpressure_timeouts,
               static_cast<unsigned>(_last_command),
               static_cast<unsigned>(_rx_dropped_count));
    }
    // A previous full-canvas commit may still be in flight on the C61. No
    // producer may mutate the canvas until that DMA source is released.
    if (_spi_bus.busy())
    {
      _lcd.waitDMA();
    }

    bool processed_command = false;
    while (command())
    {
      processed_command = true;
    }
    service_local_address_change();
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    // LVGL and the local controller are owned exclusively by this Arduino task.
    // The presenter merges tiles into the legacy canvas; the final flush below
    // remains the only physical LCD commit for local UI frames.
    local_ui::handle();
#endif
    if (!processed_command && !_modified && _firmupdate_state == firmupdate_state_t::nothing)
    {
      cpu_clock::request_clock_down(
        static_cast<cpu_clock::cpu_clock_t>(board_config::CPU_IDLE_MHZ));
      // The C61 polls in case notifications are suppressed during flash work; the dual-core
      // original blocks until notified as shipped and keeps its 20 MHz idle.
      const TickType_t wait_ticks = board_config::IDLE_POLL_INTERVAL_MS
                                  ? pdMS_TO_TICKS(board_config::IDLE_POLL_INTERVAL_MS)
                                  : portMAX_DELAY;
      ulTaskNotifyTake(pdTRUE, wait_ticks);
      cpu_clock::request_clock_up(
        static_cast<cpu_clock::cpu_clock_t>(board_config::CPU_ACTIVE_MHZ));
    }
    // The framebuffer is committed after the commands currently available in
    // the RX queue have been applied. C61 leaves DMA running asynchronously;
    // the original ESP32 closes the transaction to release M5GFX's DMA guard.
    if (_modified && !_spi_bus.busy())
    {
      _modified = false;
      // The panel's native 24-bit stream can be transferred without conversion.
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

  static inline void IRAM_ATTR markFirmupdateActivity(void)
  {
    _firmupdate_last_activity_tick = xTaskGetTickCountFromISR();
  }

  static inline void IRAM_ATTR expireAbandonedFirmupdate(void)
  {
    if (_firmupdate_state == firmupdate_state_t::nothing || !_nvs_push || _param_index != 0) return;
    const TickType_t now = xTaskGetTickCountFromISR();
    if (now - _firmupdate_last_activity_tick < FIRMUPDATE_INACTIVITY_TIMEOUT_TICKS) return;

    // Abandon the session if no IAP byte arrives for 5 seconds. Clearing only _nvs_push would
    // let a partial payload or UPDATE_END be processed as the old session, so close the state and parser too.
    // If the parser is mid-command, do not close yet; defer expiry to the next command boundary.
    // Recovering a few bytes late is safer than splitting an in-flight, order-dependent draw command.
    // The check is a tick comparison in the ISR when the next write byte arrives; no timer task or logging.
    _nvs_push = false;
    _firmupdate_state = firmupdate_state_t::nothing;
    closeData();
  }

  bool IRAM_ATTR rxNeedsBackpressure(void)
  {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    if (_rx_pending_enqueue) return true;
    const auto free_slots = (_rx_buffer_getpos - _rx_buffer_setpos) & (RX_BUFFER_MAX - 1);
    // Under sustained backpressure a 1-byte read command still lands at the tail of the hardware FIFO.
    // Stop at a reserve that lets the whole FIFO be parsed in order down to the reply command.
    return free_slots != 0 && free_slots <= RX_BACKPRESSURE_RESERVE;
#else
    return false;
#endif
  }

  bool IRAM_ATTR rxCanDrainReadCommand(std::size_t rx_bytes)
  {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    if (_rx_pending_enqueue) return false;
    auto free_slots = (_rx_buffer_getpos - _rx_buffer_setpos) & (RX_BUFFER_MAX - 1);
    if (free_slots == 0) free_slots = RX_BUFFER_MAX;
    // Keep the current parser slot even in the worst case where every byte completes a command.
    return free_slots > rx_bytes + RX_DRAIN_PARSER_SLOTS;
#else
    return false;
#endif
  }

  bool IRAM_ATTR rxCanCaptureReadCommand(void)
  {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    // The IAP payload arrives in a single write transaction; while in progress every byte goes
    // to the current UPDATE_DATA. If a status read starts while the last byte is still in the
    // FIFO at the high-water mark, leave it for the normal parser drain instead of taking it as a command.
    // During sector_write the host reads without resending the command, so keep the latest
    // UPDATE_DATA and return live status.
    return _firmupdate_state != firmupdate_state_t::progress
        && _firmupdate_state != firmupdate_state_t::sector_write;
#else
    return false;
#endif
  }

  bool IRAM_ATTR tryAcceptReadCommand(std::uint8_t value)
  {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    // While a completed command is parked in the parser slot, addData() must not overwrite
    // its first byte. Hand the byte back to the deferred path of the transport.
    if (_rx_pending_enqueue) return false;
    switch (value)
    {
    case lgfx::Panel_M5UnitLCD::CMD_READ_ID:
    case lgfx::Panel_M5UnitLCD::CMD_READ_BUFCOUNT:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_8:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_16:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_24:
#if UNIT_LCD_HAS_ENCODER
    case unit_lcd_plus_protocol::CMD_READ_ENCODER_STATE:
    case unit_lcd_plus_protocol::CMD_PEEK_ENCODER_EVENT:
    case unit_lcd_plus_protocol::CMD_READ_BUTTON_CONFIG:
#endif
      // Apply the Sr of W->Sr->R as a parser boundary first, then complete only the 1-byte read
      // command, which consumes no queue entry, through the normal addData path.
      closeData();
      addData(value);
      return true;

    default:
      return false;
    }
#else
    return false;
#endif
  }

  static bool IRAM_ATTR enqueueCompletedCommand(void)
  {
    const auto new_setpos = (_rx_buffer_setpos + 1) & (RX_BUFFER_MAX - 1);
    if (new_setpos == _rx_buffer_getpos)
    {
      return false;
    }
    _rx_buffer_setpos = new_setpos;
    _param_index = _param_resetindex;
    for (std::size_t i = 0; i < _param_resetindex; ++i)
    {
      _rx_buffer[new_setpos][i] = _params[i];
    }
    _params = _rx_buffer[new_setpos];
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    _rx_pending_enqueue = false;
#endif
    return true;
  }

  bool IRAM_ATTR retryPendingRxCommand(void)
  {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    return !_rx_pending_enqueue || enqueueCompletedCommand();
#else
    return true;
#endif
  }

  void IRAM_ATTR discardRxForBackpressureTimeout(void)
  {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    _rx_dropped_count = _rx_dropped_count + 1;
    if ((_param_index != 0 || _rx_pending_enqueue)
     && _params[0] == lgfx::Panel_M5UnitLCD::CMD_CHANGE_ADDR)
    {
      _nvs_push = false;
    }
    _rx_pending_enqueue = false;
    closeData();
#endif
  }

  /// I2CペリフェラルISRから1Byteずつデータを受取る処理
  bool IRAM_ATTR addData(std::uint8_t value)
  {
    expireAbandonedFirmupdate();
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
      // RAM_FILL was missing from the classifier and became a NOP as an undefined command before reaching the executor.
      // CMD_SET_COLOR shares the value 0x50, so accept it under the RAM_FILL name as a 1-byte command without a duplicate case.
      case lgfx::Panel_M5UnitLCD::CMD_RAM_FILL:
      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_8:
      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_16:
      case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_24:
#if UNIT_LCD_HAS_ENCODER
      case unit_lcd_plus_protocol::CMD_READ_ENCODER_STATE:
      case unit_lcd_plus_protocol::CMD_PEEK_ENCODER_EVENT:
      case unit_lcd_plus_protocol::CMD_READ_BUTTON_CONFIG:
#endif
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

#if UNIT_LCD_HAS_ENCODER
      case unit_lcd_plus_protocol::CMD_ACK_ENCODER_EVENT:
      case unit_lcd_plus_protocol::CMD_SET_BUTTON_CONFIG:
        _param_need_count = 3;
        return false;

      case unit_lcd_plus_protocol::CMD_SET_ENCODER_POSITION:
        _param_need_count = 5;
        return false;
#endif
      }
    }

    // Only UPDATE_DATA/END write bytes count as IAP activity. Plain status reads and
    // READ_BUFCOUNT polls do not extend the deadline; a legitimately slow block extends it on every byte.
    if (_firmupdate_state != firmupdate_state_t::nothing
     && (_params[0] == lgfx::Panel_M5UnitLCD::CMD_UPDATE_DATA
      || _params[0] == lgfx::Panel_M5UnitLCD::CMD_UPDATE_END))
    {
      markFirmupdateActivity();
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
#if defined(CONFIG_IDF_TARGET_ESP32)
      // The original cannot wait on a read address match, so keep prefetching when the command completes.
      i2c_slave::clear_txdata();
      beginTxResponse();
#endif
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
          i2c_slave::shutdown();
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
          // Throttling starts with the first UPDATE_DATA header, so a session with only BEGIN
          // never expires. Hosts that need more than 5 s to prepare SD/network are accepted, and
          // a wait_data left behind stays the unthrottled behaviour it always was.
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
            _firmupdate_state = firmupdate_state_t::sector_write;
          }
          else
          {
            // A block with a CRC mismatch is not queued; wait for a resend at the same offset.
            // BROKEN stays the live reply until the next UPDATE_DATA header or UPDATE_BEGIN.
            _firmupdate_result = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_BROKEN;
            update::discardBlock();
            _firmupdate_state = firmupdate_state_t::wait_data;
#if defined(CONFIG_IDF_TARGET_ESP32)
            i2c_slave::fill_txdata();
#endif
            closeData();
            return false;
          }
#if defined(CONFIG_IDF_TARGET_ESP32)
          i2c_slave::fill_txdata();
#endif
          closeData();
        }
        else
        if ((_firmupdate_state != firmupdate_state_t::nothing)
         && (_params[1] == 0x77)
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
#if defined(CONFIG_IDF_TARGET_ESP32)
        i2c_slave::fill_txdata();
#endif
        closeData();
        return false;

      case lgfx::Panel_M5UnitLCD::CMD_READ_ID:
      case lgfx::Panel_M5UnitLCD::CMD_READ_BUFCOUNT:
#if defined(CONFIG_IDF_TARGET_ESP32)
        i2c_slave::fill_txdata();
#endif
        closeData();
        return false;

#if UNIT_LCD_HAS_ENCODER
      case unit_lcd_plus_protocol::CMD_READ_ENCODER_STATE:
        encoder_input::read_state(_encoder_state_response);
#if defined(CONFIG_IDF_TARGET_ESP32)
        beginTxResponse();
        i2c_slave::fill_txdata();
#endif
        closeData();
        return false;

      case unit_lcd_plus_protocol::CMD_PEEK_ENCODER_EVENT:
        encoder_input::peek_event(_encoder_event_response);
#if defined(CONFIG_IDF_TARGET_ESP32)
        beginTxResponse();
        i2c_slave::fill_txdata();
#endif
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
#if defined(CONFIG_IDF_TARGET_ESP32)
        beginTxResponse();
        i2c_slave::fill_txdata();
#endif
        closeData();
        return false;

      case unit_lcd_plus_protocol::CMD_SET_BUTTON_CONFIG:
        encoder_input::set_button_config(unit_lcd_plus_protocol::read_u16_be(&_params[1]));
        closeData();
        return false;
#endif
      }

      /// Queue insertion. The C61 stops the transport at a high-water mark that leaves room for
      /// the whole FIFO and the parser slot, so the queue normally never fills. If it does, the
      /// completed command is parked until main frees an entry. The original ESP32 cannot
      /// stretch on address match, so it drops the new command as before. Old commands are kept
      /// because drawing is order dependent and stopping after command N is self-consistent.
      if (!enqueueCompletedCommand())
      {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
        // Normally unreachable thanks to the reserved slots. Even when completely full, keep the
        // completed command in the parser slot and hold the transport until main frees an entry.
        _rx_pending_enqueue = true;
        return _nvs_push ? false : true;
#else
        _rx_dropped_count = _rx_dropped_count + 1;
        if (_params[0] == lgfx::Panel_M5UnitLCD::CMD_CHANGE_ADDR)
        {
          /// Leaving _nvs_push set for a dropped CHANGE_ADDR would keep READ_BUFCOUNT stuck
          /// at 1, so forget the request entirely
          _nvs_push = false;
        }
        /// The parser continues in the current slot. The prefix of a variable-length command
        /// (_param_resetindex bytes) is still in the current slot, so no copy is needed.
        _param_index = _param_resetindex;
        return false;
#endif
      }


      // NVS領域やファームウェアへの書き込みはタスク通知を使うとクラッシュするのでfalseを返す
      return _nvs_push ? false : true;
    }
    return false;
  }

  void IRAM_ATTR beginTxResponse(void)
  {
    _tx_response_command = static_cast<std::uint8_t>(_last_command);
    _tx_response_cursor = 0;
    _tx_response_length = 1;
    _tx_response_snapshot[0] = 0xff;

    switch (_tx_response_command)
    {
    default:
      break;

    case lgfx::Panel_M5UnitLCD::CMD_READ_ID:
      _tx_response_length = sizeof(read_id_data);
      memcpy(_tx_response_snapshot, read_id_data, _tx_response_length);
      break;

#if UNIT_LCD_HAS_ENCODER
    case unit_lcd_plus_protocol::CMD_READ_ENCODER_STATE:
      _tx_response_length = sizeof(_encoder_state_response);
      memcpy(_tx_response_snapshot, _encoder_state_response, _tx_response_length);
      break;

    case unit_lcd_plus_protocol::CMD_PEEK_ENCODER_EVENT:
      _tx_response_length = sizeof(_encoder_event_response);
      memcpy(_tx_response_snapshot, _encoder_event_response, _tx_response_length);
      break;

    case unit_lcd_plus_protocol::CMD_READ_BUTTON_CONFIG:
      _tx_response_length = sizeof(_button_config_response);
      memcpy(_tx_response_snapshot, _button_config_response, _tx_response_length);
      break;
#endif

    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_8:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_16:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_24:
      _tx_response_length = 0;
      // Sections 3.3/12 define pixel continuity within a single read transaction, with the
      // read pointer starting at the top-left of the window. The released host readRect() also
      // reads everything in one readBytes, so on the C61 the cursor is deliberately returned to
      // the window start on every address match; it does not continue across plain reads.
      _read_xptr = _read_xs;
      _read_yptr = _read_ys;
      break;
    }
  }

  std::size_t IRAM_ATTR produceTx(std::uint8_t* destination, std::size_t capacity)
  {
    if (destination == nullptr || capacity == 0)
    {
      return 0;
    }

    switch (_tx_response_command)
    {
    // The released host sends the command once and then repeats plain reads waiting for the
    // value to change, so these two replies are re-evaluated on every refill instead of snapshotted at begin.
    case lgfx::Panel_M5UnitLCD::CMD_UPDATE_DATA:
      destination[0] = static_cast<std::uint8_t>(_firmupdate_result);
      return 1;

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
            // READ_BUFCOUNT advertises free space of the 512-entry window shared with the
            // released host in units of 2 entries, not the whole physical queue. The 0..255
            // semantics seen by the host stay the same even though queue sizes differ per target.
            std::int32_t buf_free = gp - sp;
            if (buf_free > 0) buf_free -= RX_BUFFER_MAX;
            //
            buf_free = (buf_free + ADVERTISED_FLOW_WINDOW) >> 1;
            if (buf_free < 1)
            {
              res = 1;
            }
            else if (buf_free > 254)
            {
              res = 254;
            }
            else
            {
              res = static_cast<std::uint32_t>(buf_free);
            }
          }
        }
        destination[0] = static_cast<std::uint8_t>(res);
        return 1;
      }

    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_8:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_16:
    case lgfx::Panel_M5UnitLCD::CMD_READ_RAW_24:
      {
        const std::size_t bytes_per_pixel = _tx_response_command & 3;
        const std::size_t available_pixels = capacity / bytes_per_pixel;
        const std::size_t pixel_count = available_pixels < 8 ? available_pixels : 8;
        std::size_t produced = 0;
        for (std::size_t i = 0; i < pixel_count; ++i)
        {
          std::uint32_t res = _canvas_ready
                            ? _canvas.readPixelValue(_read_xptr, _read_yptr)
                            : 0;
          switch (bytes_per_pixel)
          {
            case 2: res = lgfx::color_convert<lgfx::swap565_t, lgfx::bgr888_t>(res); break;
            case 1: res = lgfx::color_convert<lgfx::rgb332_t, lgfx::bgr888_t>(res); break;
            default: break;
          }
          if (_byteswap)
          {
            switch (bytes_per_pixel)
            {
              case 3: res = lgfx::getSwap24(res); break;
              case 2: res = lgfx::getSwap16(res); break;
              default: break;
            }
          }
          memcpy(destination + produced, &res, bytes_per_pixel);
          produced += bytes_per_pixel;
          if (++_read_xptr > _read_xe)
          {
            _read_xptr = _read_xs;
            if (++_read_yptr > _read_ye)
            {
              _read_yptr = _read_ys;
            }
          }
        }
        return produced;
      }

    default:
      {
        if (_tx_response_length == 0) return 0;
        if (_tx_response_cursor == _tx_response_length) _tx_response_cursor = 0;
        const auto remaining = _tx_response_length - _tx_response_cursor;
        const auto produced = capacity < remaining ? capacity : remaining;
        memcpy(destination, _tx_response_snapshot + _tx_response_cursor, produced);
        _tx_response_cursor += produced;
        return produced;
      }
    }
  }
}
