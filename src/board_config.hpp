//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <cstddef>
#include <cstdint>
#include <driver/gpio.h>
#include <driver/spi_master.h>

#include "common.hpp"

#if defined(CONFIG_IDF_TARGET_ESP32C61)
 #define UNIT_LCD_HAS_ENCODER 1
#elif defined(CONFIG_IDF_TARGET_ESP32)
 #define UNIT_LCD_HAS_ENCODER 0
#else
 #error "Unit LCD firmware supports only ESP32 and ESP32-C61"
#endif

namespace board_config
{
#if defined(CONFIG_IDF_TARGET_ESP32C61)
  static constexpr gpio_num_t PIN_ENCODER_A = GPIO_NUM_2;
  static constexpr gpio_num_t PIN_ENCODER_B = GPIO_NUM_3;
  static constexpr gpio_num_t PIN_ENCODER_BUTTON = GPIO_NUM_9;

  static constexpr gpio_num_t PIN_I2C_SDA = GPIO_NUM_5;
  static constexpr gpio_num_t PIN_I2C_SCL = GPIO_NUM_6;

  static constexpr gpio_num_t PIN_UART_RX = GPIO_NUM_10;
  static constexpr gpio_num_t PIN_UART_TX = GPIO_NUM_11;

  static constexpr gpio_num_t PIN_LCD_RST = GPIO_NUM_23;
  static constexpr gpio_num_t PIN_LCD_DC = GPIO_NUM_22;
  static constexpr gpio_num_t PIN_LCD_MOSI = GPIO_NUM_28;
  static constexpr gpio_num_t PIN_LCD_SCLK = GPIO_NUM_27;
  static constexpr gpio_num_t PIN_LCD_CS = GPIO_NUM_26;
  static constexpr gpio_num_t PIN_LCD_BL = GPIO_NUM_25;

  static constexpr spi_host_device_t LCD_SPI_HOST = SPI2_HOST;
  // M5GFX 0.2.27 fixes the C61 AHB_DMA descriptor address register.
  // A nonzero value requests automatic DMA channel allocation on current ESP-IDF.
  static constexpr std::uint8_t LCD_DMA_CHANNEL = 1;
  static constexpr std::int8_t PIN_LCD_MISO = -1;
  static constexpr bool LCD_BACKLIGHT_INVERTED = false;
  static constexpr bool LCD_ASYNC_DMA = true;

  static constexpr std::uint16_t CPU_IDLE_MHZ = 40;
  static constexpr std::uint16_t CPU_ACTIVE_MHZ = 160;
  static constexpr std::uint16_t CPU_POWER_MIN_MHZ[] = { 40, 80, 160 };
  static constexpr std::uint16_t CPU_POWER_MAX_MHZ[] = { 40, 160, 160 };
  static constexpr std::uint16_t IDLE_POLL_INTERVAL_MS = 1;
  static constexpr model_id_t MODEL_ID = model_id_t::unit_lcd_plus;
  static constexpr char MODEL_NAME[] = "Unit LCD Plus";
  static constexpr std::uint8_t DEFAULT_FIRMWARE_VERSION = UNIT_LCD_DEFAULT_FIRMWARE_VERSION;
  // The C61 clock-stretches at the queue high-water mark, so a queue deeper than the
  // 512-entry flow-control window the host uses is unnecessary. Returning static DRAM
  // to the heap guarantees a contiguous stack for the OTA task.
  static constexpr std::size_t RX_QUEUE_CAPACITY = 0x0200;
#else
  // Wiring and operating conditions of the shipped original Unit LCD (ESP32-PICO-D4).
  static constexpr gpio_num_t PIN_I2C_SDA = GPIO_NUM_21;
  static constexpr gpio_num_t PIN_I2C_SCL = GPIO_NUM_22;

  static constexpr gpio_num_t PIN_UART_RX = GPIO_NUM_3;
  static constexpr gpio_num_t PIN_UART_TX = GPIO_NUM_1;

  static constexpr gpio_num_t PIN_LCD_RST = GPIO_NUM_18;
  static constexpr gpio_num_t PIN_LCD_DC = GPIO_NUM_23;
  static constexpr gpio_num_t PIN_LCD_MOSI = GPIO_NUM_15;
  static constexpr gpio_num_t PIN_LCD_SCLK = GPIO_NUM_13;
  static constexpr gpio_num_t PIN_LCD_CS = GPIO_NUM_5;
  static constexpr gpio_num_t PIN_LCD_BL = GPIO_NUM_4;

  // VSPI_HOST is an alias of SPI3_HOST in Arduino-ESP32 3.x.
  static constexpr spi_host_device_t LCD_SPI_HOST = SPI3_HOST;
  static constexpr std::uint8_t LCD_DMA_CHANNEL = 2;
  static constexpr std::int8_t PIN_LCD_MISO = GPIO_NUM_14;
  static constexpr bool LCD_BACKLIGHT_INVERTED = true;
  // M5GFX's original-ESP32 DMA workaround is released by endTransaction().
  static constexpr bool LCD_ASYNC_DMA = false;

  static constexpr std::uint16_t CPU_IDLE_MHZ = 20;
  static constexpr std::uint16_t CPU_ACTIVE_MHZ = 240;
  static constexpr std::uint16_t CPU_POWER_MIN_MHZ[] = { 20, 80, 240 };
  static constexpr std::uint16_t CPU_POWER_MAX_MHZ[] = { 20, 160, 240 };
  static constexpr std::uint16_t IDLE_POLL_INTERVAL_MS = 0;
  // The shipped original already returns 0 in READ_ID byte 2; keep that value as its model id.
  static constexpr model_id_t MODEL_ID = model_id_t::unit_lcd;
  static constexpr char MODEL_NAME[] = "Unit LCD";
  static constexpr std::uint8_t DEFAULT_FIRMWARE_VERSION = UNIT_LCD_DEFAULT_FIRMWARE_VERSION;
  // So the original can hold the canvas and the command queue at once, keep the queue in
  // static DRAM but shrink it to 0x1000 entries, still far above the 512 entries advertised by READ_BUFCOUNT.
  static constexpr std::size_t RX_QUEUE_CAPACITY = 0x1000;
#endif

  static constexpr bool HAS_ENCODER = UNIT_LCD_HAS_ENCODER;
  // The window advertised by READ_BUFCOUNT is a host protocol contract independent of the physical queue size.
  // Both the deep queue of the original and the 512-entry queue of the C61 return the same 0..255.
  static constexpr std::size_t ADVERTISED_FLOW_WINDOW = 0x0200;

  static constexpr std::uint16_t LCD_WIDTH = 135;
  static constexpr std::uint16_t LCD_HEIGHT = 240;
  static constexpr std::uint16_t LCD_OFFSET_X = 52;
  static constexpr std::uint16_t LCD_OFFSET_Y = 40;
}
