//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#include <cstdint>
#include <esp_attr.h>

namespace encoder_input
{
  // Local UI events use a separate queue from the protocol queue. The host's
  // PEEK/ACK cursor is never advanced when the LVGL task polls this sink.
  // Rotation is reported once per mechanical detent (two raw quadrature edges)
  // with delta +1/-1; the host queue keeps its original edge resolution.
  struct local_event_t
  {
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::int16_t delta = 0;
    std::int32_t position = 0;
  };

  bool setup(void);
  void suspend_for_flash_write(void);
  void resume_after_flash_write(void);

  // The GPIO ISR service is not in IRAM and IDF masks it while the cache is off, so this
  // guard is not a crash guard. It drains a running ISR before the stop and re-anchors
  // deterministically to the current GPIO state on resume. Without it an edge latched
  // during the stop could join the pre-stop state and count three transitions as one reversed transition.
  class flash_guard_t
  {
  public:
    flash_guard_t();
    ~flash_guard_t();

    flash_guard_t(const flash_guard_t&) = delete;
    flash_guard_t& operator=(const flash_guard_t&) = delete;
  };

  void IRAM_ATTR read_state(std::uint8_t* response);
  void IRAM_ATTR peek_event(std::uint8_t* response);
  void IRAM_ATTR acknowledge(std::uint16_t sequence);
  void IRAM_ATTR set_position(std::int32_t position);
  void IRAM_ATTR read_button_config(std::uint8_t* response);
  void IRAM_ATTR set_button_config(std::uint16_t long_press_ms);

  // Atomically takes one event from the local-only queue. Returns false when
  // no event is pending. This is intended for the single LVGL owner task.
  bool poll_local_event(local_event_t* event);
  void clear_local_events(void);
}
