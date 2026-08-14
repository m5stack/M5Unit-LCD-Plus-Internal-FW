//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <cstdint>
#include <driver/gpio.h>

namespace board_config
{
  static constexpr gpio_num_t PIN_ENCODER_A = GPIO_NUM_2;
  static constexpr gpio_num_t PIN_ENCODER_B = GPIO_NUM_3;
  static constexpr gpio_num_t PIN_ENCODER_BUTTON = GPIO_NUM_4;

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

  static constexpr std::uint16_t LCD_WIDTH = 135;
  static constexpr std::uint16_t LCD_HEIGHT = 240;
  static constexpr std::uint16_t LCD_OFFSET_X = 52;
  static constexpr std::uint16_t LCD_OFFSET_Y = 40;
}
