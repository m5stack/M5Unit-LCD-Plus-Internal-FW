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
  // Initializes only the reply snapshot/cursor; does not touch the I2C hardware FIFO.
  void beginTxResponse(void);
  // Produces at most capacity bytes and advances the logical cursor only by what was produced.
  std::size_t produceTx(std::uint8_t* destination, std::size_t capacity);
}
