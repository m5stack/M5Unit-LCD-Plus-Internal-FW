//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <Arduino.h>
#include <esp32-hal-cpu.h>
#include <algorithm>
#include <cstdint>

#include "cpu_clock.hpp"

namespace cpu_clock
{
  namespace
  {
    constexpr std::uint32_t clock_mhz[] = {40, 80, 160};

    cpu_clock_t clock_min = clock_80MHz;
    cpu_clock_t clock_max = clock_160MHz;
    cpu_clock_t now_clock = clock_MAX;
    cpu_clock_t requested_clock = clock_160MHz;
  }

  void set_cpu_clock(cpu_clock_t clock)
  {
    if (clock >= clock_MAX || now_clock == clock)
    {
      return;
    }

#if CONFIG_IDF_TARGET_ESP32C61
    // Arduino Core 3.3.6's direct RTC clock switch does not return on C61.
    // Keep the CPU at its boot frequency until the core exposes a safe path.
    now_clock = clock_160MHz;
    return;
#else
    if (setCpuFrequencyMhz(clock_mhz[clock]))
    {
      now_clock = clock;
    }
#endif
  }

  void init(void)
  {
    now_clock = clock_MAX;
    requested_clock = clock_160MHz;
  }

  void request_clock_up(cpu_clock_t clock)
  {
    clock = std::min(clock, clock_max);
    if (requested_clock < clock)
    {
      requested_clock = clock;
      set_cpu_clock(clock);
    }
  }

  void request_clock_down(cpu_clock_t clock)
  {
    clock = std::max(clock, clock_min);
    if (requested_clock > clock)
    {
      requested_clock = clock;
      set_cpu_clock(clock);
    }
  }

  void set_clock_limit(cpu_clock_t minimum, cpu_clock_t maximum)
  {
    clock_min = minimum;
    clock_max = maximum;
    set_cpu_clock(std::min(maximum, std::max(minimum, requested_clock)));
  }
}
