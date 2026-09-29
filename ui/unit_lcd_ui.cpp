#include "unit_lcd_ui.hpp"
#include "boot_logo.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace unit_lcd_ui
{
  namespace
  {
    constexpr std::int16_t DISPLAY_WIDTH = 135;
    constexpr std::int16_t DISPLAY_HEIGHT = 240;
    constexpr std::int16_t HEADER_HEIGHT = 30;
    constexpr std::int16_t ROW_HEIGHT = 34;
    constexpr std::uint32_t MOTION_TIMER_PERIOD_MS = 16;
    constexpr variant_t THEME_OPTIONS[] = { variant_t::utility, variant_t::precision };

    lv_color_t color(std::uint32_t hex)
    {
      return lv_color_hex(hex);
    }

    void set_obj_base(lv_obj_t* object)
    {
      if (object == nullptr) return;
      lv_obj_remove_style_all(object);
      lv_obj_set_style_pad_all(object, 0, 0);
      lv_obj_set_style_border_width(object, 0, 0);
      lv_obj_set_style_outline_width(object, 0, 0);
      lv_obj_set_style_shadow_width(object, 0, 0);
      lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
      lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    }

    void set_label_base(lv_obj_t* label, const lv_font_t* font, std::uint32_t text_color)
    {
      set_obj_base(label);
      lv_obj_set_style_text_font(label, font, 0);
      lv_obj_set_style_text_color(label, color(text_color), 0);
      lv_obj_set_style_text_letter_space(label, 0, 0);
      lv_obj_set_style_text_line_space(label, 1, 0);
    }

    void set_size_pos(lv_obj_t* object, std::int16_t x, std::int16_t y,
                      std::int16_t width, std::int16_t height)
    {
      lv_obj_set_pos(object, x, y);
      lv_obj_set_size(object, width, height);
    }


  }

  controller_t::~controller_t()
  {
    destroy();
  }

  const controller_t::theme_t& controller_t::theme_for(variant_t variant)
  {
    static const theme_t precision_theme = {
      0x101416, 0x1A2023, 0x252D30, 0x425054,
      0xF2F6F5, 0x91A0A1, 0x20D6C7, 0xFFB547,
      0xFFB547, 0xF26060, 0x49D17D, 0x07100F,
      3, 3, 1, 7, 121, false, false, false,
    };
    static const theme_t utility_theme = {
      0xF2F4F3, 0xFFFFFF, 0xE4EAE7, 0xC5CECA,
      0x17201E, 0x65716D, 0xF15D36, 0x138F83,
      0xC87913, 0xC93F3F, 0x238A55, 0x17201E,
      6, 6, 1, 7, 121, true, true, false,
    };
    static const theme_t console_theme = {
      0x0D100C, 0x171C14, 0x222A1D, 0x46523E,
      0xEEF5E5, 0x98A68C, 0xB7ED55, 0x52BFC2,
      0xF1B845, 0xFF6B61, 0x6DD58A, 0x11160D,
      0, 0, 1, 6, 123, false, false, true,
    };
    switch (variant)
    {
      case variant_t::utility: return utility_theme;
      case variant_t::console: return console_theme;
      default: return precision_theme;
    }
  }

  bool controller_t::create(lv_obj_t* parent, variant_t variant,
                            const device_info_t& info, const callbacks_t& callbacks)
  {
    destroy();
    if (parent == nullptr) return false;

    // Preserve persisted IDs; the retired Console palette falls back to Light.
    variant_ = variant == variant_t::precision ? variant_t::precision : variant_t::utility;
    theme_ = &theme_for(variant_);
    info_ = info;
    callbacks_ = callbacks;
    pending_address_ = info.address;
    screen_ = screen_t::home;
    selection_ = 0;
    pressed_ = false;

    init_styles();
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    set_size_pos(root_, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_add_style(root_, &styles_.root, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    // Selector motion is a retargetable spring (smooth_ui_toolkit) so rapid
    // encoder steps redirect mid-flight with velocity preserved instead of
    // restarting a fixed-duration tween per detent.
    selector_y_.springOptions().visualDuration = 0.24F;
    selector_y_.springOptions().bounce = 0.2F;
    motion_timer_ = lv_timer_create(motion_timer_callback, MOTION_TIMER_PERIOD_MS, this);

    build_screen(screen_);
    return true;
  }

  void controller_t::show_boot_logo()
  {
    if (root_ == nullptr) return;
    selection_ = 0;
    build_screen(screen_t::boot_logo);
  }

  void controller_t::destroy()
  {
    // Stop the motion pump before its LVGL targets disappear; the toolkit
    // springs themselves hold no object references.
    if (motion_timer_ != nullptr)
    {
      lv_timer_delete(motion_timer_);
      motion_timer_ = nullptr;
    }
    address_digits_[0].reset();
    address_digits_[1].reset();
    if (root_ != nullptr)
    {
      lv_anim_delete(root_, nullptr);
      lv_obj_delete(root_);
      root_ = nullptr;
    }
    content_ = nullptr;
    selector_block_ = nullptr;
    address_flow_ = nullptr;
    for (auto& character : address_chars_) character = nullptr;
    rows_[0] = rows_[1] = rows_[2] = nullptr;
    row_labels_[0] = row_labels_[1] = row_labels_[2] = nullptr;
    row_values_[0] = row_values_[1] = row_values_[2] = nullptr;
    reset_styles();
  }

  void controller_t::init_styles()
  {
    lv_style_init(&styles_.root);
    lv_style_set_bg_color(&styles_.root, color(theme_->bg));
    lv_style_set_bg_opa(&styles_.root, LV_OPA_COVER);
    lv_style_set_border_width(&styles_.root, 0);
    lv_style_set_pad_all(&styles_.root, 0);

    lv_style_init(&styles_.panel);
    lv_style_set_bg_color(&styles_.panel, color(theme_->surface));
    lv_style_set_bg_opa(&styles_.panel, LV_OPA_COVER);
    lv_style_set_border_color(&styles_.panel, color(theme_->border));
    lv_style_set_border_width(&styles_.panel, theme_->border_width);
    lv_style_set_radius(&styles_.panel, theme_->radius);
    lv_style_set_pad_all(&styles_.panel, 0);

    lv_style_init(&styles_.selector);
    lv_style_set_bg_color(&styles_.selector, color(theme_->surface_raised));
    lv_style_set_bg_opa(&styles_.selector, LV_OPA_COVER);
    lv_style_set_border_color(&styles_.selector, color(theme_->accent));
    lv_style_set_border_width(&styles_.selector, theme_->border_width);
    lv_style_set_radius(&styles_.selector, theme_->row_radius);
    lv_style_set_pad_all(&styles_.selector, 0);

    lv_style_init(&styles_.selector_pressed);
    lv_style_set_bg_color(&styles_.selector_pressed, color(theme_->accent));
    lv_style_set_bg_opa(&styles_.selector_pressed, LV_OPA_COVER);
    lv_style_set_border_color(&styles_.selector_pressed, color(theme_->accent));
    lv_style_set_border_width(&styles_.selector_pressed, theme_->border_width);
    lv_style_set_radius(&styles_.selector_pressed, theme_->row_radius);
    lv_style_set_pad_all(&styles_.selector_pressed, 0);
    styles_.initialized = true;
  }

  void controller_t::reset_styles()
  {
    if (!styles_.initialized) return;
    lv_style_reset(&styles_.selector_pressed);
    lv_style_reset(&styles_.selector);
    lv_style_reset(&styles_.panel);
    lv_style_reset(&styles_.root);
    styles_.initialized = false;
  }

  lv_obj_t* controller_t::make_label(lv_obj_t* parent, const char* text,
                                     const lv_font_t* font, std::uint32_t text_color,
                                     std::int16_t x, std::int16_t y,
                                     std::int16_t width, lv_text_align_t align)
  {
    auto* label = lv_label_create(parent);
    if (label == nullptr) return nullptr;
    set_label_base(label, font, text_color);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, align, 0);
    set_size_pos(label, x, y, width, lv_font_get_line_height(font) + 3);
    return label;
  }

  void controller_t::build_header(lv_obj_t* parent, const char* section)
  {
    const bool home = section == nullptr || section[0] == '\0';
    const bool device = !home && std::strcmp(section, "Device") == 0;
    const char* title = home ? "Unit LCD-Plus" : device ? "Device Info" : "SETTING";
    // All pages use the font selected for the longest product title.
    const lv_font_t* candidates[] = {
      &lv_font_montserrat_24, &lv_font_montserrat_18,
      &lv_font_montserrat_16, &lv_font_montserrat_14, &lv_font_montserrat_12,
    };
    const lv_font_t* font = &lv_font_montserrat_12;
    for (const auto* candidate : candidates)
    {
      lv_point_t size;
      lv_text_get_size(&size, "Unit LCD-Plus", candidate, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      if (size.x <= DISPLAY_WIDTH - 12 && size.y <= HEADER_HEIGHT - 6)
      {
        font = candidate;
        break;
      }
    }
    make_label(parent, title, font, theme_->text_primary, 6,
               (HEADER_HEIGHT - lv_font_get_line_height(font)) / 2,
               DISPLAY_WIDTH - 12, LV_TEXT_ALIGN_CENTER);
    auto* divider = lv_obj_create(parent);
    set_obj_base(divider);
    lv_obj_set_style_bg_color(divider, color(theme_->border), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    set_size_pos(divider, 0, HEADER_HEIGHT - 1, DISPLAY_WIDTH, 1);
    if (!home && !device)
    {
      make_label(parent, section, &lv_font_montserrat_14, theme_->text_secondary,
                 6, 40, DISPLAY_WIDTH - 12, LV_TEXT_ALIGN_CENTER);
    }
  }

  lv_obj_t* controller_t::make_row(lv_obj_t* parent, std::uint8_t index,
                                   std::int16_t y, const char* label, const char* value)
  {
    // Rows are transparent label anchors; the moving selector block behind
    // them carries the focus visuals, so unfocused options stay quiet text.
    auto* row = lv_obj_create(parent);
    set_obj_base(row);
    set_size_pos(row, theme_->content_x, y, theme_->content_width, ROW_HEIGHT);

    char visible_label[32];
    if (theme_->numbered_rows)
    {
      std::snprintf(visible_label, sizeof(visible_label), "%02u  %s",
                    static_cast<unsigned>(index + 1), label);
    }
    else
    {
      std::snprintf(visible_label, sizeof(visible_label), "%s", label);
    }
    const auto label_width = value == nullptr ? theme_->content_width - 18
                           : theme_->numbered_rows ? 86 : 78;
    row_labels_[index] = make_label(row, visible_label, &lv_font_montserrat_12,
                                    theme_->text_primary, 9, 9, label_width);
    if (value != nullptr)
    {
      const auto value_x = theme_->numbered_rows ? 99 : 84;
      row_values_[index] = make_label(row, value, &lv_font_montserrat_12,
                                      theme_->text_secondary, value_x, 9,
                                      theme_->content_width - value_x - 8,
                                      LV_TEXT_ALIGN_RIGHT);
    }
    rows_[index] = row;
    row_y_[index] = y;
    row_height_[index] = ROW_HEIGHT;
    return row;
  }

  void controller_t::build_home(lv_obj_t* parent)
  {
    build_header(parent, "");
    const auto focal_x = 0;
    const auto focal_w = DISPLAY_WIDTH;
    auto* focal = lv_obj_create(parent);
    set_obj_base(focal);
    lv_obj_set_style_bg_color(focal,
                              color(theme_->focal_band ? theme_->accent : 0x164A48), 0);
    lv_obj_set_style_bg_opa(focal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(focal, color(theme_->border), 0);
    lv_obj_set_style_border_width(focal, 0, 0);
    lv_obj_set_style_radius(focal, 0, 0);
    set_size_pos(focal, focal_x, 40, focal_w, 72);
    const auto focal_primary = theme_->focal_band ? theme_->focus_text : theme_->text_primary;
    const auto focal_secondary = theme_->focal_band ? 0x49332D : 0xBBDFD9;
    make_label(focal, "I2C Address", &lv_font_montserrat_12, focal_secondary,
               0, 9, focal_w, LV_TEXT_ALIGN_CENTER);
    char address[8];
    std::snprintf(address, sizeof(address), "0x%02X", info_.address);
    make_label(focal, address, &lv_font_montserrat_28, focal_primary,
               0, 29, focal_w, LV_TEXT_ALIGN_CENTER);
    row_count_ = 3;
    make_row(parent, 0, 126, "Address", LV_SYMBOL_RIGHT);
    make_row(parent, 1, 164, "Theme", LV_SYMBOL_RIGHT);
    make_row(parent, 2, 202, "Device", LV_SYMBOL_RIGHT);
  }

  void controller_t::build_address_edit(lv_obj_t* parent)
  {
    build_header(parent, "I2C Address");

    row_count_ = 2;
    auto* address_row = make_row(parent, 0, 64, "", nullptr);
    row_height_[0] = 48;
    lv_obj_set_height(address_row, row_height_[0]);
    make_row(parent, 1, 202, "Back", LV_SYMBOL_LEFT);
    // Six equal-width slots share one font and baseline: < 0 x H L >.
    const auto* font = &lv_font_montserrat_24;
    std::int16_t slot_width = 1;
    for (const char glyph : "<0x123456789ABCDEF>")
    {
      if (glyph == '\0') continue;
      lv_point_t size;
      const char text[] = {glyph, '\0'};
      lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      slot_width = std::max<std::int16_t>(slot_width, size.x);
    }
    const auto group_width = static_cast<std::int16_t>(6 * slot_width);
    const auto group_x = static_cast<std::int16_t>((DISPLAY_WIDTH - group_width) / 2);
    const auto group_y = static_cast<std::int16_t>(64 + (48 - lv_font_get_line_height(font)) / 2);
    address_flow_ = lv_obj_create(parent);
    set_obj_base(address_flow_);
    set_size_pos(address_flow_, group_x, group_y, group_width,
                 lv_font_get_line_height(font) + 3);
    char text[8];
    std::snprintf(text, sizeof(text), "<0x%02X>", pending_address_);
    for (std::uint8_t i = 0; i < 6; ++i)
    {
      const char glyph[] = {text[i], '\0'};
      address_chars_[i] = make_label(address_flow_, glyph, font, theme_->text_primary,
                                     i * slot_width, 0, slot_width, LV_TEXT_ALIGN_CENTER);
    }
#if defined(UNIT_LCD_UI_TEST_STATIC_ADDRESS)
    const bool flow_ready = false;
#else
    const bool flow_ready =
      address_digits_[0].create(address_flow_, font, theme_->text_primary, slot_width)
      && address_digits_[1].create(address_flow_, font, theme_->text_primary, slot_width);
#endif
    if (flow_ready)
    {
      for (std::uint8_t i = 0; i < 2; ++i)
      {
        lv_obj_add_flag(address_chars_[i + 3], LV_OBJ_FLAG_HIDDEN);
        address_digits_[i].set_pos((i + 3) * slot_width, 0);
        address_digits_[i].teleport(i == 0 ? pending_address_ >> 4 : pending_address_ & 0x0F);
      }
    }
    else
    {
      address_digits_[0].destroy();
      address_digits_[1].destroy();
    }
    displayed_address_ = pending_address_;

    make_label(parent, "Valid Range", &lv_font_montserrat_12,
               theme_->text_secondary, 6, 120, DISPLAY_WIDTH - 12, LV_TEXT_ALIGN_CENTER);
    make_label(parent, "0x08 ~ 0x77", &lv_font_montserrat_14,
               theme_->text_primary, 6, 135, DISPLAY_WIDTH - 12, LV_TEXT_ALIGN_CENTER);
    make_label(parent, "Current", &lv_font_montserrat_12,
               theme_->text_secondary, 6, 157, DISPLAY_WIDTH - 12, LV_TEXT_ALIGN_CENTER);
    char current[8];
    std::snprintf(current, sizeof(current), "0x%02X", info_.address);
    make_label(parent, current, &lv_font_montserrat_14, theme_->text_primary,
               6, 172, DISPLAY_WIDTH - 12, LV_TEXT_ALIGN_CENTER);

  }

  void controller_t::build_address_confirm(lv_obj_t* parent)
  {
    build_header(parent, "I2C Address");

    char old_address[8];
    char new_address[8];
    std::snprintf(old_address, sizeof(old_address), "0x%02X", info_.address);
    std::snprintf(new_address, sizeof(new_address), "0x%02X", pending_address_);
    make_label(parent, old_address, &lv_font_montserrat_18, theme_->text_secondary,
               8, 78, 48, LV_TEXT_ALIGN_CENTER);
    make_label(parent, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, theme_->accent,
               58, 80, 19, LV_TEXT_ALIGN_CENTER);
    make_label(parent, new_address, &lv_font_montserrat_18, theme_->text_primary,
               79, 78, 48, LV_TEXT_ALIGN_CENTER);

    auto* warning = make_label(parent, "Applying\nwill reboot!", &lv_font_montserrat_14,
                               theme_->danger, theme_->content_x, 110,
                               theme_->content_width, LV_TEXT_ALIGN_CENTER);
    if (warning != nullptr)
    {
      // The shared label helper defaults to a single-line height.
      lv_obj_set_height(warning, LV_SIZE_CONTENT);
      lv_obj_set_style_text_line_space(warning, 2, 0);
    }

    row_count_ = 2;
    make_row(parent, 0, 160, "Apply", LV_SYMBOL_OK);
    make_row(parent, 1, 202, "Cancel", LV_SYMBOL_CLOSE);
  }

  void controller_t::build_address_result(lv_obj_t* parent, screen_t screen)
  {
    build_header(parent, "I2C Address");
    const bool saving = screen == screen_t::address_saving;
    const bool success = screen == screen_t::address_saved;
    const auto semantic_color = saving ? theme_->accent_secondary
                              : success ? theme_->success : theme_->danger;
    const char* symbol = saving ? LV_SYMBOL_REFRESH : success ? LV_SYMBOL_OK : "!";
    const char* title = saving ? "SAVING" : success ? "SAVED" : "NOT SAVED";
    const char* detail = saving ? "WRITING SETTINGS"
                       : success ? "RESTARTING UNIT"
                                 : "NO CHANGE MADE";

    auto* mark = lv_obj_create(parent);
    set_obj_base(mark);
    lv_obj_set_style_border_color(mark, color(semantic_color), 0);
    lv_obj_set_style_border_width(mark, 2, 0);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
    set_size_pos(mark, 43, 65, 49, 49);
    make_label(mark, symbol, &lv_font_montserrat_24, semantic_color,
               0, 10, 49, LV_TEXT_ALIGN_CENTER);
    make_label(parent, title, &lv_font_montserrat_18, theme_->text_primary,
               theme_->content_x, 117, theme_->content_width, LV_TEXT_ALIGN_CENTER);
    make_label(parent, detail, &lv_font_montserrat_12, theme_->text_secondary,
               theme_->content_x, 139, theme_->content_width, LV_TEXT_ALIGN_CENTER);

    row_count_ = 0;
    if (!saving && !success)
    {
      row_count_ = 2;
      make_row(parent, 0, 160, "Try Again", LV_SYMBOL_REFRESH);
      make_row(parent, 1, 202, "Cancel", LV_SYMBOL_CLOSE);
    }
  }

  void controller_t::build_about(lv_obj_t* parent)
  {
    build_header(parent, "Device");
    const auto label = [&](const char* text, std::int16_t y)
    {
      make_label(parent, text, &lv_font_montserrat_12, theme_->text_secondary,
                 6, y, DISPLAY_WIDTH - 12, LV_TEXT_ALIGN_CENTER);
    };
    const auto value = [&](const char* text, std::int16_t y)
    {
      make_label(parent, text, &lv_font_montserrat_14, theme_->text_primary,
                 6, y, DISPLAY_WIDTH - 12, LV_TEXT_ALIGN_CENTER);
    };
    label("SoC", 37);
    value("ESP32-C61", 52);
    label("Flash", 73);
    value("4MiB", 88);
    label("Display", 117);
    value("135x240", 133);
    label("Firmware", 161);
    char firmware[12];
    std::snprintf(firmware, sizeof(firmware), "V%u.%u", info_.firmware_major, info_.firmware_minor);
    value(firmware, 177);
    for (const auto y : {109, 153})
    {
      auto* divider = lv_obj_create(parent);
      set_obj_base(divider);
      lv_obj_set_style_bg_color(divider, color(theme_->border), 0);
      lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
      set_size_pos(divider, theme_->content_x, y, theme_->content_width, 1);
    }
    row_count_ = 1;
    make_row(parent, 0, 202, "Back", LV_SYMBOL_LEFT);
  }

  void controller_t::build_theme_select(lv_obj_t* parent)
  {
    build_header(parent, "Theme");
    static const char* const names[] = { "Light", "Dark" };
    row_count_ = 3;
    for (std::uint8_t i = 0; i < 2; ++i)
    {
      auto* row = make_row(parent, i, static_cast<std::int16_t>(72 + i * 62), names[i],
                            THEME_OPTIONS[i] == variant_ ? LV_SYMBOL_OK : nullptr);
      row_height_[i] = 50;
      lv_obj_set_height(row, row_height_[i]);
      lv_obj_set_style_bg_color(row, color(theme_->surface), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(row, color(theme_->border), 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_radius(row, theme_->row_radius, 0);
      // Palette preview is fixed to the option, independent of active skin.
      const auto& preview = theme_for(THEME_OPTIONS[i]);
      auto* swatch = lv_obj_create(row);
      set_obj_base(swatch);
      set_size_pos(swatch, 8, 12, 24, 26);
      lv_obj_set_style_bg_color(swatch, color(preview.bg), 0);
      lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(swatch, color(theme_->border), 0);
      lv_obj_set_style_border_width(swatch, 1, 0);
      auto* accent = lv_obj_create(swatch);
      set_obj_base(accent);
      set_size_pos(accent, 3, 14, 16, 7);
      lv_obj_set_style_bg_color(accent, color(preview.accent), 0);
      lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);
      lv_obj_set_style_text_font(row_labels_[i], &lv_font_montserrat_14, 0);
      set_size_pos(row_labels_[i], 40, 17, 48, 20);
      if (row_values_[i] != nullptr)
        set_size_pos(row_values_[i], theme_->content_width - 23, 17, 15, 18);
    }
    make_row(parent, 2, 202, "Back", LV_SYMBOL_LEFT);
    selection_ = variant_ == variant_t::precision ? 1 : 0;
  }

  void controller_t::build_screen(screen_t screen)
  {
    if (content_ != nullptr)
    {
      lv_anim_delete(content_, nullptr);
      lv_obj_delete(content_);
    }
    content_ = lv_obj_create(root_);
    set_obj_base(content_);
    set_size_pos(content_, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    selector_block_ = nullptr;
    address_flow_ = nullptr;
    for (auto& character : address_chars_) character = nullptr;
    address_digits_[0].reset();
    address_digits_[1].reset();
    rows_[0] = rows_[1] = rows_[2] = nullptr;
    row_labels_[0] = row_labels_[1] = row_labels_[2] = nullptr;
    row_values_[0] = row_values_[1] = row_values_[2] = nullptr;
    row_count_ = 0;
    pressed_ = false;
    screen_ = screen;
    if (screen != screen_t::address_edit) address_adjusting_ = false;

    switch (screen_)
    {
      case screen_t::boot_logo:
      {
        lv_obj_set_style_bg_color(content_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(content_, LV_OPA_COVER, 0);
        char address[8];
        char firmware[12];
        std::snprintf(address, sizeof(address), "0x%02X", info_.address);
        std::snprintf(firmware, sizeof(firmware), "V%u.%u", info_.firmware_major, info_.firmware_minor);
        make_label(content_, "I2C Address", &lv_font_montserrat_12, 0x65716D,
                   7, 7, 121, LV_TEXT_ALIGN_CENTER);
        make_label(content_, address, &lv_font_montserrat_18, 0x17201E,
                   7, 26, 121, LV_TEXT_ALIGN_CENTER);
        make_label(content_, "Firmware", &lv_font_montserrat_12, 0x65716D,
                   7, 192, 121, LV_TEXT_ALIGN_CENTER);
        make_label(content_, firmware, &lv_font_montserrat_18, 0x17201E,
                   7, 211, 121, LV_TEXT_ALIGN_CENTER);
        auto* logo = lv_image_create(content_);
        if (logo != nullptr)
        {
          lv_image_set_src(logo, &boot_logo_image);
          lv_obj_set_pos(logo, (DISPLAY_WIDTH - 100) / 2, (DISPLAY_HEIGHT - 100) / 2);
        }
        boot_started_ = lv_tick_get();
        break;
      }
      case screen_t::home: build_home(content_); break;
      case screen_t::address_edit: build_address_edit(content_); break;
      case screen_t::address_confirm: build_address_confirm(content_); break;
      case screen_t::address_saving:
      case screen_t::address_saved:
      case screen_t::address_error:
        build_address_result(content_, screen_);
        break;
      case screen_t::about: build_about(content_); break;
      case screen_t::theme_select: build_theme_select(content_); break;
    }

    if (row_count_ != 0)
    {
      // The moving selector block is the focus indicator: a square-cornered
      // color block behind the focused row that springs between options.
      selector_block_ = lv_obj_create(content_);
      lv_obj_remove_style_all(selector_block_);
      lv_obj_add_style(selector_block_, &styles_.selector, 0);
      lv_obj_clear_flag(selector_block_, LV_OBJ_FLAG_SCROLLABLE);
      set_size_pos(selector_block_, theme_->content_x, row_y_[selection_],
                   theme_->content_width, row_height_[selection_]);
      selector_y_.teleport(row_y_[selection_]);
      selector_settled_ = true;
      if (screen_ == screen_t::theme_select)
        lv_obj_set_style_bg_opa(selector_block_, LV_OPA_TRANSP, 0);
      else
        lv_obj_move_to_index(selector_block_, 0);
      update_focus(false);
    }
  }

  void controller_t::update_focus(bool animate)
  {
    if (row_count_ == 0 || selector_block_ == nullptr) return;
    for (std::uint8_t i = 0; i < row_count_; ++i)
    {
      lv_obj_set_style_text_color(row_labels_[i], color(theme_->text_primary), 0);
      if (row_values_[i] != nullptr)
      {
        lv_obj_set_style_text_color(row_values_[i], color(theme_->text_secondary), 0);
      }
    }

    lv_obj_set_height(selector_block_, row_height_[selection_]);
    if (screen_ == screen_t::address_edit)
    {
      const auto text_color = address_adjusting_ ? theme_->success : theme_->text_primary;
      for (auto* character : address_chars_)
        if (character != nullptr) lv_obj_set_style_text_color(character, color(text_color), 0);
      for (auto& digit : address_digits_) digit.set_color(text_color);
      lv_obj_set_style_border_color(selector_block_,
                                    color(theme_->accent), 0);
      lv_obj_set_style_border_width(selector_block_, address_adjusting_ ? 2 : 1, 0);
      // Keep editing text legible and uniformly green while the button is held.
      lv_obj_set_style_bg_color(selector_block_, color(theme_->surface_raised), 0);
    }
    if (screen_ == screen_t::theme_select)
    {
      for (std::uint8_t i = 0; i < row_count_; ++i)
      {
        const bool active_press = pressed_ && i == selection_;
        lv_obj_set_style_bg_color(rows_[i], color(active_press ? theme_->accent : theme_->surface), 0);
        lv_obj_set_style_bg_opa(rows_[i], i < 2 || active_press ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
      }
    }
    lv_obj_remove_style(selector_block_, &styles_.selector_pressed, 0);
    if (pressed_)
    {
      lv_obj_add_style(selector_block_, &styles_.selector_pressed, 0);
      lv_obj_set_style_text_color(row_labels_[selection_], color(theme_->focus_text), 0);
      if (row_values_[selection_] != nullptr)
      {
        lv_obj_set_style_text_color(row_values_[selection_], color(theme_->focus_text), 0);
      }
    }

    const auto target_y = static_cast<float>(row_y_[selection_]);
    if (animate)
    {
      selector_y_.move(target_y);
    }
    else
    {
      selector_y_.teleport(target_y);
    }
    selector_settled_ = false;
  }

  void controller_t::update_pending_address()
  {
    // Static labels remain the identical fallback behind the rolling slots.
    char text[8];
    std::snprintf(text, sizeof(text), "<0x%02X>", pending_address_);
    for (std::uint8_t i = 3; i < 5; ++i)
    {
      const char glyph[] = {text[i], '\0'};
      if (address_chars_[i] != nullptr) lv_label_set_text(address_chars_[i], glyph);
    }
    if (!address_digits_[0].created()) return;
    const auto previous = displayed_address_;
    const auto current = pending_address_;
    displayed_address_ = current;

    // The pending address only moves by one step per dispatch; roll direction
    // follows the semantic increment/decrement like NumberFlow.
    const bool increasing = current > previous;
    const auto previous_high = static_cast<std::uint8_t>(previous >> 4);
    const auto current_high = static_cast<std::uint8_t>(current >> 4);
    if (current_high != previous_high)
    {
      address_digits_[0].roll_to(current_high, increasing);
    }
    const auto previous_low = static_cast<std::uint8_t>(previous & 0x0F);
    const auto current_low = static_cast<std::uint8_t>(current & 0x0F);
    if (current_low != previous_low)
    {
      address_digits_[1].roll_to(current_low, increasing);
    }
  }

  void controller_t::pump_motion()
  {
    if (screen_ == screen_t::boot_logo)
    {
      if (lv_tick_elaps(boot_started_) >= 1000)
      {
        selection_ = 0;
        build_screen(screen_t::home);
      }
      return;
    }
    if (selector_block_ != nullptr)
    {
      selector_y_.update();
      if (!selector_settled_ || !selector_y_.done())
      {
        lv_obj_set_y(selector_block_,
                     static_cast<std::int32_t>(selector_y_.directValue()));
        selector_settled_ = selector_y_.done();
      }
    }
    address_digits_[0].update();
    address_digits_[1].update();
  }

  void controller_t::motion_timer_callback(lv_timer_t* timer)
  {
    auto* self = static_cast<controller_t*>(lv_timer_get_user_data(timer));
    if (self != nullptr) self->pump_motion();
  }

  void controller_t::set_pressed(bool pressed)
  {
    if (row_count_ == 0 && screen_ != screen_t::address_edit) return;
    pressed_ = pressed;
    update_focus(false);
  }

  void controller_t::activate()
  {
    switch (screen_)
    {
      case screen_t::boot_logo:
        break;
      case screen_t::home:
        if (selection_ == 0)
        {
          pending_address_ = info_.address;
          selection_ = 0;
          build_screen(screen_t::address_edit);
        }
        else if (selection_ == 1)
        {
          selection_ = 0;
          build_screen(screen_t::theme_select);
        }
        else
        {
          selection_ = 0;
          build_screen(screen_t::about);
        }
        break;

      case screen_t::theme_select:
      {
        if (selection_ == 2)
        {
          selection_ = 1;
          build_screen(screen_t::home);
          break;
        }
        const auto chosen = THEME_OPTIONS[selection_];
        if (chosen != variant_)
        {
          // Re-init the style set with the new palette and rebuild the picker
          // in place: the whole screen re-themes immediately as the preview,
          // and the check mark moves to the chosen row. Styles keep their
          // storage, so objects referencing them stay valid; the root needs
          // an explicit invalidate to re-read its background.
          variant_ = chosen;
          theme_ = &theme_for(variant_);
          reset_styles();
          init_styles();
          lv_obj_invalidate(root_);
          build_screen(screen_t::theme_select);
          if (callbacks_.theme_changed != nullptr)
          {
            callbacks_.theme_changed(variant_, callbacks_.user_data);
          }
        }
        break;
      }

      case screen_t::address_edit:
        if (address_adjusting_)
        {
          selection_ = 0;
          build_screen(screen_t::address_confirm);
        }
        else if (selection_ == 0)
        {
          address_adjusting_ = true;
          update_focus(false);
        }
        else
        {
          pending_address_ = info_.address;
          selection_ = 0;
          build_screen(screen_t::home);
        }
        break;

      case screen_t::address_confirm:
        if (selection_ == 0)
        {
          build_screen(screen_t::address_saving);
          if (callbacks_.request_address_change != nullptr)
          {
            callbacks_.request_address_change(pending_address_, callbacks_.user_data);
          }
        }
        else
        {
          selection_ = 0;
          build_screen(screen_t::address_edit);
        }
        break;

      case screen_t::address_error:
        if (selection_ == 0)
        {
          selection_ = 0;
          build_screen(screen_t::address_edit);
        }
        else
        {
          selection_ = 0;
          pending_address_ = info_.address;
          build_screen(screen_t::home);
        }
        break;

      case screen_t::about:
        selection_ = 2;
        build_screen(screen_t::home);
        break;

      case screen_t::address_saving:
      case screen_t::address_saved:
        break;
    }
  }

  void controller_t::dispatch(action_t action)
  {
    if (root_ == nullptr || screen_ == screen_t::boot_logo) return;
    switch (action)
    {
      case action_t::previous:
      case action_t::next:
        if (pressed_) return;
        if (screen_ == screen_t::address_edit && address_adjusting_)
        {
          if (action == action_t::previous && pending_address_ > 0x08) --pending_address_;
          if (action == action_t::next && pending_address_ < 0x77) ++pending_address_;
          update_pending_address();
        }
        else if (row_count_ > 1)
        {
          if (action == action_t::previous)
          {
            selection_ = selection_ == 0 ? row_count_ - 1 : selection_ - 1;
          }
          else
          {
            selection_ = static_cast<std::uint8_t>((selection_ + 1) % row_count_);
          }
          update_focus(true);
        }
        break;

      case action_t::press:
        set_pressed(true);
        break;

      case action_t::release:
        if (pressed_)
        {
          set_pressed(false);
          activate();
        }
        break;

      case action_t::long_press:
        // Long holds have no navigation meaning. Disarm their release so it
        // cannot accidentally edit, save, change theme, or activate Back.
        set_pressed(false);
        break;
    }
  }

  void controller_t::set_address_save_result(bool success)
  {
    if (screen_ != screen_t::address_saving) return;
    selection_ = 0;
    build_screen(success ? screen_t::address_saved : screen_t::address_error);
  }

  void controller_t::show_preview_state(preview_state_t state)
  {
    pending_address_ = info_.address < 0x77 ? info_.address + 1 : info_.address - 1;
    selection_ = 0;
    address_adjusting_ = false;
    switch (state)
    {
      case preview_state_t::boot_logo:
        show_boot_logo();
        break;
      case preview_state_t::home:
        pending_address_ = info_.address;
        build_screen(screen_t::home);
        break;
      case preview_state_t::pressed:
        pending_address_ = info_.address;
        build_screen(screen_t::home);
        set_pressed(true);
        break;
      case preview_state_t::address_edit:
        build_screen(screen_t::address_edit);
        break;
      case preview_state_t::address_adjust:
        address_adjusting_ = true;
        build_screen(screen_t::address_edit);
        break;
      case preview_state_t::address_confirm:
        build_screen(screen_t::address_confirm);
        break;
      case preview_state_t::address_saving:
        build_screen(screen_t::address_saving);
        break;
      case preview_state_t::address_saved:
        build_screen(screen_t::address_saved);
        break;
      case preview_state_t::save_error:
        build_screen(screen_t::address_error);
        break;
      case preview_state_t::about:
        build_screen(screen_t::about);
        break;
      case preview_state_t::theme:
        build_screen(screen_t::theme_select);
        break;
    }
  }

  const char* variant_name(variant_t variant)
  {
    switch (variant)
    {
      case variant_t::precision: return "dark";
      default: return "light";
    }
  }

  bool parse_variant(const char* value, variant_t* variant)
  {
    if (value == nullptr || variant == nullptr) return false;
    if (std::strcmp(value, "dark") == 0 || std::strcmp(value, "precision") == 0 || std::strcmp(value, "a") == 0)
    {
      *variant = variant_t::precision;
      return true;
    }
    if (std::strcmp(value, "light") == 0 || std::strcmp(value, "utility") == 0 || std::strcmp(value, "b") == 0)
    {
      *variant = variant_t::utility;
      return true;
    }
    if (std::strcmp(value, "console") == 0 || std::strcmp(value, "c") == 0)
    {
      *variant = variant_t::utility;
      return true;
    }
    return false;
  }
}
