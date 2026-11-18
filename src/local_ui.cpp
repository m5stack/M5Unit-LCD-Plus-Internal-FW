#include "local_ui.hpp"

#if defined(CONFIG_IDF_TARGET_ESP32C61)

#include <cstdlib>

#include <Arduino.h>
#include <lvgl.h>

#include <core/hal/hal.hpp>

#include "command_processor.hpp"
#include "common.hpp"
#include "encoder_input.hpp"
#include "lvgl_display_port.hpp"
#include "../ui/unit_lcd_ui.hpp"
#include "unit_lcd_plus_protocol.hpp"

namespace local_ui
{
  namespace
  {
    constexpr std::uint32_t ADDRESS_RESTART_DELAY_MS = 750;
    constexpr std::size_t MAX_INPUT_EVENTS_PER_TICK = 12;

    lvgl_display_port::port_t display_port;
    unit_lcd_ui::controller_t controller;
    bool active = false;
    bool ownership_stopped = false;
    bool address_result_applied = false;
    std::uint32_t restart_due = 0;

    bool flush_allowed(void*)
    {
      return command_processor::display_owner()
          == command_processor::display_owner_t::local_ui;
    }

    bool present_tile(std::int32_t x,
                      std::int32_t y,
                      std::int32_t width,
                      std::int32_t height,
                      const std::uint8_t* pixels,
                      void*)
    {
      return command_processor::present_local_ui_tile(x, y, width, height, pixels);
    }

    void address_change_requested(std::uint8_t address, void*)
    {
      if (command_processor::request_local_address_change(address))
      {
        // A previous failed request may have already applied its result. Clear
        // that edge-triggered latch before starting a new save transaction so
        // the retry is observed by apply_address_result().
        address_result_applied = false;
        restart_due = 0;
        return;
      }

      // The controller has already entered its SAVING screen before invoking
      // this callback. A rejection (host takeover, IAP, invalid/current
      // address, or another pending request) must not leave that screen stuck.
      controller.set_address_save_result(false);
      command_processor::acknowledge_local_address_change();
    }

    unit_lcd_ui::variant_t theme_from_nvs(void)
    {
      switch (command_processor::local_ui_theme())
      {
        case 0: return unit_lcd_ui::variant_t::precision;
        // Legacy Console (2) and unknown values use the Light palette.
        default: return unit_lcd_ui::variant_t::utility;
      }
    }

    void theme_changed(unit_lcd_ui::variant_t variant, void*)
    {
      // The palette is already live; this only persists it. A failed commit
      // keeps the change for this boot, matching the address-save contract of
      // never blocking the UI on NVS.
      if (!command_processor::save_local_ui_theme(static_cast<std::uint8_t>(variant)))
      {
        ESP_LOGW("local_ui", "theme change was not persisted to NVS");
      }
    }

    void dispatch_encoder_event(const encoder_input::local_event_t& event)
    {
      switch (event.type)
      {
        case unit_lcd_plus_protocol::ENCODER_EVENT_ROTATE:
          if (event.delta > 0)
          {
            for (std::int16_t i = 0; i < event.delta; ++i)
            {
              controller.dispatch(unit_lcd_ui::action_t::next);
            }
          }
          else
          {
            for (std::int16_t i = 0; i > event.delta; --i)
            {
              controller.dispatch(unit_lcd_ui::action_t::previous);
            }
          }
          break;

        case unit_lcd_plus_protocol::ENCODER_EVENT_PRESS:
          controller.dispatch(unit_lcd_ui::action_t::press);
          break;

        case unit_lcd_plus_protocol::ENCODER_EVENT_RELEASE:
          controller.dispatch(unit_lcd_ui::action_t::release);
          break;

        case unit_lcd_plus_protocol::ENCODER_EVENT_LONG_PRESS:
          // Long holds are ignored by the UI and disarm the release click.
          controller.dispatch(unit_lcd_ui::action_t::long_press);
          break;

        default:
          // SINGLE_CLICK and DOUBLE_CLICK are informational host protocol
          // events. PRESS/RELEASE already provide the UI activation semantics
          // and must not trigger a second action here.
          break;
      }
    }

    void apply_address_result(void)
    {
      const auto status = command_processor::local_address_change_status();
      if (status == command_processor::local_address_status_t::pending
       || address_result_applied)
      {
        return;
      }

      if (status == command_processor::local_address_status_t::saved)
      {
        controller.set_address_save_result(true);
        address_result_applied = true;
        restart_due = millis() + ADDRESS_RESTART_DELAY_MS;
      }
      else if (status == command_processor::local_address_status_t::failed)
      {
        controller.set_address_save_result(false);
        address_result_applied = true;
        command_processor::acknowledge_local_address_change();
      }
    }

    void stop_for_owner_transition(void)
    {
      if (ownership_stopped) return;
      // The command processor fences its legacy canvas before host writes. This
      // second fence covers a just-finished LVGL tile and makes the hand-off
      // explicit even if the caller changes loop ordering later.
      display_port.wait_for_flush();
      display_port.set_flush_enabled(false);
      encoder_input::clear_local_events();
      ownership_stopped = true;
    }
  }

  bool init(LGFX_Device& lcd,
            std::uint8_t address,
            std::uint8_t firmware_major,
            std::uint8_t firmware_minor)
  {
    if (active) return true;

    // smooth_ui_toolkit's default HAL tick is std::chrono; pin it to the
    // Arduino millisecond clock once so spring timing matches LVGL's tick.
    smooth_ui_toolkit::ui_hal::on_get_tick(
      [] { return static_cast<std::uint32_t>(millis()); });

    if (!display_port.init(lcd,
                           flush_allowed,
                           nullptr,
                           nullptr,
                           false,
                           present_tile))
    {
      return false;
    }

    unit_lcd_ui::device_info_t info;
    info.address = address;
    info.firmware_major = firmware_major;
    info.firmware_minor = firmware_minor;

    unit_lcd_ui::callbacks_t callbacks;
    callbacks.request_address_change = address_change_requested;
    callbacks.theme_changed = theme_changed;
    callbacks.user_data = nullptr;

    auto* screen = lv_screen_active();
    if (screen == nullptr || !controller.create(screen,
                                                 theme_from_nvs(),
                                                 info,
                                                 callbacks))
    {
      display_port.deinit();
      return false;
    }

    controller.show_boot_logo();
    active = true;
    ownership_stopped = false;
    address_result_applied = false;
    restart_due = 0;
    return true;
  }

  void handle(void)
  {
    if (!active) return;

    if (command_processor::display_owner()
        != command_processor::display_owner_t::local_ui)
    {
      stop_for_owner_transition();
      return;
    }

    apply_address_result();

    encoder_input::local_event_t event;
    std::size_t handled = 0;
    while (handled < MAX_INPUT_EVENTS_PER_TICK
        && encoder_input::poll_local_event(&event))
    {
      dispatch_encoder_event(event);
      ++handled;
    }

    // LVGL is called only here, from the Arduino loop task. The presenter in
    // lvgl_display_port synchronously merges each RGB565 tile into the shared
    // RGB888 canvas, after which command_processor flushes that canvas once.
    display_port.handle();

    if (restart_due != 0
     && static_cast<std::int32_t>(millis() - restart_due) >= 0
     && command_processor::local_address_change_status()
          == command_processor::local_address_status_t::saved)
    {
      display_port.wait_for_flush();
      command_processor::restart_after_local_address_change();
    }
  }

  void deinit(void)
  {
    if (!active)
    {
      display_port.deinit();
      return;
    }

    display_port.wait_for_flush();
    controller.destroy();
    display_port.deinit();
    encoder_input::clear_local_events();
    active = false;
    ownership_stopped = false;
    address_result_applied = false;
    restart_due = 0;
  }

  bool initialized(void)
  {
    return active;
  }
}

#else

namespace local_ui
{
  bool init(LGFX_Device&, std::uint8_t, std::uint8_t, bool) { return false; }
  void handle(void) {}
  void deinit(void) {}
  bool initialized(void) { return false; }
}

#endif
