//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <cstdint>

#if __has_include(<hal/i2c_types.h>)
 #include <hal/i2c_types.h>
#endif
#if __has_include(<hal/gpio_types.h>)
 #include <hal/gpio_types.h>
#endif
#include <driver/i2c.h>

namespace i2c_slave
{
  struct diagnostics_t
  {
    std::uint32_t interrupts;
    std::uint32_t last_status;
    std::uint32_t rx_bytes;
    std::uint32_t tx_fifo_events;
    std::uint32_t tx_bytes;
    std::uint32_t tx_capacity_violations;
    std::uint32_t stretch_address;
    std::uint32_t stretch_tx_empty;
    std::uint32_t stretch_other;
  };

  bool init(int i2c_num, int pin_sda, int pin_scl, std::uint8_t i2c_addr, void* main_handle, int interrupt_flags);
  bool is_busy(void);
  void start_isr(void);
  void stop_isr(void);
  void reset(void);
  // Reads the free FIFO space and writes everything through a 32-byte DRAM staging buffer.
  void fill_txdata(void);
  void clear_txdata(void);
  void get_diagnostics(diagnostics_t* diagnostics);
}
