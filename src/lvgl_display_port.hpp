#pragma once

#include <cstddef>
#include <cstdint>

#include <M5GFX.h>
#include <lvgl.h>

namespace lvgl_display_port
{
  /**
   * A display port for the 135 x 240 Unit LCD Plus panel.
   *
   * The port owns LVGL's partial draw buffers and the LVGL display object, but
   * it does not own the application screen or decide who owns the physical LCD.
   * The optional gate is evaluated in the LVGL owner task immediately before a
   * flush, allowing the firmware to keep host/IAP ownership policy elsewhere.
   */
  class port_t final
  {
  public:
    static constexpr std::uint16_t DISPLAY_WIDTH = 135;
    static constexpr std::uint16_t DISPLAY_HEIGHT = 240;
    static constexpr std::uint16_t DRAW_BUFFER_ROWS = 24;
    static constexpr std::size_t BYTES_PER_PIXEL = 2;
    static constexpr std::size_t DRAW_BUFFER_BYTES =
      static_cast<std::size_t>(DISPLAY_WIDTH) * DRAW_BUFFER_ROWS * BYTES_PER_PIXEL;

    using tick_callback_t = std::uint32_t (*)(void);
    using flush_allowed_callback_t = bool (*)(void* user_data);
    using present_callback_t = bool (*)(std::int32_t x,
                                        std::int32_t y,
                                        std::int32_t width,
                                        std::int32_t height,
                                        const std::uint8_t* pixels,
                                        void* user_data);

    port_t() = default;
    ~port_t();

    port_t(const port_t&) = delete;
    port_t& operator=(const port_t&) = delete;

    /**
     * Initialize LVGL's core (if needed), display object, and DMA draw buffers.
     * The M5GFX device must already be initialized and configured for the
     * panel's native 24-bit write depth before this call.
     */
    bool init(LGFX_Device& lcd,
              flush_allowed_callback_t flush_allowed = nullptr,
              void* user_data = nullptr,
              tick_callback_t tick_callback = nullptr,
              bool asynchronous_flush = false,
              present_callback_t present = nullptr);

    /** Wait for any M5GFX SPI transfer and release this port's LVGL objects. */
    void deinit();

    /** Run LVGL timers and rendering from the task that owns this port. */
    std::uint32_t handle();

    /** Return the configured LVGL display, or nullptr before init. */
    lv_display_t* display() const { return display_; }

    /** Explicitly allow or suppress local flushes without changing ownership. */
    void set_flush_enabled(bool enabled) { flush_enabled_ = enabled; }
    bool flush_enabled() const { return flush_enabled_; }

    /** Fence the M5GFX transfer when an owner transition needs a hard boundary. */
    void wait_for_flush();

    /** Default tick source for Arduino builds; replace it in init for other hosts. */
    static std::uint32_t arduino_tick_ms();

    bool initialized() const { return display_ != nullptr; }

  private:
    static void flush_callback(lv_display_t* display,
                               const lv_area_t* area,
                               std::uint8_t* px_map);
    static void flush_wait_callback(lv_display_t* display);

    void flush(lv_display_t* display,
               const lv_area_t* area,
               std::uint8_t* px_map);
    bool flush_allowed() const;

    LGFX_Device* lcd_ = nullptr;
    lv_display_t* display_ = nullptr;
    std::uint8_t* draw_buffers_[2] = { nullptr, nullptr };
    flush_allowed_callback_t flush_allowed_callback_ = nullptr;
    present_callback_t present_callback_ = nullptr;
    void* user_data_ = nullptr;
    tick_callback_t tick_callback_ = nullptr;
    bool flush_enabled_ = true;
    bool asynchronous_flush_ = false;
  };
}
