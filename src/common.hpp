//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <cstdint>

/// Legacy model IDs; READ_ID now reports firmware major/minor instead.
enum class model_id_t : std::uint8_t
{
  unit_lcd = 0,
  unit_lcd_plus = 1,
};

static constexpr char LOGNAME[] = "M5UnitLCD";
#ifndef UNIT_LCD_FIRMWARE_MAJOR
#define UNIT_LCD_FIRMWARE_MAJOR 1
#endif
#ifndef UNIT_LCD_FIRMWARE_MINOR
#define UNIT_LCD_FIRMWARE_MINOR 2
#endif
static_assert(UNIT_LCD_FIRMWARE_MAJOR >= 0 && UNIT_LCD_FIRMWARE_MAJOR <= 255,
              "firmware major version must fit in READ_ID byte 2");
static_assert(UNIT_LCD_FIRMWARE_MINOR >= 0 && UNIT_LCD_FIRMWARE_MINOR <= 255,
              "firmware minor version must fit in READ_ID byte 3");
static constexpr std::uint8_t FIRMWARE_MAJOR_VERSION = UNIT_LCD_FIRMWARE_MAJOR;
static constexpr std::uint8_t FIRMWARE_MINOR_VERSION = UNIT_LCD_FIRMWARE_MINOR;

// READ_ID returns the plain major and minor bytes, with no build flags.
