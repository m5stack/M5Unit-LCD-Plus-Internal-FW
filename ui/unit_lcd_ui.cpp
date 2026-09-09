#include "unit_lcd_ui.hpp"

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

    void anim_set_y(void* object, std::int32_t value)
    {
      lv_obj_set_y(static_cast<lv_obj_t*>(object), value);
    }

    void start_animation(lv_obj_t* object, lv_anim_exec_xcb_t callback,
                         std::int32_t from, std::int32_t to,
                         std::uint32_t duration)
    {
      lv_anim_delete(object, callback);
      lv_anim_t animation;
      lv_anim_init(&animation);
      lv_anim_set_var(&animation, object);
      lv_anim_set_exec_cb(&animation, callback);
      lv_anim_set_values(&animation, from, to);
      lv_anim_set_duration(&animation, duration);
      lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
      lv_anim_start(&animation);
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
      3, 3, 1, 8, 119, false, false, false,
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

    variant_ = variant;
    theme_ = &theme_for(variant);
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

    build_screen(screen_, false);
    return true;
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
    address_label_ = nullptr;
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
    // Keep the product identity as the only persistent header status.  The old
    // dot/"LOCAL" pair did not describe a state that users could act on and
    // competed with the address, which is the primary boot-time task.
    make_label(parent, "Unit LCD-PLUS", &lv_font_montserrat_10, theme_->text_primary,
               6, 8, DISPLAY_WIDTH - 12);

    auto* divider = lv_obj_create(parent);
    set_obj_base(divider);
    lv_obj_set_style_bg_color(divider, color(theme_->border), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    set_size_pos(divider, 0, HEADER_HEIGHT - 1, DISPLAY_WIDTH, 1);

    if (section != nullptr && section[0] != '\0')
    {
      make_label(parent, section, &lv_font_montserrat_12, theme_->text_secondary,
                 theme_->content_x, 36, theme_->content_width);
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
    return row;
  }

  void controller_t::build_home(lv_obj_t* parent)
  {
    build_header(parent, "");

    // Three option rows share the lower half, so the focal band shrinks to
    // keep a steady 4 px gap rhythm; the footer line is dropped because the
    // DEVICE row already carries the firmware version.
    const auto focal_x = theme_->focal_band ? 0 : theme_->content_x;
    const auto focal_w = theme_->focal_band ? DISPLAY_WIDTH : theme_->content_width;
    auto* focal = lv_obj_create(parent);
    set_obj_base(focal);
    lv_obj_set_style_bg_color(focal,
                              color(theme_->focal_band ? theme_->accent : theme_->surface), 0);
    lv_obj_set_style_bg_opa(focal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(focal, color(theme_->border), 0);
    lv_obj_set_style_border_width(focal, theme_->focal_band ? 0 : theme_->border_width, 0);
    lv_obj_set_style_radius(focal, theme_->focal_band ? 0 : theme_->radius, 0);
    set_size_pos(focal, focal_x, 37, focal_w, 82);

    const auto focal_primary = theme_->focal_band ? theme_->focus_text : theme_->text_primary;
    const auto focal_secondary = theme_->focal_band ? 0x49332D : theme_->text_secondary;
    make_label(focal, "I2C ADDRESS", &lv_font_montserrat_12, focal_secondary,
               theme_->focal_centered ? 0 : 9, 8,
               theme_->focal_centered ? focal_w : focal_w - 18,
               theme_->focal_centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);

    char address[8];
    std::snprintf(address, sizeof(address), "0x%02X", info_.address);
    make_label(focal, address, &lv_font_montserrat_28, focal_primary,
               theme_->focal_centered ? 0 : 8, 25,
               theme_->focal_centered ? focal_w : focal_w - 16,
               theme_->focal_centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);

    const char* status = theme_->numbered_rows ? "READY / WAIT"
                       : theme_->focal_band ? "WAITING FOR HOST"
                                             : "AWAITING HOST";
    make_label(focal, status, &lv_font_montserrat_12, focal_secondary,
               theme_->focal_centered ? 0 : 9, 60,
               theme_->focal_centered ? focal_w : focal_w - 18,
               theme_->focal_centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);

    row_count_ = 3;
    make_row(parent, 0, 126, "ADDRESS", LV_SYMBOL_RIGHT);
    char firmware[12];
    std::snprintf(firmware, sizeof(firmware), "V%u", info_.firmware_version);
    if (theme_->numbered_rows)
    {
      char device[24];
      std::snprintf(device, sizeof(device), "DEVICE / V%u", info_.firmware_version);
      make_row(parent, 1, 164, device, nullptr);
    }
    else
    {
      make_row(parent, 1, 164, "DEVICE", firmware);
    }
    make_row(parent, 2, 202, "THEME", LV_SYMBOL_RIGHT);
  }

  void controller_t::build_address_edit(lv_obj_t* parent)
  {
    build_header(parent, "EDIT ADDRESS");
    make_label(parent, "I2C ADDRESS", &lv_font_montserrat_12, theme_->text_secondary,
               theme_->content_x, 65, theme_->content_width, LV_TEXT_ALIGN_CENTER);

    // "0x" prefix + two rolling hex columns (NumberFlow-style digits). The
    // group is centered as one unit so column widths never shift the layout.
    // If the LVGL heap cannot hold the rolling columns, fall back to a plain
    // label so the address remains editable.
    address_flow_ = lv_obj_create(parent);
    set_obj_base(address_flow_);
    const auto prefix_width = static_cast<std::int16_t>(
      lv_font_get_glyph_width(&lv_font_montserrat_28, '0', 'x')
      + lv_font_get_glyph_width(&lv_font_montserrat_28, 'x', 0));
    const bool flow_ready =
      address_digits_[0].create(address_flow_, &lv_font_montserrat_28, theme_->text_primary)
      && address_digits_[1].create(address_flow_, &lv_font_montserrat_28, theme_->text_primary);
    if (flow_ready)
    {
      const auto column_width = static_cast<std::int16_t>(address_digits_[0].width());
      const auto digit_height = static_cast<std::int16_t>(address_digits_[0].height());
      const auto group_width = static_cast<std::int16_t>(prefix_width + 2 * column_width);
      const auto group_x = static_cast<std::int16_t>((DISPLAY_WIDTH - group_width) / 2);
      set_size_pos(address_flow_, 0, 86, DISPLAY_WIDTH, digit_height);
      make_label(address_flow_, "0x", &lv_font_montserrat_28, theme_->text_secondary,
                 group_x, 0, prefix_width);
      address_digits_[0].set_pos(group_x + prefix_width, 0);
      address_digits_[1].set_pos(group_x + prefix_width + column_width, 0);
      address_digits_[0].teleport(pending_address_ >> 4);
      address_digits_[1].teleport(pending_address_ & 0x0F);
    }
    else
    {
      address_digits_[0].reset();
      address_digits_[1].reset();
      char address[8];
      std::snprintf(address, sizeof(address), "0x%02X", pending_address_);
      address_label_ = make_label(address_flow_, address, &lv_font_montserrat_28,
                                  theme_->text_primary, 0, 0,
                                  DISPLAY_WIDTH, LV_TEXT_ALIGN_CENTER);
      set_size_pos(address_flow_, 0, 86, DISPLAY_WIDTH,
                   lv_font_get_line_height(&lv_font_montserrat_28));
    }
    displayed_address_ = pending_address_;

    make_label(parent, LV_SYMBOL_LEFT, &lv_font_montserrat_18, theme_->accent,
               theme_->content_x + 4, 91, 18, LV_TEXT_ALIGN_CENTER);
    make_label(parent, LV_SYMBOL_RIGHT, &lv_font_montserrat_18, theme_->accent,
               theme_->content_x + theme_->content_width - 22, 91, 18, LV_TEXT_ALIGN_CENTER);
    make_label(parent, "VALID 0x08 - 0x77", &lv_font_montserrat_12,
               theme_->text_secondary, theme_->content_x, 126,
               theme_->content_width, LV_TEXT_ALIGN_CENTER);

    row_count_ = 1;
    make_row(parent, 0, 174, "CONTINUE", LV_SYMBOL_RIGHT);

    char current[20];
    std::snprintf(current, sizeof(current), "CURRENT  0x%02X", info_.address);
    make_label(parent, current, &lv_font_montserrat_12, theme_->text_secondary,
               theme_->content_x, 222, theme_->content_width, LV_TEXT_ALIGN_CENTER);
  }

  void controller_t::build_address_confirm(lv_obj_t* parent)
  {
    build_header(parent, "CONFIRM");

    char old_address[8];
    char new_address[8];
    std::snprintf(old_address, sizeof(old_address), "0x%02X", info_.address);
    std::snprintf(new_address, sizeof(new_address), "0x%02X", pending_address_);
    make_label(parent, old_address, &lv_font_montserrat_18, theme_->text_secondary,
               6, 68, 48, LV_TEXT_ALIGN_RIGHT);
    make_label(parent, LV_SYMBOL_RIGHT, &lv_font_montserrat_14, theme_->accent,
               57, 71, 21, LV_TEXT_ALIGN_CENTER);
    make_label(parent, new_address, &lv_font_montserrat_18, theme_->text_primary,
               81, 68, 48);

    auto* warning = lv_obj_create(parent);
    lv_obj_remove_style_all(warning);
    lv_obj_add_style(warning, &styles_.panel, 0);
    lv_obj_set_style_border_color(warning, color(theme_->warning), 0);
    set_size_pos(warning, theme_->content_x, 105, theme_->content_width, 38);
    make_label(warning, "!", &lv_font_montserrat_18, theme_->warning, 9, 8, 14,
               LV_TEXT_ALIGN_CENTER);
    make_label(warning, "WILL REBOOT", &lv_font_montserrat_12,
               theme_->text_primary, 26, 11, theme_->content_width - 31);

    row_count_ = 2;
    make_row(parent, 0, 157, "APPLY", LV_SYMBOL_OK);
    make_row(parent, 1, 198, "CANCEL", LV_SYMBOL_CLOSE);
  }

  void controller_t::build_address_result(lv_obj_t* parent, screen_t screen)
  {
    build_header(parent, "ADDRESS UPDATE");
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
    set_size_pos(mark, 43, 56, 49, 49);
    make_label(mark, symbol, &lv_font_montserrat_24, semantic_color,
               0, 10, 49, LV_TEXT_ALIGN_CENTER);
    make_label(parent, title, &lv_font_montserrat_18, theme_->text_primary,
               theme_->content_x, 119, theme_->content_width, LV_TEXT_ALIGN_CENTER);
    make_label(parent, detail, &lv_font_montserrat_12, theme_->text_secondary,
               theme_->content_x, 146, theme_->content_width, LV_TEXT_ALIGN_CENTER);

    row_count_ = 0;
    if (!saving && !success)
    {
      row_count_ = 2;
      make_row(parent, 0, 169, "TRY AGAIN", LV_SYMBOL_REFRESH);
      make_row(parent, 1, 205, "CANCEL", LV_SYMBOL_CLOSE);
    }
  }

  void controller_t::build_about(lv_obj_t* parent)
  {
    build_header(parent, "DEVICE INFO");

    struct info_row_t
    {
      const char* label;
      const char* value;
    };
    char firmware[16];
    std::snprintf(firmware, sizeof(firmware), "%u %s", info_.firmware_version,
                  info_.debug_build ? "DEBUG" : "RELEASE");
    const info_row_t values[] = {
      { "MODEL", "Unit LCD-PLUS" },
      { "SOC", "ESP32-C61" },
      { "FIRMWARE", firmware },
      { "DISPLAY", "135 x 240" },
    };

    for (std::size_t i = 0; i < 4; ++i)
    {
      const auto y = static_cast<std::int16_t>(56 + i * 34);
      make_label(parent, values[i].label, &lv_font_montserrat_12,
                 theme_->text_secondary, theme_->content_x, y,
                 theme_->content_width);
      make_label(parent, values[i].value, &lv_font_montserrat_14,
                 theme_->text_primary, theme_->content_x, y + 14,
                 theme_->content_width);
      if (i != 3)
      {
        auto* divider = lv_obj_create(parent);
        set_obj_base(divider);
        lv_obj_set_style_bg_color(divider, color(theme_->border), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
        set_size_pos(divider, theme_->content_x, y + 30, theme_->content_width, 1);
      }
    }
    row_count_ = 1;
    make_row(parent, 0, 202, "BACK", LV_SYMBOL_LEFT);
  }

  void controller_t::build_theme_select(lv_obj_t* parent)
  {
    build_header(parent, "THEME");

    static const char* const names[] = { "PRECISION", "UTILITY", "CONSOLE" };
    // Selecting a row applies that palette to the whole UI immediately, so
    // the picker itself is the preview; the active theme carries the check.
    row_count_ = 3;
    for (std::uint8_t i = 0; i < 3; ++i)
    {
      make_row(parent, i, static_cast<std::int16_t>(64 + i * 42), names[i],
               static_cast<variant_t>(i) == variant_ ? LV_SYMBOL_OK : nullptr);
    }
    selection_ = static_cast<std::uint8_t>(variant_);
  }

  void controller_t::build_screen(screen_t screen, bool animate)
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
    address_label_ = nullptr;
    address_digits_[0].reset();
    address_digits_[1].reset();
    rows_[0] = rows_[1] = rows_[2] = nullptr;
    row_labels_[0] = row_labels_[1] = row_labels_[2] = nullptr;
    row_values_[0] = row_values_[1] = row_values_[2] = nullptr;
    row_count_ = 0;
    pressed_ = false;
    screen_ = screen;

    switch (screen_)
    {
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
                   theme_->content_width, ROW_HEIGHT);
      selector_y_.teleport(row_y_[selection_]);
      selector_settled_ = true;
      lv_obj_move_to_index(selector_block_, 0);
      update_focus(false);
    }
    if (animate) start_screen_motion();
  }

  void controller_t::start_screen_motion()
  {
    if (content_ == nullptr) return;
    /* A plain object translation keeps the transition out of LVGL's layer allocator. */
    lv_obj_set_y(content_, 5);
    start_animation(content_, anim_set_y, 5, 0, 160);
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
    if (address_label_ != nullptr)
    {
      // Static fallback when the rolling columns could not be allocated.
      char address[8];
      std::snprintf(address, sizeof(address), "0x%02X", pending_address_);
      lv_label_set_text(address_label_, address);
      displayed_address_ = pending_address_;
      return;
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
    if (row_count_ == 0) return;
    pressed_ = pressed;
    update_focus(false);
  }

  void controller_t::activate()
  {
    switch (screen_)
    {
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
          build_screen(screen_t::about);
        }
        else
        {
          selection_ = 0;
          build_screen(screen_t::theme_select);
        }
        break;

      case screen_t::theme_select:
      {
        const auto chosen = static_cast<variant_t>(selection_);
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
        selection_ = 0;
        build_screen(screen_t::address_confirm);
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
          pending_address_ = info_.address;
          build_screen(screen_t::home);
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
        selection_ = 1;
        build_screen(screen_t::home);
        break;

      case screen_t::address_saving:
      case screen_t::address_saved:
        break;
    }
  }

  void controller_t::dispatch(action_t action)
  {
    if (root_ == nullptr) return;
    switch (action)
    {
      case action_t::previous:
      case action_t::next:
        if (pressed_) return;
        if (screen_ == screen_t::address_edit)
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

      case action_t::back:
        pressed_ = false;
        switch (screen_)
        {
          case screen_t::address_edit:
          case screen_t::about:
          case screen_t::address_error:
            selection_ = 0;
            pending_address_ = info_.address;
            build_screen(screen_t::home);
            break;
          case screen_t::theme_select:
            selection_ = 2;
            build_screen(screen_t::home);
            break;
          case screen_t::address_confirm:
            selection_ = 0;
            build_screen(screen_t::address_edit);
            break;
          default:
            break;
        }
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
    switch (state)
    {
      case preview_state_t::home:
        pending_address_ = info_.address;
        build_screen(screen_t::home, false);
        break;
      case preview_state_t::pressed:
        pending_address_ = info_.address;
        build_screen(screen_t::home, false);
        set_pressed(true);
        break;
      case preview_state_t::address_edit:
        build_screen(screen_t::address_edit, false);
        break;
      case preview_state_t::address_confirm:
        build_screen(screen_t::address_confirm, false);
        break;
      case preview_state_t::address_saving:
        build_screen(screen_t::address_saving, false);
        break;
      case preview_state_t::address_saved:
        build_screen(screen_t::address_saved, false);
        break;
      case preview_state_t::save_error:
        build_screen(screen_t::address_error, false);
        break;
      case preview_state_t::about:
        build_screen(screen_t::about, false);
        break;
      case preview_state_t::theme:
        build_screen(screen_t::theme_select, false);
        break;
    }
  }

  const char* variant_name(variant_t variant)
  {
    switch (variant)
    {
      case variant_t::utility: return "utility";
      case variant_t::console: return "console";
      default: return "precision";
    }
  }

  bool parse_variant(const char* value, variant_t* variant)
  {
    if (value == nullptr || variant == nullptr) return false;
    if (std::strcmp(value, "precision") == 0 || std::strcmp(value, "a") == 0)
    {
      *variant = variant_t::precision;
      return true;
    }
    if (std::strcmp(value, "utility") == 0 || std::strcmp(value, "b") == 0)
    {
      *variant = variant_t::utility;
      return true;
    }
    if (std::strcmp(value, "console") == 0 || std::strcmp(value, "c") == 0)
    {
      *variant = variant_t::console;
      return true;
    }
    return false;
  }
}
