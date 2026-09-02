//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <esp_attr.h>
#include <driver/gpio.h>
#include <esp_intr_alloc.h>
#include <esp_private/periph_ctrl.h>
#include <esp_rom_gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/i2c_hal.h>
#include <hal/i2c_ll.h>
#include <soc/i2c_periph.h>
#include <soc/soc_caps.h>

#include "command_processor.hpp"
#include "i2c_slave.hpp"

namespace i2c_slave
{
  namespace
  {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    // The C61 can stretch on read address match / RX full, so use a half-FIFO threshold
    // to limit interrupts. A tail below the threshold is also collected on START/COMPLETE.
    constexpr std::uint32_t RX_FIFO_FULL_THRESHOLD = SOC_I2C_FIFO_LEN / 2;
#else
    // The original ESP32 cannot wait on a read address match, so keep an RX threshold of 1
    // so a 1-byte read command is handled before the repeated START.
    constexpr std::uint32_t RX_FIFO_FULL_THRESHOLD = 1;
#endif
    constexpr std::uint32_t TX_FIFO_EMPTY_THRESHOLD = 2;
    constexpr std::size_t TX_STAGING_CAPACITY = 32;
    static_assert(SOC_I2C_FIFO_LEN == TX_STAGING_CAPACITY,
                  "I2C TX staging must match the hardware FIFO");
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    // C61 encodes this as an exponent: timeout period = 2^value source clocks.
    constexpr std::uint32_t SLAVE_TIMEOUT = 22;
    // 2^19 clocks at a 40 MHz XTAL = about 13.1 ms. That leaves margin over the measured
    // 7.05 ms of one COPYRECT while bounding how long SCL is held if main stalls. An IDF
    // master with its ~2 ms timeout may fail, but a host that sees the failure beats a silent drop.
    constexpr std::uint32_t RX_BACKPRESSURE_TIMEOUT_EXPONENT = 19;
    constexpr std::uint32_t BACKPRESSURE_TIMEOUT_INTERRUPT = I2C_SCL_ST_TO_INT_ENA_M;
    constexpr std::uint32_t INTERRUPT_MASK = I2C_INTR_SLV_RXFIFO_WM
                                           | I2C_INTR_SLV_TXFIFO_WM
                                           | I2C_INTR_SLV_COMPLETE
                                           | I2C_INTR_START
                                           | I2C_INTR_STRETCH;
    constexpr std::uint32_t ALLOCATION_INTERRUPT_MASK = INTERRUPT_MASK
                                                      | BACKPRESSURE_TIMEOUT_INTERRUPT;
    constexpr std::uint32_t RX_INTERRUPT = I2C_INTR_SLV_RXFIFO_WM;
    constexpr std::uint32_t TX_INTERRUPT = I2C_INTR_SLV_TXFIFO_WM;
    constexpr std::uint32_t COMPLETE_INTERRUPT = I2C_INTR_SLV_COMPLETE;
    constexpr std::uint32_t START_INTERRUPT = I2C_INTR_START;
#else
    // Arduino-ESP32 3.x / IDF 5.5 no longer expose the old struct field names, so map the
    // same FULL/EMPTY/COMPLETE/START bits onto the LL masks.
    constexpr std::uint32_t SLAVE_TIMEOUT = 0xFFFFF;
    constexpr std::uint32_t RX_INTERRUPT = I2C_RXFIFO_FULL_INT_ENA_M;
    constexpr std::uint32_t TX_INTERRUPT = I2C_TXFIFO_EMPTY_INT_ENA_M;
    constexpr std::uint32_t COMPLETE_INTERRUPT = I2C_TRANS_COMPLETE_INT_ENA_M;
    constexpr std::uint32_t START_INTERRUPT = I2C_TRANS_START_INT_ENA_M;
    constexpr std::uint32_t ARBITRATION_INTERRUPT = I2C_ARBITRATION_LOST_INT_ENA_M;
    constexpr std::uint32_t INTERRUPT_MASK = RX_INTERRUPT
                                           | TX_INTERRUPT
                                           | COMPLETE_INTERRUPT
                                           | START_INTERRUPT
                                           | ARBITRATION_INTERRUPT;
    constexpr std::uint32_t ALLOCATION_INTERRUPT_MASK = INTERRUPT_MASK;
#endif

    struct i2c_object_t
    {
      intr_handle_t interrupt_handle = nullptr;
      i2c_port_t port = I2C_NUM_0;
      TaskHandle_t main_handle = nullptr;
      std::uint8_t address = 0;
    };

    i2c_object_t i2c_object;
    bool initialized = false;
    volatile std::uint32_t diagnostic_interrupts = 0;
    volatile std::uint32_t diagnostic_last_status = 0;
    volatile std::uint32_t diagnostic_rx_bytes = 0;
    volatile std::uint32_t diagnostic_tx_fifo_events = 0;
    volatile std::uint32_t diagnostic_tx_bytes = 0;
    volatile std::uint32_t diagnostic_tx_capacity_violations = 0;
    volatile std::uint32_t diagnostic_stretch_address = 0;
    volatile std::uint32_t diagnostic_stretch_tx_empty = 0;
    volatile std::uint32_t diagnostic_stretch_other = 0;
    volatile std::uint32_t diagnostic_rx_backpressure_holds = 0;
    volatile std::uint32_t diagnostic_rx_backpressure_timeouts = 0;
    DRAM_ATTR std::uint8_t tx_staging_buffer[TX_STAGING_CAPACITY];
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    portMUX_TYPE i2c_mux = portMUX_INITIALIZER_UNLOCKED;
    volatile bool rx_backpressure = false;
    volatile bool rx_backpressure_stretch_held = false;
    volatile bool rx_deferred_byte_valid = false;
    volatile std::uint8_t rx_deferred_byte = 0;
    // Bit N marks a transaction boundary N bytes after the current RX FIFO head.
    // Every FIFO position can be represented, so multiple STOP/STARTs under backpressure are not lost.
    static_assert(SOC_I2C_FIFO_LEN < 64, "RX boundary mask must cover the FIFO");
    volatile std::uint64_t rx_boundary_mask = 0;
#endif

    i2c_dev_t* IRAM_ATTR hardware()
    {
      return I2C_LL_GET_HW(i2c_object.port);
    }

    bool configure_pins(int pin_sda, int pin_scl)
    {
      const gpio_config_t config = {
        .pin_bit_mask = (1ULL << pin_sda) | (1ULL << pin_scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
      };

      if (gpio_config(&config) != ESP_OK)
      {
        return false;
      }

      gpio_set_level(static_cast<gpio_num_t>(pin_sda), 1);
      gpio_set_level(static_cast<gpio_num_t>(pin_scl), 1);
      const auto& signals = i2c_periph_signal[i2c_object.port];
      esp_rom_gpio_connect_out_signal(pin_sda, signals.sda_out_sig, false, false);
      esp_rom_gpio_connect_in_signal(pin_sda, signals.sda_in_sig, false);
      esp_rom_gpio_connect_out_signal(pin_scl, signals.scl_out_sig, false, false);
      esp_rom_gpio_connect_in_signal(pin_scl, signals.scl_in_sig, false);
      return true;
    }

    void configure_peripheral()
    {
#if defined(SOC_RCC_IS_INDEPENDENT) && SOC_RCC_IS_INDEPENDENT
      i2c_ll_enable_bus_clock(i2c_object.port, true);
      i2c_ll_reset_register(i2c_object.port);
#else
      PERIPH_RCC_ATOMIC()
      {
        i2c_ll_enable_bus_clock(i2c_object.port, true);
        i2c_ll_reset_register(i2c_object.port);
      }
#endif

      auto* device = hardware();
      i2c_hal_context_t hal = {.dev = device};
      i2c_ll_enable_controller_clock(device, true);
      i2c_ll_set_source_clk(device, I2C_CLK_SRC_DEFAULT);
      i2c_hal_slave_init(&hal);
      i2c_ll_enable_fifo_mode(device, true);
      i2c_ll_set_slave_addr(device, i2c_object.address, false);
      i2c_ll_set_txfifo_empty_thr(device, TX_FIFO_EMPTY_THRESHOLD);
      i2c_ll_set_rxfifo_full_thr(device, RX_FIFO_FULL_THRESHOLD);
#if defined(CONFIG_IDF_TARGET_ESP32C61)
      i2c_ll_set_sda_timing(device, 10, 10);
      device->scl_st_time_out.scl_st_to_i2c = RX_BACKPRESSURE_TIMEOUT_EXPONENT;
#else
      // Keep sample/hold=4 and the glitch filter enabled (threshold=0) as in the shipped firmware.
      i2c_ll_set_sda_timing(device, 4, 4);
      device->ctr.sda_force_out = 1;
      device->ctr.scl_force_out = 1;
      device->scl_filter_cfg.en = 1;
      device->scl_filter_cfg.thres = 0;
      device->sda_filter_cfg.en = 1;
      device->sda_filter_cfg.thres = 0;
#endif
      i2c_ll_set_tout(device, SLAVE_TIMEOUT);
#if defined(CONFIG_IDF_TARGET_ESP32C61)
      i2c_ll_slave_enable_scl_stretch(device, true);
      i2c_ll_slave_enable_auto_start(device, true);
      i2c_ll_slave_set_stretch_protect_num(device, I2C_LL_STRETCH_PROTECT_TIME);
#endif
      i2c_ll_clear_intr_mask(device, UINT32_MAX);
      i2c_ll_update(device);
    }

#if defined(CONFIG_IDF_TARGET_ESP32C61)
    void IRAM_ATTR record_rx_boundary(std::uint32_t rx_count)
    {
      if (rx_count <= SOC_I2C_FIFO_LEN)
      {
        rx_boundary_mask |= (std::uint64_t{1} << rx_count);
      }
    }

    void IRAM_ATTR apply_rx_boundary_if_due(void)
    {
      if (!rx_backpressure && (rx_boundary_mask & 1U))
      {
        command_processor::closeData();
        rx_boundary_mask &= ~std::uint64_t{1};
      }
    }

    std::uint32_t IRAM_ATTR drain_rx_fifo(i2c_dev_t* device,
                                          std::uint32_t rx_count,
                                          bool* notify,
                                          bool use_reserved_slots = false)
    {
      std::uint32_t drained_rx_bytes = 0;
      // Once the high-water mark is reached the C61 stops taking bytes out of the FIFO and lets
      // the master run into hardware RX_FULL and SCL stretch. Reading one byte at a time keeps
      // the following commands of the same FIFO chunk from being lost in software.
      apply_rx_boundary_if_due();
      while (rx_count != 0 && (!rx_backpressure || use_reserved_slots))
      {
        if (!use_reserved_slots && command_processor::rxNeedsBackpressure())
        {
          rx_backpressure = true;
          i2c_ll_disable_intr_mask(device, RX_INTERRUPT);
          break;
        }
        std::uint8_t value = 0;
        i2c_ll_read_rxfifo(device, &value, 1);
        diagnostic_rx_bytes += 1;
        drained_rx_bytes += 1;
        if (notify != nullptr)
        {
          *notify |= command_processor::addData(value);
        }
        else
        {
          // resume_rx() is called from the main task while it processes commands, so queueing
          // a new command lets the same loop keep draining the queue.
          command_processor::addData(value);
        }
        --rx_count;
        // Advance the FIFO head by one byte and apply a STOP/START boundary reached exactly
        // there to the parser before the next transaction byte.
        rx_boundary_mask >>= 1;
        apply_rx_boundary_if_due();
        if (!use_reserved_slots && command_processor::rxNeedsBackpressure())
        {
          rx_backpressure = true;
          i2c_ll_disable_intr_mask(device, RX_INTERRUPT);
        }
      }
      return drained_rx_bytes;
    }

    void IRAM_ATTR release_backpressure_stretch(i2c_dev_t* device)
    {
      // Do not depend on the STRETCH interrupt firing again for normal recovery. Clean up
      // held and timeout together so a late spurious timeout cannot discard the parser/FIFO.
      // When the ISR drained RX_FULL in the same pass the hardware is still holding even
      // before the software held flag is set, so always release without relying on the flag.
      i2c_ll_slave_clear_stretch(device);
      rx_backpressure_stretch_held = false;
      i2c_ll_disable_intr_mask(device, BACKPRESSURE_TIMEOUT_INTERRUPT);
      i2c_ll_clear_intr_mask(device, BACKPRESSURE_TIMEOUT_INTERRUPT);
    }

#endif

    void IRAM_ATTR interrupt_handler(void* argument)
    {
      auto* object = static_cast<i2c_object_t*>(argument);
      auto* device = I2C_LL_GET_HW(object->port);
      std::uint32_t interrupt_status = 0;
      i2c_ll_get_intr_mask(device, &interrupt_status);
      i2c_ll_clear_intr_mask(device, interrupt_status);
      ++diagnostic_interrupts;
      diagnostic_last_status = interrupt_status;

#if defined(CONFIG_IDF_TARGET_ESP32C61)
      portENTER_CRITICAL_ISR(&i2c_mux);
#endif
      std::uint32_t rx_count = 0;
      i2c_ll_get_rxfifo_cnt(device, &rx_count);
      bool notify = false;
#if defined(CONFIG_IDF_TARGET_ESP32C61)
      const bool is_read = i2c_ll_slave_get_read_write_status(device)
                        == I2C_SLAVE_READ_BY_MASTER;
      // START stays a parser boundary, but handle it before draining so that an RX watermark
      // coalesced into the same status does not close in the middle of received bytes.
      // A 4096-byte IAP block contains no START, so single-transaction semantics are preserved.
      if ((interrupt_status & START_INTERRUPT)
       && !(interrupt_status & COMPLETE_INTERRUPT))
      {
        if (!rx_backpressure)
        {
          command_processor::closeData();
          rx_boundary_mask = 0;
        }
        // Under backpressure START and RX bytes may coalesce into one status and the boundary
        // position is unknown, so use the position recorded at the previous COMPLETE.
        // A single IAP block contains no START, so the existing boundary semantics hold.
      }
      if (interrupt_status & COMPLETE_INTERRUPT)
      {
        // The FIFO level at the time of the status read is the number of undrained bytes up to this STOP.
        // Several transactions can coexist in the same FIFO as different bits.
        record_rx_boundary(rx_count);
      }

      const auto drained_rx_bytes = drain_rx_fifo(device, rx_count, &notify);
#else
      std::uint8_t data[32];
      while (rx_count != 0)
      {
        const auto length = static_cast<std::uint8_t>(std::min<std::uint32_t>(rx_count, sizeof(data)));
        i2c_ll_read_rxfifo(device, data, length);
        diagnostic_rx_bytes += length;
        for (std::uint8_t index = 0; index < length; ++index)
        {
          notify |= command_processor::addData(data[index]);
        }
        rx_count -= length;
      }
#endif

#if defined(CONFIG_IDF_TARGET_ESP32)
      // The original prefetches while receiving, so the reply must not be rebuilt here.
      // Re-initializing returns the READ_RAW cursor to the start and breaks windows that are not a multiple of 8.
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C61)
      if (interrupt_status & TX_INTERRUPT)
      {
        if (is_read)
        {
          ++diagnostic_tx_fifo_events;
          fill_txdata();
        }
        else
        {
          // The TX watermark is a level interrupt; disable it if it is left set outside a read.
          i2c_ll_disable_intr_mask(device, TX_INTERRUPT);
        }
      }

      if (interrupt_status & COMPLETE_INTERRUPT)
      {
        // Never carry a reply the master did not read into the next transaction. is_read may
        // already reflect the next address match, so do not guard on direction or backpressure
        // state: always stop the TX watermark and discard the FIFO on every STOP.
        // If TX_WM coalesced into the same status, discard after the refill above as well.
        i2c_ll_disable_intr_mask(device, TX_INTERRUPT);
        i2c_ll_txfifo_rst(device);
      }
#else
      // As before, the original also handles a TX empty raised together with RX later on.
      if (interrupt_status & TX_INTERRUPT)
      {
        ++diagnostic_tx_fifo_events;
        fill_txdata();
      }
#endif

#if !defined(CONFIG_IDF_TARGET_ESP32C61)
      if (interrupt_status & (COMPLETE_INTERRUPT | START_INTERRUPT | ARBITRATION_INTERRUPT))
      {
        command_processor::closeData();
      }
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C61)
      bool backpressure_timed_out = false;
      if (interrupt_status & BACKPRESSURE_TIMEOUT_INTERRUPT)
      {
        i2c_ll_disable_intr_mask(device, BACKPRESSURE_TIMEOUT_INTERRUPT);
        if (rx_backpressure_stretch_held)
        {
          // Never hold the whole bus forever even if main/IAP processing stalls. Discard the
          // held parser/FIFO, release SCL and return an explicit failure to the master.
          diagnostic_rx_backpressure_timeouts = diagnostic_rx_backpressure_timeouts + 1;
          command_processor::discardRxForBackpressureTimeout();
          i2c_ll_rxfifo_rst(device);
          rx_backpressure = false;
          rx_backpressure_stretch_held = false;
          rx_deferred_byte_valid = false;
          rx_boundary_mask = 0;
          i2c_ll_slave_clear_stretch(device);
          i2c_ll_enable_intr_mask(device, RX_INTERRUPT | I2C_INTR_STRETCH);
          backpressure_timed_out = true;
        }
      }

      if (!backpressure_timed_out && (interrupt_status & I2C_INTR_STRETCH))
      {
        i2c_slave_stretch_cause_t stretch_cause;
        i2c_ll_slave_get_stretch_cause(device, &stretch_cause);
        if (stretch_cause == I2C_SLAVE_STRETCH_CAUSE_ADDRESS_MATCH)
        {
          ++diagnostic_stretch_address;
          if (is_read)
          {
            // Reads consume no RX queue slot. Recovery polling such as READ_BUFCOUNT keeps working
            // under backpressure; the reply is built as usual while the address match is held.
            // If the FIFO holds only the last command, complete it immediately. Under sustained
            // backpressure with leftovers of an old write, confirm the trailing 1-byte transaction
            // by its boundary and parse the whole FIFO in order into the reserved slots before replying.
            i2c_ll_get_rxfifo_cnt(device, &rx_count);
            // During IAP progress/sector_write the FIFO tail may be payload.
            // A status poll does not resend the command, so do not take the byte here; keep the
            // latest UPDATE_DATA and leave the payload to the normal parser drain.
            if (rx_backpressure
             && !rx_deferred_byte_valid
             && command_processor::rxCanCaptureReadCommand())
            {
              if (rx_count == 1)
              {
                // From one byte the hardware cannot tell a transaction start from the tail of a normal
                // write held by backpressure, and a value equal to a read opcode could be misread.
                // IAP payload is already excluded by rxCanCaptureReadCommand(). Normally acceptance is
                // limited to read-only commands that are never queued, so the effect is confined to the
                // reply choice of this poll, does not propagate, and is overwritten by the next real command.
                // Disabling capture would let a genuine 1-byte flow-control read go stale under backpressure, so accept this.
                std::uint8_t value = 0;
                i2c_ll_read_rxfifo(device, &value, 1);
                diagnostic_rx_bytes += 1;
                // Advance the hardware FIFO head. Bit 0 is the STOP boundary right after this byte, but
                // it is consumed here because the read-command completion or the deferred replay closes it.
                rx_boundary_mask >>= 1;
                rx_boundary_mask &= ~std::uint64_t{1};
                if (!command_processor::tryAcceptReadCommand(value))
                {
                  rx_deferred_byte = value;
                  rx_deferred_byte_valid = true;
                }
              }
              else if (rx_count > 1
                    && (rx_boundary_mask & (std::uint64_t{1} << (rx_count - 1)))
                    && command_processor::rxCanDrainReadCommand(rx_count))
              {
                // Under sustained backpressure the leftovers of an old write and the following 1-byte read
                // command share the FIFO. Only when the previous boundary is one byte before the tail,
                // parse everything in bus order into the reserved slots to reach the reply command.
                rx_backpressure = false;
                drain_rx_fifo(device, rx_count, &notify, true);
                if (command_processor::rxNeedsBackpressure())
                {
                  rx_backpressure = true;
                  i2c_ll_disable_intr_mask(device, RX_INTERRUPT);
                }
              }
            }
            clear_txdata();
            command_processor::beginTxResponse();
            fill_txdata();
          }
          i2c_ll_slave_clear_stretch(device);
        }
        else if (stretch_cause == I2C_SLAVE_STRETCH_CAUSE_TX_EMPTY)
        {
          ++diagnostic_stretch_tx_empty;
          fill_txdata();
          i2c_ll_slave_clear_stretch(device);
        }
        else if (rx_backpressure
              && stretch_cause == I2C_SLAVE_STRETCH_CAUSE_RX_FULL)
        {
          if (!rx_backpressure_stretch_held)
          {
            diagnostic_rx_backpressure_holds = diagnostic_rx_backpressure_holds + 1;
          }

          if (drained_rx_bytes != 0)
          {
            // One command worth of space freed by main was drained. Keep the RX watermark at the
            // high-water mark, release only the current stretch and let the master continue.
            release_backpressure_stretch(device);
            i2c_ll_enable_intr_mask(device, I2C_INTR_STRETCH);
          }
          else
          {
            // On the single-core C61 waiting inside the ISR would keep main from freeing the queue.
            // Disable the stretch/WM interrupts, leave the ISR and wait for resume_rx() from main.
            rx_backpressure_stretch_held = true;
            i2c_ll_disable_intr_mask(device, RX_INTERRUPT | I2C_INTR_STRETCH);
            i2c_ll_clear_intr_mask(device, BACKPRESSURE_TIMEOUT_INTERRUPT);
            i2c_ll_enable_intr_mask(device, BACKPRESSURE_TIMEOUT_INTERRUPT);
          }
        }
        else
        {
          ++diagnostic_stretch_other;
          // Only RX_FULL is held for backpressure. Do not drag a write address match or an
          // unknown cause into the 13.1 ms hold; release it immediately.
          i2c_ll_slave_clear_stretch(device);
        }
      }
      portEXIT_CRITICAL_ISR(&i2c_mux);
#endif

      if (notify)
      {
        BaseType_t higher_priority_task_woken = pdFALSE;
        vTaskNotifyGiveFromISR(object->main_handle, &higher_priority_task_woken);
        if (higher_priority_task_woken == pdTRUE)
        {
          portYIELD_FROM_ISR();
        }
      }
    }
  }

  bool IRAM_ATTR is_busy(void)
  {
    return initialized && i2c_ll_is_bus_busy(hardware());
  }

  void IRAM_ATTR resume_rx(void)
  {
#if defined(CONFIG_IDF_TARGET_ESP32C61)
    if (!initialized || !rx_backpressure) return;
    auto* device = hardware();
    portENTER_CRITICAL(&i2c_mux);
    // If a command was parked while completely full, queue it first into the space main freed.
    // Once below the high-water mark, drain the remaining FIFO, including what is below the
    // threshold, from main so the parked COMPLETE boundary is collected without waiting for another RX watermark.
    if (command_processor::retryPendingRxCommand()
     && !command_processor::rxNeedsBackpressure())
    {
      rx_backpressure = false;
      if (rx_deferred_byte_valid)
      {
        // A non-read byte set aside during an address-match read to avoid side effects is put
        // back in its original order from the main context once the queue has room, closed by the Sr boundary.
        command_processor::closeData();
        command_processor::addData(rx_deferred_byte);
        command_processor::closeData();
        rx_deferred_byte_valid = false;
        if (command_processor::rxNeedsBackpressure())
        {
          rx_backpressure = true;
          i2c_ll_disable_intr_mask(device, RX_INTERRUPT);
        }
      }
      std::uint32_t rx_count = 0;
      i2c_ll_get_rxfifo_cnt(device, &rx_count);
      const auto drained_rx_bytes = drain_rx_fifo(device, rx_count, nullptr);

      // Release the current SCL hold as soon as at least one byte advanced. If the high-water
      // mark is reached again while draining, keep RX stopped and wait for the next RX_FULL stretch.
      if (!rx_backpressure || drained_rx_bytes != 0)
      {
        release_backpressure_stretch(device);
      }
      i2c_ll_enable_intr_mask(device,
                              (rx_backpressure ? 0 : RX_INTERRUPT)
                            | I2C_INTR_STRETCH);
    }
    portEXIT_CRITICAL(&i2c_mux);
#endif
  }

  void IRAM_ATTR clear_txdata(void)
  {
    auto* device = hardware();
    i2c_ll_disable_intr_mask(device, TX_INTERRUPT);
    i2c_ll_txfifo_rst(device);
  }

  void IRAM_ATTR fill_txdata(void)
  {
    auto* device = hardware();
    std::uint32_t available = 0;
    i2c_ll_get_txfifo_len(device, &available);
    // Also called from the status-read address match during an OTA flash write. Keep this
    // path to IRAM functions and DRAM data only, so debug builds do not out-line std::min
    // or ordinary inline helpers into flash.
    const std::size_t capacity = available < TX_STAGING_CAPACITY
                               ? available
                               : TX_STAGING_CAPACITY;
    const auto produced = command_processor::produceTx(tx_staging_buffer, capacity);

    // If the producer violates the capacity contract, hold the whole reply instead of writing part of it.
    // Normally the whole produced amount is transferred in one FIFO write.
    if (produced > capacity)
    {
      diagnostic_tx_capacity_violations = diagnostic_tx_capacity_violations + 1;
    }
    else if (produced != 0)
    {
      i2c_ll_write_txfifo(device, tx_staging_buffer, static_cast<std::uint8_t>(produced));
      diagnostic_tx_bytes += produced;
    }
    i2c_ll_clear_intr_mask(device, TX_INTERRUPT);
    i2c_ll_enable_intr_mask(device, TX_INTERRUPT);
  }

  void IRAM_ATTR start_isr(void)
  {
    if (!initialized) return;
    auto* device = hardware();
    i2c_ll_clear_intr_mask(device, INTERRUPT_MASK);
    i2c_ll_enable_intr_mask(device, INTERRUPT_MASK);
  }

  void IRAM_ATTR stop_isr(void)
  {
    if (!initialized) return;
    auto* device = hardware();
    i2c_ll_disable_intr_mask(device, UINT32_MAX);
    i2c_ll_clear_intr_mask(device, UINT32_MAX);
  }

  bool init(int i2c_num, int pin_sda, int pin_scl, std::uint8_t i2c_addr, void* main_handle, int interrupt_flags)
  {
    if (initialized || i2c_num != I2C_NUM_0 || main_handle == nullptr)
    {
      return false;
    }

    i2c_object.port = static_cast<i2c_port_t>(i2c_num);
    i2c_object.address = i2c_addr;
    i2c_object.main_handle = static_cast<TaskHandle_t>(main_handle);
    if (!configure_pins(pin_sda, pin_scl))
    {
      return false;
    }

    configure_peripheral();
    auto* device = hardware();
    const esp_err_t result = esp_intr_alloc_intrstatus(
      i2c_periph_signal[i2c_object.port].irq,
      interrupt_flags,
      reinterpret_cast<std::uint32_t>(i2c_ll_get_interrupt_status_reg(device)),
      ALLOCATION_INTERRUPT_MASK,
      interrupt_handler,
      &i2c_object,
      &i2c_object.interrupt_handle);

    if (result != ESP_OK)
    {
      return false;
    }

    initialized = true;
    start_isr();
    return true;
  }

  void shutdown(void)
  {
    if (!initialized) return;
    stop_isr();
    // Reset the module so the peripheral, still configured, does not answer the bus on its
    // own (address match / auto_start read) while the chip reboots.
#if defined(SOC_RCC_IS_INDEPENDENT) && SOC_RCC_IS_INDEPENDENT
    i2c_ll_reset_register(i2c_object.port);
#else
    PERIPH_RCC_ATOMIC()
    {
      i2c_ll_reset_register(i2c_object.port);
    }
#endif
  }

  void reset(void)
  {
    if (!initialized) return;
    stop_isr();
    configure_peripheral();
    start_isr();
  }

  void get_diagnostics(diagnostics_t* diagnostics)
  {
    if (diagnostics == nullptr) return;
    diagnostics->interrupts = diagnostic_interrupts;
    diagnostics->last_status = diagnostic_last_status;
    diagnostics->rx_bytes = diagnostic_rx_bytes;
    diagnostics->tx_fifo_events = diagnostic_tx_fifo_events;
    diagnostics->tx_bytes = diagnostic_tx_bytes;
    diagnostics->tx_capacity_violations = diagnostic_tx_capacity_violations;
    diagnostics->stretch_address = diagnostic_stretch_address;
    diagnostics->stretch_tx_empty = diagnostic_stretch_tx_empty;
    diagnostics->stretch_other = diagnostic_stretch_other;
    diagnostics->rx_backpressure_holds = diagnostic_rx_backpressure_holds;
    diagnostics->rx_backpressure_timeouts = diagnostic_rx_backpressure_timeouts;
  }
}
