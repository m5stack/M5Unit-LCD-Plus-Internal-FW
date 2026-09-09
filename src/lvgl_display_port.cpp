#include "lvgl_display_port.hpp"

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace lvgl_display_port
{
  namespace
  {
    std::uint8_t* allocate_draw_buffer()
    {
      return static_cast<std::uint8_t*>(
        heap_caps_malloc(port_t::DRAW_BUFFER_BYTES,
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    }
  }

  port_t::~port_t()
  {
    deinit();
  }

  bool port_t::init(LGFX_Device& lcd,
                    flush_allowed_callback_t flush_allowed,
                    void* user_data,
                    tick_callback_t tick_callback,
                    bool asynchronous_flush,
                    present_callback_t present)
  {
    if (display_ != nullptr)
    {
      return lcd_ == &lcd;
    }

    if (!lv_is_initialized())
    {
      lv_init();
    }

    lcd_ = &lcd;
    flush_allowed_callback_ = flush_allowed;
    present_callback_ = present;
    user_data_ = user_data;
    tick_callback_ = tick_callback != nullptr ? tick_callback : arduino_tick_ms;
    flush_enabled_ = true;
    asynchronous_flush_ = asynchronous_flush;

    lv_tick_set_cb(tick_callback_);

    draw_buffers_[0] = allocate_draw_buffer();
    draw_buffers_[1] = allocate_draw_buffer();
    if (draw_buffers_[0] == nullptr || draw_buffers_[1] == nullptr)
    {
      deinit();
      return false;
    }

    display_ = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (display_ == nullptr)
    {
      deinit();
      return false;
    }

    // LVGL RGB565 is the ordinary little-endian 5/6/5 layout. The flush path
    // presents it to M5GFX as rgb565_t, which selects conversion to the panel's
    // native RGB888 stream without changing the LVGL buffer in place.
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display_, flush_callback);
    lv_display_set_flush_wait_cb(display_, flush_wait_callback);
    lv_display_set_buffers(display_,
                           draw_buffers_[0],
                           draw_buffers_[1],
                           DRAW_BUFFER_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_driver_data(display_, this);
    return true;
  }

  void port_t::deinit()
  {
    if (lcd_ != nullptr)
    {
      lcd_->waitDMA();
    }

    if (display_ != nullptr)
    {
      lv_display_delete(display_);
      display_ = nullptr;
    }

    for (auto& buffer : draw_buffers_)
    {
      if (buffer != nullptr)
      {
        heap_caps_free(buffer);
        buffer = nullptr;
      }
    }

    lcd_ = nullptr;
    flush_allowed_callback_ = nullptr;
    present_callback_ = nullptr;
    user_data_ = nullptr;
    tick_callback_ = nullptr;
    flush_enabled_ = true;
    asynchronous_flush_ = false;
  }

  std::uint32_t port_t::handle()
  {
    return display_ == nullptr ? LV_NO_TIMER_READY : lv_timer_handler();
  }

  void port_t::wait_for_flush()
  {
    if (lcd_ != nullptr)
    {
      lcd_->waitDMA();
    }
  }

  std::uint32_t port_t::arduino_tick_ms()
  {
    return static_cast<std::uint32_t>(millis());
  }

  bool port_t::flush_allowed() const
  {
    return flush_enabled_
        && (flush_allowed_callback_ == nullptr
            || flush_allowed_callback_(user_data_));
  }

  void port_t::flush_callback(lv_display_t* display,
                              const lv_area_t* area,
                              std::uint8_t* px_map)
  {
    auto* self = static_cast<port_t*>(lv_display_get_driver_data(display));
    if (self != nullptr)
    {
      self->flush(display, area, px_map);
    }
    else
    {
      lv_display_flush_ready(display);
    }
  }

  void port_t::flush_wait_callback(lv_display_t* display)
  {
    auto* self = static_cast<port_t*>(lv_display_get_driver_data(display));
    if (self != nullptr && self->asynchronous_flush_)
    {
      self->wait_for_flush();
    }
  }

  void port_t::flush(lv_display_t* display,
                    const lv_area_t* area,
                    std::uint8_t* px_map)
  {
    if (lcd_ == nullptr || area == nullptr || px_map == nullptr || !flush_allowed())
    {
      lv_display_flush_ready(display);
      return;
    }

    const auto width = lv_area_get_width(area);
    const auto height = lv_area_get_height(area);
    if (width <= 0 || height <= 0)
    {
      lv_display_flush_ready(display);
      return;
    }

    // The application normally supplies a presenter that copies this tile into
    // the shared RGB888 host canvas.  It is deliberately synchronous: the
    // presenter fences any previous SPI DMA before touching the canvas, so LVGL
    // may immediately reuse its partial draw buffer after flush_ready().
    if (present_callback_ != nullptr)
    {
      present_callback_(area->x1, area->y1, width, height, px_map, user_data_);
      lv_display_flush_ready(display);
      return;
    }

    lcd_->pushImageDMA(area->x1,
                       area->y1,
                       width,
                       height,
                       reinterpret_cast<const lgfx::rgb565_t*>(px_map));

    if (!asynchronous_flush_)
    {
      // The first port keeps the transaction and source-buffer lifetime
      // explicit. The async mode below is available once owner transitions
      // have a tested DMA hand-off protocol.
      lcd_->waitDMA();
      lv_display_flush_ready(display);
    }
  }
}
