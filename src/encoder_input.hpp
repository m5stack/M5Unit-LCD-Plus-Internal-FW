//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <cstdint>
#include <esp_attr.h>

namespace encoder_input
{
  bool setup(void);
  void IRAM_ATTR read_state(std::uint8_t* response);
  void IRAM_ATTR peek_event(std::uint8_t* response);
  void IRAM_ATTR acknowledge(std::uint16_t sequence);
  void IRAM_ATTR set_position(std::int32_t position);
  void IRAM_ATTR read_button_config(std::uint8_t* response);
  void IRAM_ATTR set_button_config(std::uint16_t long_press_ms);
}
