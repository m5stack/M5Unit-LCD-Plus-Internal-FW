//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include "board_config.hpp"

static constexpr char LOGNAME[] = "M5UnitLCD";
static constexpr std::uint8_t FIRMWARE_MAJOR_VERSION = 0;
#ifndef UNIT_LCD_FIRMWARE_MINOR
#define UNIT_LCD_FIRMWARE_MINOR board_config::DEFAULT_FIRMWARE_MINOR
#endif
static constexpr std::uint8_t FIRMWARE_MINOR_VERSION = UNIT_LCD_FIRMWARE_MINOR;
