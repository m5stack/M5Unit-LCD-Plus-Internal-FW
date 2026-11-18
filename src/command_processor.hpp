//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

#pragma GCC optimize ("O3")

#include <cstddef>
#include <cstdint>

namespace command_processor
{
  // The LCD has one active producer at a time.  LOCAL_UI is the boot-time LVGL
  // producer, HOST_OWNED is latched by the first valid pixel-writing command,
  // and IAP is used while the firmware-update path owns the panel.  The value is
  // intentionally exposed as a small read-only state machine so the display port
  // can avoid writing the M5GFX canvas and LVGL surface concurrently.
  enum class display_owner_t : std::uint8_t
  {
    local_ui,
    host_owned,
    firmware_update,
  };

  enum class local_address_status_t : std::uint8_t
  {
    idle,
    pending,
    saved,
    failed,
  };

  void setup(void);
  void loop(void);

  // These accessors are safe to poll from the single Arduino/FreeRTOS task that
  // owns LVGL.  Ownership changes happen only at command execution boundaries;
  // the I2C ISR never calls into LVGL or changes this state.
  display_owner_t display_owner(void);
  bool host_owns_display(void);
  bool firmware_update_owns_display(void);

  // Local UI address changes are serviced by the command-processing task so
  // the NVS commit cannot run from an LVGL callback or an I2C ISR.
  bool request_local_address_change(std::uint8_t address);
  local_address_status_t local_address_change_status(void);
  void acknowledge_local_address_change(void);
  void restart_after_local_address_change(void);
  std::uint8_t current_i2c_address(void);

  // Local UI palette index (matches unit_lcd_ui::variant_t, default utility).
  // The theme applies live in the UI; saving mirrors the address NVS path so
  // the choice survives reboots. Called only from the local UI task.
  std::uint8_t local_ui_theme(void);
  bool save_local_ui_theme(std::uint8_t theme);

  // Copies one LVGL RGB565 tile into the shared RGB888 canvas. The copy is
  // rejected whenever the host or IAP owns the panel, and fences the previous
  // asynchronous SPI transfer before touching the canvas storage.
  bool present_local_ui_tile(std::int32_t x,
                             std::int32_t y,
                             std::int32_t width,
                             std::int32_t height,
                             const std::uint8_t* pixels);

  bool addData(std::uint8_t value);
  void closeData(void);
  // High-water check at which the C61 transport stops RX. Always false on the original ESP32.
  bool rxNeedsBackpressure(void);
  // Whether the whole RX FIFO can be parsed in order under backpressure without overflowing the queue/parser slot.
  bool rxCanDrainReadCommand(std::size_t rx_bytes);
  // Whether the RX tail may be treated as an independent read command on an address match under backpressure.
  bool rxCanCaptureReadCommand(void);
  // Completes a 1-byte read command without queueing it. Returns false for anything else.
  bool tryAcceptReadCommand(std::uint8_t value);
  // Retries queueing the one command parked in the parser slot when the queue was full.
  bool retryPendingRxCommand(void);
  // Drops the incomplete/parked command on backpressure timeout and records it as dropped.
  void discardRxForBackpressureTimeout(void);
  // Initializes only the reply snapshot/cursor; does not touch the I2C hardware FIFO.
  void beginTxResponse(void);
  // Produces at most capacity bytes and advances the logical cursor only by what was produced.
  std::size_t produceTx(std::uint8_t* destination, std::size_t capacity);
}
