//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <algorithm>
#include <cstdint>

#include <driver/gpio.h>
#include <esp_intr_alloc.h>
#include <esp_rom_gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/i2c_hal.h>
#include <hal/i2c_ll.h>
#include <soc/i2c_periph.h>

#include "command_processor.hpp"
#include "i2c_slave.hpp"

namespace i2c_slave
{
  namespace
  {
    constexpr std::uint32_t RX_FIFO_FULL_THRESHOLD = 1;
    constexpr std::uint32_t TX_FIFO_EMPTY_THRESHOLD = 2;
    // C61 encodes this as an exponent: timeout period = 2^value source clocks.
    constexpr std::uint32_t SLAVE_TIMEOUT = 22;
    constexpr std::uint32_t INTERRUPT_MASK = I2C_INTR_SLV_RXFIFO_WM
                                           | I2C_INTR_SLV_TXFIFO_WM
                                           | I2C_INTR_SLV_COMPLETE
                                           | I2C_INTR_START
                                           | I2C_INTR_STRETCH;

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
    volatile std::uint32_t diagnostic_stretch_address = 0;
    volatile std::uint32_t diagnostic_stretch_tx_empty = 0;
    volatile std::uint32_t diagnostic_stretch_other = 0;

    i2c_dev_t* hardware()
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
      i2c_ll_enable_bus_clock(i2c_object.port, true);
      i2c_ll_reset_register(i2c_object.port);

      auto* device = hardware();
      i2c_hal_context_t hal = {.dev = device};
      i2c_ll_enable_controller_clock(device, true);
      i2c_ll_set_source_clk(device, I2C_CLK_SRC_DEFAULT);
      i2c_hal_slave_init(&hal);
      i2c_ll_enable_fifo_mode(device, true);
      i2c_ll_set_slave_addr(device, i2c_object.address, false);
      i2c_ll_set_txfifo_empty_thr(device, TX_FIFO_EMPTY_THRESHOLD);
      i2c_ll_set_rxfifo_full_thr(device, RX_FIFO_FULL_THRESHOLD);
      i2c_ll_set_sda_timing(device, 10, 10);
      i2c_ll_set_tout(device, SLAVE_TIMEOUT);
      i2c_ll_slave_enable_scl_stretch(device, true);
      i2c_ll_slave_enable_auto_start(device, true);
      i2c_ll_clear_intr_mask(device, UINT32_MAX);
      i2c_ll_update(device);
    }

    void IRAM_ATTR interrupt_handler(void* argument)
    {
      auto* object = static_cast<i2c_object_t*>(argument);
      auto* device = I2C_LL_GET_HW(object->port);
      std::uint32_t interrupt_status = 0;
      i2c_ll_get_intr_mask(device, &interrupt_status);
      i2c_ll_clear_intr_mask(device, interrupt_status);
      ++diagnostic_interrupts;
      diagnostic_last_status = interrupt_status;

      std::uint32_t rx_count = 0;
      i2c_ll_get_rxfifo_cnt(device, &rx_count);
      bool notify = false;
      bool received_data = false;
      std::uint8_t data[32];
      while (rx_count != 0)
      {
        const auto length = static_cast<std::uint8_t>(std::min<std::uint32_t>(rx_count, sizeof(data)));
        i2c_ll_read_rxfifo(device, data, length);
        diagnostic_rx_bytes += length;
        received_data = true;
        for (std::uint8_t index = 0; index < length; ++index)
        {
          notify |= command_processor::addData(data[index]);
        }
        rx_count -= length;
      }

      if (received_data)
      {
        i2c_ll_txfifo_rst(device);
        command_processor::prepareTxData();
      }

      if (!received_data && (interrupt_status & I2C_INTR_SLV_TXFIFO_WM))
      {
        ++diagnostic_tx_fifo_events;
        command_processor::prepareTxData();
      }

      if (interrupt_status & (I2C_INTR_SLV_COMPLETE | I2C_INTR_START))
      {
        command_processor::closeData();
      }

      if (interrupt_status & I2C_INTR_STRETCH)
      {
        i2c_slave_stretch_cause_t stretch_cause;
        i2c_ll_slave_get_stretch_cause(device, &stretch_cause);
        if (stretch_cause == I2C_SLAVE_STRETCH_CAUSE_ADDRESS_MATCH)
        {
          ++diagnostic_stretch_address;
          std::uint32_t tx_available = 0;
          i2c_ll_get_txfifo_len(device, &tx_available);
          if (tx_available == SOC_I2C_FIFO_LEN)
          {
            command_processor::prepareTxData();
          }
        }
        else if (stretch_cause == I2C_SLAVE_STRETCH_CAUSE_TX_EMPTY)
        {
          ++diagnostic_stretch_tx_empty;
          command_processor::prepareTxData();
        }
        else
        {
          ++diagnostic_stretch_other;
        }
        i2c_ll_slave_clear_stretch(device);
      }

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

  void IRAM_ATTR clear_txdata(void)
  {
    auto* device = hardware();
    i2c_ll_slave_disable_tx_it(device);
    i2c_ll_txfifo_rst(device);
  }

  void IRAM_ATTR add_txdata(const std::uint8_t* buffer, std::size_t length)
  {
    auto* device = hardware();
    std::uint32_t available = 0;
    i2c_ll_get_txfifo_len(device, &available);
    const auto write_length = static_cast<std::uint8_t>(std::min<std::size_t>(length, available));
    if (write_length != 0)
    {
      i2c_ll_write_txfifo(device, buffer, write_length);
      diagnostic_tx_bytes += write_length;
    }
    i2c_ll_clear_intr_mask(device, I2C_INTR_SLV_TXFIFO_WM);
    i2c_ll_slave_enable_tx_it(device);
  }

  void IRAM_ATTR add_txdata(std::uint8_t value)
  {
    add_txdata(&value, 1);
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
      INTERRUPT_MASK,
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
    diagnostics->stretch_address = diagnostic_stretch_address;
    diagnostics->stretch_tx_empty = diagnostic_stretch_tx_empty;
    diagnostics->stretch_other = diagnostic_stretch_other;
  }
}
