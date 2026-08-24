//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <cstdint>

namespace cpu_clock
{
  enum cpu_clock_t : std::uint16_t
  {
    clock_8MHz = 8,
    clock_10MHz = 10,
    clock_20MHz = 20,
    clock_40MHz = 40,
    clock_80MHz = 80,
    clock_160MHz = 160,
    clock_240MHz = 240,
    clock_MAX,
  };

  void init(void);
  void set_cpu_clock(cpu_clock_t clock);
  void set_clock_limit(cpu_clock_t clock_min, cpu_clock_t clock_max);
  void request_clock_up(cpu_clock_t clock);
  void request_clock_down(cpu_clock_t clock);
}
