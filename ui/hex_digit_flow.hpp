#pragma once

#include <cstdint>

#include <lvgl.h>

#include <core/animation/animate_value/animate_value.hpp>

namespace unit_lcd_ui
{
  // NumberFlow-style rolling column for a single hex digit. Upstream
  // smooth_ui_toolkit::lvgl_cpp::DigitFlow only covers the decimal charset and
  // the address readout needs 0-F, so this project-owned column re-implements
  // the same padded wraparound-list technique on top of the toolkit's
  // AnimateValue spring. The digit list is one multi-line label inside a
  // clipping column: only the line at the current offset is visible, and the
  // rolling cost is a single object move per frame.
  class hex_digit_flow_t
  {
  public:
    // Padded list layout (same trick as DigitFlow): the guard copies at both
    // ends turn the F<->0 wrap into a one-step roll instead of a long rewind.
    // Line 0 and 17 render 'F'/'0'; lines 1..16 render '0'..'F'.
    static constexpr std::uint8_t LIST_SIZE = 18;

    bool create(lv_obj_t* parent, const lv_font_t* font, std::uint32_t text_color,
                std::int32_t slot_width = 0)
    {
      if (parent == nullptr || font == nullptr) return false;
      font_ = font;
      font_height_ = lv_font_get_line_height(font_);
      column_width_ = 1;
      for (std::uint8_t i = 0; i < 16; ++i)
      {
        const auto advance = lv_font_get_glyph_width(font_, DIGITS[i], DIGITS[i]);
        if (advance > column_width_) column_width_ = advance;
      }

      if (slot_width > column_width_) column_width_ = slot_width;

      column_ = lv_obj_create(parent);
      if (column_ == nullptr) return false;
      lv_obj_remove_style_all(column_);
      lv_obj_set_style_bg_opa(column_, LV_OPA_TRANSP, 0);
      lv_obj_clear_flag(column_, LV_OBJ_FLAG_SCROLLABLE);
      // The rolling label deliberately overflows the column's clip box; the
      // simulator's layout validator skips USER_1 subtrees for that reason.
      lv_obj_add_flag(column_, LV_OBJ_FLAG_USER_1);
      lv_obj_set_size(column_, column_width_, font_height_);

      label_ = lv_label_create(column_);
      if (label_ == nullptr)
      {
        lv_obj_delete(column_);
        column_ = nullptr;
        return false;
      }
      for (std::uint8_t i = 0; i < LIST_SIZE; ++i)
      {
        text_[i * 2] = digit_at(i);
        text_[i * 2 + 1] = i + 1 == LIST_SIZE ? '\0' : '\n';
      }
      lv_obj_remove_style_all(label_);
      lv_obj_set_style_text_font(label_, font_, 0);
      lv_obj_set_style_text_color(label_, lv_color_hex(text_color), 0);
      lv_obj_set_style_text_align(label_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_line_space(label_, 0, 0);
      lv_label_set_text_static(label_, text_);
      lv_obj_set_size(label_, column_width_, LIST_SIZE * font_height_);
      lv_obj_set_pos(label_, 0, 0);

      y_offset_.springOptions().visualDuration = 0.3F;
      y_offset_.springOptions().bounce = 0.12F;
      teleport(0);
      return true;
    }

    // Screen teardown deletes the LVGL objects with their parent; only the
    // cached pointers need clearing. The spring carries no LVGL reference.
    void reset()
    {
      column_ = nullptr;
      label_ = nullptr;
      font_ = nullptr;
    }

    void set_color(std::uint32_t text_color)
    {
      if (label_ != nullptr) lv_obj_set_style_text_color(label_, lv_color_hex(text_color), 0);
    }

    // Used only when a partially created pair must fall back to static slots.
    void destroy()
    {
      if (column_ != nullptr) lv_obj_delete(column_);
      reset();
    }

    bool created() const { return column_ != nullptr; }

    // Instant jump for screen entry and preview captures.
    void teleport(std::uint8_t nibble)
    {
      index_ = static_cast<std::uint8_t>(nibble & 0x0F) + 1;
      y_offset_.teleport(static_cast<float>(index_) * font_height_);
      settled_ = false;
    }

    // Animated roll; direction follows the semantic increment/decrement.
    void roll_to(std::uint8_t nibble, bool increasing)
    {
      const auto target = static_cast<std::uint8_t>(nibble & 0x0F);
      for (std::uint8_t guard = 0; guard < LIST_SIZE && value() != target; ++guard)
      {
        step(increasing);
      }
    }

    // Drive from the controller's motion timer.
    void update()
    {
      if (column_ == nullptr) return;
      y_offset_.update();
      if (settled_ && y_offset_.done()) return;
      const auto offset = static_cast<std::int32_t>(y_offset_.directValue());
      lv_obj_set_y(label_, -offset);
      settled_ = y_offset_.done();
    }

    void set_pos(std::int32_t x, std::int32_t y)
    {
      if (column_ != nullptr) lv_obj_set_pos(column_, x, y);
    }

    std::int32_t width() const { return column_width_; }
    std::int32_t height() const { return font_height_; }

    std::uint8_t value() const
    {
      return static_cast<std::uint8_t>((index_ + 15) % 16);
    }

  private:
    static constexpr char DIGITS[17] = "0123456789ABCDEF";

    static char digit_at(std::uint8_t list_index)
    {
      if (list_index == 0) return 'F';
      if (list_index == LIST_SIZE - 1) return '0';
      return DIGITS[list_index - 1];
    }

    void step(bool increasing)
    {
      if (increasing)
      {
        if (index_ >= LIST_SIZE - 2)
        {
          index_ = 1;
          y_offset_.retarget(0.0F, static_cast<float>(font_height_));
        }
        else
        {
          ++index_;
          y_offset_.move(static_cast<float>(index_) * font_height_);
        }
      }
      else
      {
        if (index_ <= 1)
        {
          index_ = LIST_SIZE - 2;
          y_offset_.retarget(static_cast<float>(LIST_SIZE - 1) * font_height_,
                             static_cast<float>(index_) * font_height_);
        }
        else
        {
          --index_;
          y_offset_.move(static_cast<float>(index_) * font_height_);
        }
      }
      settled_ = false;
    }

    lv_obj_t* column_ = nullptr;
    lv_obj_t* label_ = nullptr;
    const lv_font_t* font_ = nullptr;
    smooth_ui_toolkit::AnimateValue y_offset_;
    char text_[LIST_SIZE * 2] = {};
    std::int32_t font_height_ = 0;
    std::int32_t column_width_ = 0;
    std::uint8_t index_ = 1;
    bool settled_ = true;
  };
}
