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

    constexpr std::uint32_t POLLING_INTERVAL_US = 1000;
    constexpr std::uint16_t BUTTON_DEBOUNCE_SAMPLES = 10;
    constexpr std::uint8_t BUTTON_PRESSED_LEVEL = 0;

    struct event_t
    {
      std::uint8_t type;
      std::uint8_t flags;
      std::uint16_t sequence;
      std::int16_t delta;
      std::int32_t position;
    };

    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    esp_timer_handle_t timer = nullptr;
    event_t events[ENCODER_QUEUE_CAPACITY] = {};
    std::uint8_t queue_head = 0;
    std::uint8_t queue_count = 0;
    std::uint16_t next_sequence = 0;
    std::uint16_t dropped_count = 0;
    std::int32_t position = 0;
    std::uint8_t encoder_code = 0;
    std::uint16_t encoder_store = 0;
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

    std::uint8_t IRAM_ATTR state_flags_locked()
    {
      std::uint8_t flags = 0;
      if (button_pressed) flags |= ENCODER_FLAG_BUTTON_PRESSED;
      if (button_long_pressed) flags |= ENCODER_FLAG_BUTTON_LONG_PRESSED;
      if (queue_count != 0) flags |= ENCODER_FLAG_EVENT_PENDING;
      if (dropped_count != 0) flags |= ENCODER_FLAG_OVERFLOW;
      return flags;
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
      static constexpr std::uint8_t valid_states[16] = {
        0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0
      };

      encoder_code = static_cast<std::uint8_t>((encoder_code << 2)
                    | gpio_get_level(board_config::PIN_ENCODER_A)
                    | (gpio_get_level(board_config::PIN_ENCODER_B) << 1));
      encoder_code &= 0x0F;
      if (!valid_states[encoder_code]) return;

      encoder_store = static_cast<std::uint16_t>((encoder_store << 4) | encoder_code);
      std::int16_t delta = 0;
      if (encoder_store == 0xE817 || encoder_store == 0x17E8) delta = 1;
      if (encoder_store == 0xD42B || encoder_store == 0x2BD4) delta = -1;
      if (delta == 0) return;

      encoder_store = 0;
      position = saturated_add(position, delta);
      push_event_locked(ENCODER_EVENT_ROTATE, delta);
    }

    void timer_callback(void*)
    {
      portENTER_CRITICAL(&lock);
      sample_button_locked();
      sample_encoder_locked();
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

    const esp_timer_create_args_t timer_args = {
      .callback = timer_callback,
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "unit_lcd_encoder",
      .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &timer) == ESP_OK
        && esp_timer_start_periodic(timer, POLLING_INTERVAL_US) == ESP_OK;
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
}

#endif
