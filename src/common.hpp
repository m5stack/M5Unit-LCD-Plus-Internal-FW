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
static_assert(UNIT_LCD_FIRMWARE_MINOR >= 0 && UNIT_LCD_FIRMWARE_MINOR <= 0x7F,
              "firmware minor version must fit in READ_ID byte 3");
static constexpr std::uint8_t FIRMWARE_MAJOR_VERSION = UNIT_LCD_FIRMWARE_MAJOR;
static constexpr std::uint8_t FIRMWARE_MINOR_VERSION = UNIT_LCD_FIRMWARE_MINOR;

#if DEBUG == 1
static constexpr bool FIRMWARE_DEBUG = true;
#else
static constexpr bool FIRMWARE_DEBUG = false;
#endif

// READ_ID byte 2 is the major version; byte 3 keeps the minor version and advertises
// debug builds in bit 7. Release builds therefore preserve the plain version.
static constexpr std::uint8_t READ_ID_FIRMWARE_MINOR =
    FIRMWARE_MINOR_VERSION | (FIRMWARE_DEBUG ? 0x80 : 0x00);
