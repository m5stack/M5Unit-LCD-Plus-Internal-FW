//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <cstddef>
#include <cstdint>

namespace unit_lcd_plus_protocol
{
  // Factory-programmed MAC-48 in normal display order (first octet first).
  static constexpr std::uint8_t CMD_READ_MAC = 0x0A;
  static constexpr std::size_t MAC_ADDRESS_SIZE = 6;

  static constexpr std::uint8_t CMD_READ_ENCODER_STATE = 0xB0;
  static constexpr std::uint8_t CMD_PEEK_ENCODER_EVENT = 0xB1;
  static constexpr std::uint8_t CMD_ACK_ENCODER_EVENT = 0xB2;
  static constexpr std::uint8_t CMD_SET_ENCODER_POSITION = 0xB3;
  static constexpr std::uint8_t CMD_READ_BUTTON_CONFIG = 0xB4;
  static constexpr std::uint8_t CMD_SET_BUTTON_CONFIG = 0xB5;

  static constexpr std::size_t ENCODER_STATE_SIZE = 8;
  static constexpr std::size_t ENCODER_EVENT_SIZE = 10;
  static constexpr std::size_t BUTTON_CONFIG_SIZE = 2;
  static constexpr std::size_t ENCODER_QUEUE_CAPACITY = 64;

  static_assert(ENCODER_STATE_SIZE <= 32, "encoder state must fit in the I2C FIFO");
  static_assert(ENCODER_EVENT_SIZE <= 32, "encoder event must fit in the I2C FIFO");
  static_assert(BUTTON_CONFIG_SIZE <= 32, "button config must fit in the I2C FIFO");

  static constexpr std::uint16_t BUTTON_LONG_PRESS_DEFAULT_MS = 500;
  static constexpr std::uint16_t BUTTON_LONG_PRESS_MIN_MS = 500;
  static constexpr std::uint16_t BUTTON_LONG_PRESS_MAX_MS = 5000;
  static constexpr std::uint16_t BUTTON_DOUBLE_CLICK_MS = 300;

  enum encoder_flag_t : std::uint8_t
  {
    ENCODER_FLAG_BUTTON_PRESSED = 1U << 0,
    ENCODER_FLAG_BUTTON_LONG_PRESSED = 1U << 1,
    ENCODER_FLAG_EVENT_PENDING = 1U << 2,
    ENCODER_FLAG_OVERFLOW = 1U << 7,
  };

  enum encoder_event_type_t : std::uint8_t
  {
    ENCODER_EVENT_NONE = 0,
    ENCODER_EVENT_ROTATE = 1,
    ENCODER_EVENT_PRESS = 2,
    ENCODER_EVENT_RELEASE = 3,
    ENCODER_EVENT_SINGLE_CLICK = 4,
    ENCODER_EVENT_CLICK = ENCODER_EVENT_SINGLE_CLICK,
    ENCODER_EVENT_LONG_PRESS = 5,
    ENCODER_EVENT_DOUBLE_CLICK = 6,
  };

  inline std::uint16_t read_u16_be(const std::uint8_t* data)
  {
    return static_cast<std::uint16_t>(data[0] << 8) | data[1];
  }

  inline std::int16_t read_i16_be(const std::uint8_t* data)
  {
    return static_cast<std::int16_t>(read_u16_be(data));
  }

  inline std::int32_t read_i32_be(const std::uint8_t* data)
  {
    const auto value = (static_cast<std::uint32_t>(data[0]) << 24)
                     | (static_cast<std::uint32_t>(data[1]) << 16)
                     | (static_cast<std::uint32_t>(data[2]) << 8)
                     | data[3];
    return static_cast<std::int32_t>(value);
  }

  inline void write_u16_be(std::uint8_t* data, std::uint16_t value)
  {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
  }

  inline void write_i16_be(std::uint8_t* data, std::int16_t value)
  {
    write_u16_be(data, static_cast<std::uint16_t>(value));
  }

  inline void write_i32_be(std::uint8_t* data, std::int32_t value)
  {
    const auto encoded = static_cast<std::uint32_t>(value);
    data[0] = static_cast<std::uint8_t>(encoded >> 24);
    data[1] = static_cast<std::uint8_t>(encoded >> 16);
    data[2] = static_cast<std::uint8_t>(encoded >> 8);
    data[3] = static_cast<std::uint8_t>(encoded);
  }
}
