#pragma once

#include <cstdint>

#include <M5GFX.h>

namespace local_ui
{
  // Initializes the C61-only LVGL utility UI after the legacy M5GFX panel and
  // shared host canvas are ready. The function is deliberately independent of
  // the I2C ISR; all LVGL work remains in the Arduino owner task.
  bool init(LGFX_Device& lcd,
            std::uint8_t address,
            std::uint8_t firmware_major,
            std::uint8_t firmware_minor,
            bool debug_build);

  void handle(void);
  void deinit(void);
  bool initialized(void);
}
