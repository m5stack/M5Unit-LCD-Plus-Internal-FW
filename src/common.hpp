//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <cstdint>

/// The MODEL_ID table in docs/Unit_LCD_Plus_I2C_Protocol_zh.md section 6.1 is authoritative; each enumerator maps 1:1 to that value.
enum class model_id_t : std::uint8_t
{
  unit_lcd = 0,
  unit_lcd_plus = 1,
};

static constexpr char LOGNAME[] = "M5UnitLCD";
static constexpr std::uint8_t UNIT_LCD_DEFAULT_FIRMWARE_VERSION = 5;
#ifndef UNIT_LCD_FIRMWARE_VERSION
#define UNIT_LCD_FIRMWARE_VERSION UNIT_LCD_DEFAULT_FIRMWARE_VERSION
#endif
static_assert(UNIT_LCD_FIRMWARE_VERSION <= 0x7F,
              "firmware version must fit in the lower 7 bits of READ_ID byte 3");
static constexpr std::uint8_t FIRMWARE_VERSION = UNIT_LCD_FIRMWARE_VERSION;

#if DEBUG == 1
static constexpr bool FIRMWARE_DEBUG = true;
#else
static constexpr bool FIRMWARE_DEBUG = false;
#endif

// READ_ID byte 3 keeps the shared serial version in bits 0..6 and advertises
// debug builds in bit 7. Release builds therefore preserve the plain version.
static constexpr std::uint8_t READ_ID_FIRMWARE_VERSION =
    FIRMWARE_VERSION | (FIRMWARE_DEBUG ? 0x80 : 0x00);
