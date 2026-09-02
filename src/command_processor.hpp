//! Copyright (c) M5Stack. All rights reserved.
//! Licensed under the MIT license. See LICENSE file in the project root for full license information.

#pragma once

// #define DEBUG 1
#pragma GCC optimize ("O3")

#include <cstddef>
#include <cstdint>

namespace command_processor
{
  void setup(void);
  void loop(void);

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
