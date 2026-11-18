#pragma once

#include <cstdint>

#include <lvgl.h>

#include "hex_digit_flow.hpp"

namespace unit_lcd_ui
{
  enum class variant_t : std::uint8_t
  {
    precision,  // Persisted ID 0: Dark.
    utility,    // Persisted ID 1: Light.
    console,    // Legacy ID 2: normalized to Light when loaded.
  };

  enum class action_t : std::uint8_t
  {
    previous,
    next,
    press,
    release,
    long_press,
  };

    enum class preview_state_t : std::uint8_t
    {
      boot_logo,
      home,
      pressed,
      address_edit,
      address_adjust,
      address_confirm,
      address_saving,
      address_saved,
      save_error,
      about,
      theme,
  };

  struct device_info_t
  {
    std::uint8_t address = 0x3E;
    std::uint8_t firmware_major = 1;
    std::uint8_t firmware_minor = 1;
  };

  struct callbacks_t
  {
    void (*request_address_change)(std::uint8_t address, void* user_data) = nullptr;
    // Fired after a theme is applied live in the theme picker. The firmware
    // persists it to NVS; the simulator only counts invocations.
    void (*theme_changed)(variant_t variant, void* user_data) = nullptr;
    void* user_data = nullptr;
  };

  class controller_t
  {
  public:
    controller_t() = default;
    ~controller_t();

    controller_t(const controller_t&) = delete;
    controller_t& operator=(const controller_t&) = delete;

    bool create(lv_obj_t* parent, variant_t variant,
                const device_info_t& info, const callbacks_t& callbacks = {});
    void destroy();
    void show_boot_logo();
    void dispatch(action_t action);
    void set_address_save_result(bool success);
    void show_preview_state(preview_state_t state);

    std::uint8_t pending_address() const { return pending_address_; }

  private:
    enum class screen_t : std::uint8_t
    {
      boot_logo,
      home,
      address_edit,
      address_confirm,
      address_saving,
      address_saved,
      address_error,
      about,
      theme_select,
    };

    struct theme_t
    {
      std::uint32_t bg;
      std::uint32_t surface;
      std::uint32_t surface_raised;
      std::uint32_t border;
      std::uint32_t text_primary;
      std::uint32_t text_secondary;
      std::uint32_t accent;
      std::uint32_t accent_secondary;
      std::uint32_t warning;
      std::uint32_t danger;
      std::uint32_t success;
      std::uint32_t focus_text;
      std::uint8_t radius;
      std::uint8_t row_radius;
      std::uint8_t border_width;
      std::int16_t content_x;
      std::int16_t content_width;
      bool focal_band;
      bool focal_centered;
      bool numbered_rows;
    };

    struct styles_t
    {
      lv_style_t root;
      lv_style_t panel;
      lv_style_t selector;
      lv_style_t selector_pressed;
      bool initialized = false;
    };

    static const theme_t& theme_for(variant_t variant);
    void init_styles();
    void reset_styles();
    void build_screen(screen_t screen);
    void build_header(lv_obj_t* parent, const char* section);
    void build_home(lv_obj_t* parent);
    void build_address_edit(lv_obj_t* parent);
    void build_address_confirm(lv_obj_t* parent);
    void build_address_result(lv_obj_t* parent, screen_t screen);
    void build_about(lv_obj_t* parent);
    void build_theme_select(lv_obj_t* parent);
    void activate();
    void update_focus(bool animate);
    void update_pending_address();
    void set_pressed(bool pressed);

    lv_obj_t* make_label(lv_obj_t* parent, const char* text,
                         const lv_font_t* font, std::uint32_t color,
                         std::int16_t x, std::int16_t y,
                         std::int16_t width, lv_text_align_t align = LV_TEXT_ALIGN_LEFT);
    lv_obj_t* make_row(lv_obj_t* parent, std::uint8_t index,
                       std::int16_t y, const char* label, const char* value);
    void pump_motion();
    static void motion_timer_callback(lv_timer_t* timer);

    variant_t variant_ = variant_t::precision;
    const theme_t* theme_ = nullptr;
    device_info_t info_ = {};
    callbacks_t callbacks_ = {};
    styles_t styles_ = {};
    screen_t screen_ = screen_t::home;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* selector_block_ = nullptr;
    lv_obj_t* address_flow_ = nullptr;
    lv_obj_t* address_chars_[6] = {};
    lv_timer_t* motion_timer_ = nullptr;
    hex_digit_flow_t address_digits_[2];
    smooth_ui_toolkit::AnimateValue selector_y_;
    lv_obj_t* rows_[3] = {};
    lv_obj_t* row_labels_[3] = {};
    lv_obj_t* row_values_[3] = {};
    std::int16_t row_y_[3] = {};
    std::int16_t row_height_[3] = {};
    bool address_adjusting_ = false;
    std::uint32_t boot_started_ = 0;
    std::uint8_t row_count_ = 0;
    std::uint8_t selection_ = 0;
    std::uint8_t pending_address_ = 0x3E;
    std::uint8_t displayed_address_ = 0x3E;
    bool selector_settled_ = true;
    bool pressed_ = false;
  };

  const char* variant_name(variant_t variant);
  bool parse_variant(const char* value, variant_t* variant);
}
