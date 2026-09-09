//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.
//! Gray-code decoding is adapted from esp-idf-lib/encoder; see LICENSES/encoder-BSD-3-Clause.txt.

#include "encoder_input.hpp"
#include "board_config.hpp"

#if UNIT_LCD_HAS_ENCODER

#include <climits>
#include <cstddef>

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include "unit_lcd_plus_protocol.hpp"

namespace encoder_input
{
  namespace
  {
    using namespace unit_lcd_plus_protocol;

    constexpr std::uint32_t BUTTON_POLLING_INTERVAL_US = 1000;
    constexpr std::uint16_t BUTTON_DEBOUNCE_SAMPLES = 10;
    constexpr std::uint8_t BUTTON_PRESSED_LEVEL = 0;
    // Enum constants have no storage and fold into immediates, so the GPIO ISR never
    // reads a valid-state table in flash. Bit n means transition code n qualifies.
    enum : std::uint16_t
    {
      ENCODER_VALID_TRANSITIONS = 0x6996,     // 0x1, 0x2, 0x4, 0x7, 0x8, 0xB, 0xD, 0xE
      ENCODER_CLOCKWISE_TRANSITIONS = 0x2814, // 0x2, 0x4, 0xB, 0xD
    };

    struct event_t
    {
      std::uint8_t type;
      std::uint8_t flags;
      std::uint16_t sequence;
      std::int16_t delta;
      std::int32_t position;
    };

    // Keep this queue independent from the 64-entry host queue. A 32-entry
    // local queue absorbs a short input burst while the UI task renders and
    // costs only 256 bytes of static DRAM.
    constexpr std::uint8_t LOCAL_QUEUE_CAPACITY = 32;

    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    esp_timer_handle_t timer = nullptr;
    event_t events[ENCODER_QUEUE_CAPACITY] = {};
    local_event_t local_events[LOCAL_QUEUE_CAPACITY] = {};
    std::uint8_t queue_head = 0;
    std::uint8_t queue_count = 0;
    std::uint8_t local_queue_head = 0;
    std::uint8_t local_queue_count = 0;
    std::int8_t local_rotation_accumulator = 0;
    std::uint16_t next_sequence = 0;
    std::uint16_t dropped_count = 0;
    std::uint16_t local_dropped_count = 0;
    std::int32_t position = 0;
    std::uint8_t encoder_code = 0;
    bool button_pressed = false;
    bool button_long_pressed = false;
    bool button_candidate_pressed = false;
    std::uint16_t button_candidate_samples = 0;
    std::uint16_t button_pressed_samples = 0;
    std::uint16_t long_press_ms = BUTTON_LONG_PRESS_DEFAULT_MS;
    std::uint16_t active_long_press_samples = BUTTON_LONG_PRESS_DEFAULT_MS;
    std::uint16_t click_window_samples = 0;
    bool single_click_pending = false;
    bool second_click_pressed = false;
    bool encoder_interrupts_ready = false;

    std::uint8_t IRAM_ATTR state_flags_locked()
    {
      std::uint8_t flags = 0;
      if (button_pressed) flags |= ENCODER_FLAG_BUTTON_PRESSED;
      if (button_long_pressed) flags |= ENCODER_FLAG_BUTTON_LONG_PRESSED;
      if (queue_count != 0) flags |= ENCODER_FLAG_EVENT_PENDING;
      if (dropped_count != 0) flags |= ENCODER_FLAG_OVERFLOW;
      return flags;
    }

    std::uint8_t IRAM_ATTR local_state_flags_locked()
    {
      std::uint8_t flags = 0;
      if (button_pressed) flags |= ENCODER_FLAG_BUTTON_PRESSED;
      if (button_long_pressed) flags |= ENCODER_FLAG_BUTTON_LONG_PRESSED;
      if (local_queue_count != 0) flags |= ENCODER_FLAG_EVENT_PENDING;
      if (local_dropped_count != 0) flags |= ENCODER_FLAG_OVERFLOW;
      return flags;
    }

    void IRAM_ATTR push_local_event_locked(std::uint8_t type, std::int16_t delta)
    {
      if (type == ENCODER_EVENT_ROTATE)
      {
        local_rotation_accumulator = static_cast<std::int8_t>(
          local_rotation_accumulator + delta);
        if (local_rotation_accumulator > -2 && local_rotation_accumulator < 2)
        {
          return;
        }
        delta = local_rotation_accumulator > 0 ? 1 : -1;
        local_rotation_accumulator = static_cast<std::int8_t>(
          local_rotation_accumulator - delta * 2);
      }

      if (local_queue_count == LOCAL_QUEUE_CAPACITY)
      {
        local_queue_head = static_cast<std::uint8_t>(
          (local_queue_head + 1) % LOCAL_QUEUE_CAPACITY);
        --local_queue_count;
        if (local_dropped_count != UINT16_MAX) ++local_dropped_count;
      }

      const auto tail = static_cast<std::uint8_t>(
        (local_queue_head + local_queue_count) % LOCAL_QUEUE_CAPACITY);
      local_events[tail] = {
        .type = type,
        .flags = static_cast<std::uint8_t>(local_state_flags_locked()
                                            | ENCODER_FLAG_EVENT_PENDING),
        .delta = delta,
        .position = position,
      };
      ++local_queue_count;
    }

    void IRAM_ATTR push_event_locked(std::uint8_t type, std::int16_t delta)
    {
      if (queue_count == ENCODER_QUEUE_CAPACITY)
      {
        queue_head = static_cast<std::uint8_t>((queue_head + 1) % ENCODER_QUEUE_CAPACITY);
        --queue_count;
        if (dropped_count != UINT16_MAX) ++dropped_count;
      }

      const auto tail = static_cast<std::uint8_t>((queue_head + queue_count) % ENCODER_QUEUE_CAPACITY);
      events[tail] = {
        .type = type,
        .flags = static_cast<std::uint8_t>(state_flags_locked() | ENCODER_FLAG_EVENT_PENDING),
        .sequence = next_sequence++,
        .delta = delta,
        .position = position,
      };
      ++queue_count;
      // Mirror the event after the host record is complete. The two queues have
      // independent cursors, so local polling cannot consume a host event.
      push_local_event_locked(type, delta);
    }

    std::int32_t IRAM_ATTR saturated_add(std::int32_t value, std::int16_t delta)
    {
      if (delta > 0 && value > INT32_MAX - delta) return INT32_MAX;
      if (delta < 0 && value < INT32_MIN - delta) return INT32_MIN;
      return value + delta;
    }

    void IRAM_ATTR sample_button_locked()
    {
      if (single_click_pending)
      {
        if (click_window_samples < BUTTON_DOUBLE_CLICK_MS) ++click_window_samples;
        if (click_window_samples >= BUTTON_DOUBLE_CLICK_MS)
        {
          single_click_pending = false;
          click_window_samples = 0;
          push_event_locked(ENCODER_EVENT_SINGLE_CLICK, 0);
        }
      }

      const bool raw_pressed = gpio_get_level(board_config::PIN_ENCODER_BUTTON) == BUTTON_PRESSED_LEVEL;
      if (raw_pressed != button_candidate_pressed)
      {
        button_candidate_pressed = raw_pressed;
        button_candidate_samples = 1;
      }
      else if (button_candidate_samples < BUTTON_DEBOUNCE_SAMPLES)
      {
        ++button_candidate_samples;
      }

      if (button_candidate_samples >= BUTTON_DEBOUNCE_SAMPLES && raw_pressed != button_pressed)
      {
        button_pressed = raw_pressed;
        button_pressed_samples = 0;
        if (button_pressed)
        {
          button_long_pressed = false;
          active_long_press_samples = long_press_ms;
          second_click_pressed = single_click_pending;
          if (second_click_pressed)
          {
            single_click_pending = false;
            click_window_samples = 0;
          }
          push_event_locked(ENCODER_EVENT_PRESS, 0);
        }
        else
        {
          const bool short_press = !button_long_pressed;
          button_long_pressed = false;
          push_event_locked(ENCODER_EVENT_RELEASE, 0);
          if (short_press)
          {
            if (second_click_pressed)
            {
              push_event_locked(ENCODER_EVENT_DOUBLE_CLICK, 0);
            }
            else
            {
              single_click_pending = true;
              click_window_samples = 0;
            }
          }
          second_click_pressed = false;
        }
      }

      if (button_pressed && !button_long_pressed)
      {
        if (button_pressed_samples < active_long_press_samples) ++button_pressed_samples;
        if (button_pressed_samples >= active_long_press_samples)
        {
          button_long_pressed = true;
          single_click_pending = false;
          click_window_samples = 0;
          second_click_pressed = false;
          push_event_locked(ENCODER_EVENT_LONG_PRESS, 0);
        }
      }
    }

    void IRAM_ATTR sample_encoder_locked()
    {
      encoder_code = static_cast<std::uint8_t>((encoder_code << 2)
                    | gpio_get_level(board_config::PIN_ENCODER_A)
                    | (gpio_get_level(board_config::PIN_ENCODER_B) << 1));
      encoder_code &= 0x0F;
      if (!(ENCODER_VALID_TRANSITIONS & (1U << encoder_code)))
      {
        // Discard re-samples of the same state and diagonal transitions whose direction cannot be determined.
        return;
      }

      // Report every valid transition (quadrature edge) as +/-1. One mechanical detent is two
      // Gray-code transitions (half a cycle), so a detent advances the count by 2.
      // Expose the highest resolution to the host; a host that wants detent units divides
      // position by 2 or pairs ROTATE events. The reverse is impossible (a host cannot
      // raise a lower resolution), which is why edges are the default.
      // A reversed transition adds with the opposite sign, so net displacement is preserved regardless of path.
      // Contact bounce between adjacent states shows up as a +1/-1 pair. No waiting, loops or
      // logging inside the ISR, and no extra time filter.
      // The transition observed for clockwise rotation on hardware (0x2 -> 0xB) is the positive direction.
      // This matches the existing M5Stack convention of clockwise = increase while keeping the
      // A/B pin names and the invalid-transition check: only the sign table is defined, the GPIOs are not swapped.
      const std::int16_t delta = (ENCODER_CLOCKWISE_TRANSITIONS & (1U << encoder_code)) ? 1 : -1;

      position = saturated_add(position, delta);
      push_event_locked(ENCODER_EVENT_ROTATE, delta);
    }

    void IRAM_ATTR encoder_gpio_isr(void*)
    {
      portENTER_CRITICAL_ISR(&lock);
      sample_encoder_locked();
      portEXIT_CRITICAL_ISR(&lock);
    }

    bool setup_encoder_interrupts()
    {
      // IDF masks the non-IRAM ISR while the cache is off. The explicit guard does not rely on
      // that mask; it exists to drain and re-anchor at the boundaries of flash operations.
      auto result = gpio_install_isr_service(0);
      if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;
      result = gpio_set_intr_type(board_config::PIN_ENCODER_A, GPIO_INTR_ANYEDGE);
      if (result != ESP_OK) return false;
      result = gpio_set_intr_type(board_config::PIN_ENCODER_B, GPIO_INTR_ANYEDGE);
      if (result != ESP_OK) return false;
      result = gpio_isr_handler_add(board_config::PIN_ENCODER_A, encoder_gpio_isr, nullptr);
      if (result != ESP_OK) return false;
      result = gpio_isr_handler_add(board_config::PIN_ENCODER_B, encoder_gpio_isr, nullptr);
      if (result != ESP_OK)
      {
        gpio_isr_handler_remove(board_config::PIN_ENCODER_A);
        return false;
      }
      encoder_interrupts_ready = true;
      return true;
    }

    void timer_callback(void*)
    {
      portENTER_CRITICAL(&lock);
      sample_button_locked();
      portEXIT_CRITICAL(&lock);
    }
  }

  bool setup(void)
  {
    const gpio_config_t config = {
      .pin_bit_mask = (1ULL << board_config::PIN_ENCODER_A)
                    | (1ULL << board_config::PIN_ENCODER_B)
                    | (1ULL << board_config::PIN_ENCODER_BUTTON),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) return false;

    encoder_code = static_cast<std::uint8_t>(gpio_get_level(board_config::PIN_ENCODER_A)
                   | (gpio_get_level(board_config::PIN_ENCODER_B) << 1));
    button_candidate_pressed = gpio_get_level(board_config::PIN_ENCODER_BUTTON) == BUTTON_PRESSED_LEVEL;
    const bool encoder_interrupt_init_ok = setup_encoder_interrupts();

    const esp_timer_create_args_t timer_args = {
      .callback = timer_callback,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "unit_lcd_encoder",
      .skip_unhandled_events = true,
    };
    const bool timer_init_ok = esp_timer_create(&timer_args, &timer) == ESP_OK
                            && esp_timer_start_periodic(timer, BUTTON_POLLING_INTERVAL_US) == ESP_OK;
    return encoder_interrupt_init_ok && timer_init_ok;
  }

  flash_guard_t::flash_guard_t()
  {
    suspend_for_flash_write();
  }

  flash_guard_t::~flash_guard_t()
  {
    resume_after_flash_write();
  }

  void suspend_for_flash_write(void)
  {
    if (!encoder_interrupts_ready) return;
    gpio_intr_disable(board_config::PIN_ENCODER_A);
    gpio_intr_disable(board_config::PIN_ENCODER_B);
    // Wait until an ISR that started before the disable releases the lock, so no encoder
    // handler is still running when the flash-off window begins.
    portENTER_CRITICAL(&lock);
    portEXIT_CRITICAL(&lock);
  }

  void resume_after_flash_write(void)
  {
    if (!encoder_interrupts_ready) return;
    portENTER_CRITICAL(&lock);
    // The order of transitions during the stop cannot be recovered. Joining the pre-stop state
    // with the resumed state could create an edge in the wrong direction, so restart from the current state.
    encoder_code = static_cast<std::uint8_t>(gpio_get_level(board_config::PIN_ENCODER_A)
                   | (gpio_get_level(board_config::PIN_ENCODER_B) << 1));
    portEXIT_CRITICAL(&lock);
    gpio_intr_enable(board_config::PIN_ENCODER_A);
    gpio_intr_enable(board_config::PIN_ENCODER_B);
  }

  void read_state(std::uint8_t* response)
  {
    portENTER_CRITICAL_ISR(&lock);
    response[0] = state_flags_locked();
    response[1] = queue_count;
    write_i32_be(&response[2], position);
    write_u16_be(&response[6], dropped_count);
    portEXIT_CRITICAL_ISR(&lock);
  }

  void peek_event(std::uint8_t* response)
  {
    portENTER_CRITICAL_ISR(&lock);
    event_t event = {
      .type = ENCODER_EVENT_NONE,
      .flags = state_flags_locked(),
      .sequence = UINT16_MAX,
      .delta = 0,
      .position = position,
    };
    if (queue_count != 0) event = events[queue_head];
    response[0] = event.type;
    response[1] = event.flags;
    write_u16_be(&response[2], event.sequence);
    write_i16_be(&response[4], event.delta);
    write_i32_be(&response[6], event.position);
    portEXIT_CRITICAL_ISR(&lock);
  }

  void acknowledge(std::uint16_t sequence)
  {
    portENTER_CRITICAL_ISR(&lock);
    if (queue_count != 0 && events[queue_head].sequence == sequence)
    {
      queue_head = static_cast<std::uint8_t>((queue_head + 1) % ENCODER_QUEUE_CAPACITY);
      --queue_count;
    }
    portEXIT_CRITICAL_ISR(&lock);
  }

  void set_position(std::int32_t new_position)
  {
    portENTER_CRITICAL_ISR(&lock);
    position = new_position;
    queue_head = 0;
    queue_count = 0;
    dropped_count = 0;
    local_queue_head = 0;
    local_queue_count = 0;
    local_rotation_accumulator = 0;
    local_dropped_count = 0;
    single_click_pending = false;
    click_window_samples = 0;
    portEXIT_CRITICAL_ISR(&lock);
  }

  void read_button_config(std::uint8_t* response)
  {
    portENTER_CRITICAL_ISR(&lock);
    write_u16_be(response, long_press_ms);
    portEXIT_CRITICAL_ISR(&lock);
  }

  void set_button_config(std::uint16_t new_long_press_ms)
  {
    if (new_long_press_ms < BUTTON_LONG_PRESS_MIN_MS
     || new_long_press_ms > BUTTON_LONG_PRESS_MAX_MS)
    {
      return;
    }

    portENTER_CRITICAL_ISR(&lock);
    long_press_ms = new_long_press_ms;
    portEXIT_CRITICAL_ISR(&lock);
  }

  bool poll_local_event(local_event_t* response)
  {
    if (response == nullptr) return false;

    portENTER_CRITICAL(&lock);
    if (local_queue_count == 0)
    {
      portEXIT_CRITICAL(&lock);
      return false;
    }

    *response = local_events[local_queue_head];
    local_queue_head = static_cast<std::uint8_t>(
      (local_queue_head + 1) % LOCAL_QUEUE_CAPACITY);
    --local_queue_count;
    portEXIT_CRITICAL(&lock);
    return true;
  }

  void clear_local_events(void)
  {
    portENTER_CRITICAL(&lock);
    local_queue_head = 0;
    local_queue_count = 0;
    local_rotation_accumulator = 0;
    local_dropped_count = 0;
    portEXIT_CRITICAL(&lock);
  }
}

#endif
