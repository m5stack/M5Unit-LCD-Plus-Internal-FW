//! Host-side protocol validation sketch for Unit LCD Plus
//!
//! Connects to a Unit LCD Plus from any Arduino-capable I2C master (M5Stack CoreS3,
//! M5Tab5, ...) and validates the basic commands in docs/Unit_LCD_Plus_I2C_Protocol_zh.md.
//!
//! For every item the sketch reports both whether the result matches the
//! specification (spec) and whether it matches a known defect (predicted-bug), so
//! the same sketch shows the verdict flipping (bug MATCH -> spec PASS) once the firmware is fixed.
//!
//! Destructive or risky items (abandoned IAP / invalid CHANGE_ADDR / UPDATE_END magic)
//! never run automatically; they require an explicit serial trigger ('6' / '7' / '8').
//! Their risks and recovery steps are documented in each function and printed at run time.
//! The READ_BUFCOUNT liveness test takes about 6 seconds and is also triggered explicitly with '9'.

#include <Arduino.h>
#include <Wire.h>
#include <cstdint>

namespace
{
  /// The MODEL_ID table in docs/Unit_LCD_Plus_I2C_Protocol_zh.md section 6.1 is authoritative; each enumerator maps 1:1 to that value.
  enum class model_id_t : std::uint8_t
  {
    unit_lcd = 0,
    unit_lcd_plus = 1,
  };

  // Adjust to the wiring. Example for Grove Port A:
  //   M5Stack CoreS3 : SDA = GPIO2  / SCL = GPIO1
  //   M5Tab5         : SDA = GPIO53 / SCL = GPIO54
  constexpr int PIN_SDA = 2;
  constexpr int PIN_SCL = 1;
  constexpr uint8_t SLAVE_ADDR = 0x3E;   // factory default I2C address

  // Command codes follow docs/Unit_LCD_Plus_I2C_Protocol_zh.md section 5 (command summary)
  constexpr uint8_t CMD_READ_ID       = 0x04;
  constexpr uint8_t CMD_READ_BUFCOUNT = 0x09;
  constexpr uint8_t CMD_COPYRECT      = 0x23;
  constexpr uint8_t CMD_CASET         = 0x2A;
  constexpr uint8_t CMD_RASET         = 0x2B;
  constexpr uint8_t CMD_WRITE_RAW_24  = 0x43;
  constexpr uint8_t CMD_READ_RAW_24   = 0x83;
  constexpr uint8_t CMD_CHANGE_ADDR   = 0xA0;
  constexpr uint8_t CMD_UPDATE_BEGIN  = 0xF0;
  constexpr uint8_t CMD_UPDATE_DATA   = 0xF1;
  constexpr uint8_t CMD_UPDATE_END    = 0xF2;

  constexpr int MAX_PIXELS = 16;

  uint32_t g_run = 0;
  int g_spec_pass = 0;
  int g_spec_fail = 0;
  int g_bug_match = 0;

  //--------------------------------------------------------------------------
  // Basic I2C operations
  //--------------------------------------------------------------------------

  bool i2c_write(const uint8_t* data, size_t len)
  {
    Wire.beginTransmission(SLAVE_ADDR);
    Wire.write(data, len);
    return Wire.endTransmission(true) == 0;
  }

  bool i2c_write1(uint8_t value)
  {
    return i2c_write(&value, 1);
  }

  /// STOP-separated read: send the command -> STOP -> read in a separate transaction
  bool cmd_then_read_stop(uint8_t cmd, uint8_t* dst, size_t len)
  {
    if (!i2c_write1(cmd)) return false;
    if (Wire.requestFrom((int)SLAVE_ADDR, (int)len, (int)true) != (int)len) return false;
    for (size_t i = 0; i < len; ++i) dst[i] = Wire.read();
    return true;
  }

  /// RESTART-joined read: send the command -> read after a repeated START without STOP
  /// (specification section 2.2 allows both transaction shapes)
  bool cmd_then_read_restart(uint8_t cmd, uint8_t* dst, size_t len)
  {
    Wire.beginTransmission(SLAVE_ADDR);
    Wire.write(cmd);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)SLAVE_ADDR, (int)len, (int)true) != (int)len) return false;
    for (size_t i = 0; i < len; ++i) dst[i] = Wire.read();
    return true;
  }

  //--------------------------------------------------------------------------
  // Synchronization: draw commands are queued by the ISR and executed later by the
  // device main loop (section 3.4). READ_RAW reads the canvas directly, so reading
  // before the writes ran returns stale data. Wait until READ_BUFCOUNT reports an empty queue (0xFF).
  //--------------------------------------------------------------------------

  bool wait_queue_empty(uint32_t timeout_ms = 1000)
  {
    const uint32_t t0 = millis();
    do
    {
      uint8_t v = 0;
      if (cmd_then_read_stop(CMD_READ_BUFCOUNT, &v, 1) && v == 0xFF) return true;
      delay(2);
    } while (millis() - t0 < timeout_ms);
    return false;
  }

  bool set_window(uint8_t xs, uint8_t xe, uint8_t ys, uint8_t ye)
  {
    const uint8_t caset[3] = { CMD_CASET, xs, xe };
    const uint8_t raset[3] = { CMD_RASET, ys, ye };
    return i2c_write(caset, sizeof(caset)) && i2c_write(raset, sizeof(raset));
  }

  /// Writes ( 0x10+i , 0x40+i , 0x80+i ) to the i-th pixel of the window.
  /// The low nibble of any byte recovers i, so the pixel order can be judged even
  /// when the channel order is unknown. i goes up to 15 (N <= 16).
  bool write_test_pixels(int n)
  {
    uint8_t buf[1 + MAX_PIXELS * 3];
    buf[0] = CMD_WRITE_RAW_24;
    for (int i = 0; i < n; ++i)
    {
      buf[1 + i * 3 + 0] = 0x10 + i;
      buf[1 + i * 3 + 1] = 0x40 + i;
      buf[1 + i * 3 + 2] = 0x80 + i;
    }
    return i2c_write(buf, 1 + n * 3);
  }

  /// Recovers the pixel index from 3 bytes; returns -1 if it cannot.
  int decode_index(const uint8_t* t)
  {
    const int idx = t[0] & 0x0F;
    int mask = 0;
    for (int k = 0; k < 3; ++k)
    {
      if ((t[k] & 0x0F) != idx) return -1;
      switch (t[k] >> 4)
      {
        case 0x1: mask |= 1; break;
        case 0x4: mask |= 2; break;
        case 0x8: mask |= 4; break;
        default: return -1;
      }
    }
    return (mask == 7) ? idx : -1;
  }

  void print_hex(const uint8_t* data, size_t len)
  {
    for (size_t i = 0; i < len; ++i)
    {
      Serial.printf("%02X", data[i]);
      if (i + 1 < len) Serial.print(' ');
    }
  }

  void print_indices(const int* v, int n)
  {
    Serial.print('[');
    for (int i = 0; i < n; ++i)
    {
      if (v[i] < 0) Serial.print('?'); else Serial.print(v[i]);
      if (i + 1 < n) Serial.print(',');
    }
    Serial.print(']');
  }

  //--------------------------------------------------------------------------
  // Individual tests
  //--------------------------------------------------------------------------

  /// T1: READ_ID (section 6.1). [2] is MODEL_ID, [3] is the debug bit plus the shared version number.
  void test_read_id(void)
  {
    uint8_t rd[4] = {0};
    if (!cmd_then_read_stop(CMD_READ_ID, rd, sizeof(rd)))
    {
      Serial.println("  T1 READ_ID              : I2C FAIL (no response)");
      ++g_spec_fail;
      return;
    }
    // Either model may be attached, so MODEL_ID is judged by whether it is a known value.
    // Checking only the signature would let an undefined MODEL_ID pass unnoticed.
    const bool ok = (rd[0] == 0x77 && rd[1] == 0x89
                  && rd[2] <= static_cast<std::uint8_t>(model_id_t::unit_lcd_plus));
    const uint8_t version = rd[3] & 0x7F;
    const bool debug = (rd[3] & 0x80) != 0;
    Serial.print("  T1 READ_ID              : ");
    print_hex(rd, sizeof(rd));
    Serial.printf("  expect 77 89 <model:0|1> <debug|version> -> %s "
                  "(MODEL_ID=%u, version=%u, debug=%s)\n",
                  ok ? "OK" : "NG", rd[2], version, debug ? "yes" : "no");
    ok ? ++g_spec_pass : ++g_spec_fail;
  }

  /// T2: READ_BUFCOUNT while idle (section 6.2). An empty queue reads 0xFF.
  void test_bufcount_idle(void)
  {
    uint8_t v = 0;
    if (!cmd_then_read_stop(CMD_READ_BUFCOUNT, &v, 1))
    {
      Serial.println("  T2 BUFCOUNT (idle)      : I2C FAIL");
      ++g_spec_fail;
      return;
    }
    const bool ok = (v == 0xFF);
    Serial.printf("  T2 BUFCOUNT (idle)      : 0x%02X  expect 0xFF -> %s\n", v, ok ? "OK" : "NG");
    ok ? ++g_spec_pass : ++g_spec_fail;
  }

  /// T4/T5: write a known pattern to an N-pixel window, read it back and judge the pixel order.
  ///   spec          = as specified (0,1,2,...,N-1)
  ///   predicted-bug = as the known defect (start shifted by 8 mod N pixels = rotated left)
  ///
  /// Known defect: right after the read command completes and advances the read pointer
  /// by 8 px, the ISR resets the TX FIFO and rebuilds the reply, so the start shifts by 8 px.
  /// The shift is 8 mod N, so **N=8 (and 1/2/4) hide the defect and PASS while
  /// N=9 FAILs**. That asymmetry doubles as a self-check of the sketch:
  ///   - N=8 PASS and N=9 FAIL -> the sketch (wiring, sync, decoding) is right and
  ///     the firmware has the 8 px shift defect
  ///   - both N=8 and N=9 FAIL      -> suspect the sketch (wiring, sync) first
  /// Fixed firmware gives spec PASS for every N.
  void test_window(int n, bool restart)
  {
    char label[32];
    snprintf(label, sizeof(label), "N=%-2d %-7s", n, restart ? "RESTART" : "STOP");

    if (!set_window(0, n - 1, 0, 0))
    {
      Serial.printf("  T4 %s : CASET/RASET write FAIL\n", label);
      ++g_spec_fail;
      return;
    }
    if (!write_test_pixels(n))
    {
      Serial.printf("  T4 %s : WRITE_RAW_24 FAIL\n", label);
      ++g_spec_fail;
      return;
    }
    if (!wait_queue_empty())
    {
      Serial.printf("  T4 %s : queue never became empty (sync lost, results below are invalid)\n", label);
      ++g_spec_fail;
      return;
    }

    uint8_t rd[MAX_PIXELS * 3] = {0};
    const bool read_ok = restart ? cmd_then_read_restart(CMD_READ_RAW_24, rd, n * 3)
                                 : cmd_then_read_stop(CMD_READ_RAW_24, rd, n * 3);
    if (!read_ok)
    {
      Serial.printf("  T4 %s : READ_RAW_24 FAIL\n", label);
      ++g_spec_fail;
      return;
    }

    const int shift = 8 % n;
    int expect_spec[MAX_PIXELS];
    int expect_bug[MAX_PIXELS];
    int actual[MAX_PIXELS];
    bool spec_ok = true;
    bool bug_ok = true;
    bool decode_ok = true;
    for (int i = 0; i < n; ++i)
    {
      expect_spec[i] = i;
      expect_bug[i] = (i + shift) % n;
      actual[i] = decode_index(&rd[i * 3]);
      if (actual[i] < 0) decode_ok = false;
      if (actual[i] != expect_spec[i]) spec_ok = false;
      if (actual[i] != expect_bug[i]) bug_ok = false;
    }

    Serial.printf("  T4 %s : spec=%s predicted-bug(shift=%d)=%s  got=",
                  label, spec_ok ? "PASS" : "FAIL", shift, bug_ok ? "MATCH" : "differ");
    print_indices(actual, n);
    if (!decode_ok)
    {
      Serial.print("  raw=");
      print_hex(rd, n * 3);
    }
    Serial.println();

    spec_ok ? ++g_spec_pass : ++g_spec_fail;
    if (bug_ok && !spec_ok) ++g_bug_match;
  }

  /// T3: derive the channel order (WRITE vs READ byte mapping) from the N=8 read-back.
  /// The 3 bytes of each pixel are a permutation of the written values; verify that
  /// the permutation is the same for every pixel. Later checks assume this order.
  void report_channel_order(void)
  {
    if (!set_window(0, 7, 0, 0)) return;
    if (!write_test_pixels(8)) return;
    if (!wait_queue_empty()) return;
    uint8_t rd[8 * 3] = {0};
    if (!cmd_then_read_stop(CMD_READ_RAW_24, rd, sizeof(rd))) return;

    bool consistent = true;
    uint8_t order[3] = { (uint8_t)(rd[0] >> 4), (uint8_t)(rd[1] >> 4), (uint8_t)(rd[2] >> 4) };
    for (int i = 1; i < 8; ++i)
    {
      for (int k = 0; k < 3; ++k)
      {
        if ((rd[i * 3 + k] >> 4) != order[k]) consistent = false;
      }
    }
    Serial.printf("  T3 channel order        : write(1x,4x,8x) -> read(%Xx,%Xx,%Xx)  consistent=%s\n",
                  order[0], order[1], order[2], consistent ? "yes" : "NO");
  }

  //--------------------------------------------------------------------------
  // Explicitly triggered items (not run by the automatic loop)
  //--------------------------------------------------------------------------

  /// T6: abandon an IAP session midway and see whether READ_BUFCOUNT sticks at 0x01.
  ///
  /// !!! WARNING !!!
  /// - This test starts an in-application update (IAP) and abandons it. **Until reset,
  ///   the device stays degraded (BUFCOUNT stuck at 0x01, no power-save recovery)**.
  ///   Recovery: power-cycle the device or reboot it with the RESET command (FF 77 89 FF).
  /// - Sending an IAP data block erases sectors of the next OTA partition.
  ///   This implementation abandons after the header only, so no erase happens, but
  ///   **extending the test to send payload destroys the OTA partition contents**.
  ///   **Do not run it on shipped units (units whose OTA slot holds real firmware)**;
  ///   only dedicated validation units may run it.
  ///
  /// UPDATE_BEGIN / UPDATE_DATA are 8 bytes (sections 15.2 / 15.3). BUFCOUNT sticks
  /// only once the UPDATE_DATA header is processed, so BEGIN alone does not trigger it.
  void test_iap_abandon(void)
  {
    Serial.println("== T6 IAP abandon -> BUFCOUNT stuck ==");
    Serial.println("  WARNING: device stays degraded until reset. Never run on shipped units.");
    uint8_t before = 0;
    cmd_then_read_stop(CMD_READ_BUFCOUNT, &before, 1);

    // UPDATE_BEGIN: F0 77 89 F0 <total size, 32 bit>  keep the total size to one sector
    const uint8_t begin_cmd[8] = { CMD_UPDATE_BEGIN, 0x77, 0x89, CMD_UPDATE_BEGIN,
                                   0x00, 0x00, 0x10, 0x00 };
    Serial.print("  send UPDATE_BEGIN: ");
    print_hex(begin_cmd, sizeof(begin_cmd));
    Serial.println();
    if (!i2c_write(begin_cmd, sizeof(begin_cmd)))
    {
      Serial.println("  UPDATE_BEGIN write FAIL");
      return;
    }
    delay(100);
    uint8_t mid = 0;
    cmd_then_read_stop(CMD_READ_BUFCOUNT, &mid, 1);

    // UPDATE_DATA header only: F1 77 89 F1 <block CRC32>  abandon without sending payload
    const uint8_t data_hdr[8] = { CMD_UPDATE_DATA, 0x77, 0x89, CMD_UPDATE_DATA,
                                  0xDE, 0xAD, 0xBE, 0xEF };
    Serial.print("  send UPDATE_DATA header only: ");
    print_hex(data_hdr, sizeof(data_hdr));
    Serial.println();
    if (!i2c_write(data_hdr, sizeof(data_hdr)))
    {
      Serial.println("  UPDATE_DATA write FAIL");
      return;
    }
    delay(200);
    uint8_t after = 0;
    const bool ok = cmd_then_read_stop(CMD_READ_BUFCOUNT, &after, 1);

    Serial.printf("  BUFCOUNT: before 0x%02X -> after BEGIN 0x%02X -> after DATA header 0x%02X (read=%s)\n",
                  before, mid, after, ok ? "OK" : "FAIL");
    Serial.printf("  predicted-bug (stuck at 0x01) -> %s\n", (after == 0x01) ? "MATCH" : "differ");

    // If it is stuck, the host gets throttled heavily
    uint8_t again = 0;
    delay(500);
    cmd_then_read_stop(CMD_READ_BUFCOUNT, &again, 1);
    Serial.printf("  BUFCOUNT after 0.5s: 0x%02X (0xFF = self-recovered, 0x01 = stuck)\n", again);
    Serial.println("  NOTE: reset the device before any further measurement.");
  }

  /// T7: does an invalid CHANGE_ADDR have a side effect (reboot)? Section 13.1 says invalid commands have none.
  ///
  /// The complement does not match, so the address itself never changes = no risk of losing the address.
  /// Known defect: a request that failed validation still rebooted the device (detected by
  /// the canvas returning to the splash and the known pixels vanishing). Fixed: no side effect (PASS).
  /// Even if it reboots, recovery is automatic (the same address answers again after a few seconds).
  void test_change_addr_invalid(void)
  {
    Serial.println("== T7 invalid CHANGE_ADDR side effect ==");
    // Write known pixels first; a reboot re-initializes the canvas and reveals itself
    if (!set_window(0, 7, 0, 0) || !write_test_pixels(8) || !wait_queue_empty())
    {
      Serial.println("  pre-write FAIL");
      return;
    }
    uint8_t before[8 * 3] = {0};
    if (!cmd_then_read_stop(CMD_READ_RAW_24, before, sizeof(before)))
    {
      Serial.println("  pre-read FAIL");
      return;
    }

    const uint8_t bad[4] = { CMD_CHANGE_ADDR, 0x3F, 0x00, CMD_CHANGE_ADDR }; // complement mismatch
    Serial.print("  send: ");
    print_hex(bad, sizeof(bad));
    Serial.println("  (complement of 0x3F is 0xC0, so this is invalid)");
    i2c_write(bad, sizeof(bad));

    delay(2000); // if it reboots, wait for it to finish

    uint8_t id[4] = {0};
    const bool id_ok = cmd_then_read_stop(CMD_READ_ID, id, sizeof(id));
    uint8_t after[8 * 3] = {0};
    const bool rd_ok = cmd_then_read_stop(CMD_READ_RAW_24, after, sizeof(after));
    const bool same = rd_ok && (memcmp(before, after, sizeof(before)) == 0);

    Serial.printf("  responds at 0x%02X: %s / written pixels intact: %s\n",
                  SLAVE_ADDR, id_ok ? "yes" : "NO", same ? "yes" : "NO");
    Serial.printf("  spec (no side effect) -> %s / predicted-bug (reboots) -> %s\n",
                  same ? "PASS" : "FAIL", same ? "differ" : "MATCH");
  }

  /// T8: is the UPDATE_END magic validated? (section 15.4; the correct sequence is F2 77 89 F2)
  ///
  /// Known defect: update::end() ran without checking the magic; on a unit that never
  /// received UPDATE_BEGIN since boot the OTA partition is still unselected (null), so
  /// the flash write hits an assert -> panic reboot. Fixed: no side effect (PASS).
  ///
  /// !!! WARNING !!!
  /// - Run this test **only when UPDATE_BEGIN has never been sent since reset**.
  ///   On a unit that already ran UPDATE_BEGIN (e.g. via T6), unfixed firmware
  ///   proceeds to write the header at the start of the OTA partition and to
  ///   validate the boot partition switch, which can destroy the OTA slot.
  /// - Unfixed firmware panics and reboots (display and encoder state are lost).
  ///   Recovery is automatic (the same address answers again after a few seconds); the
  ///   boot partition does not switch, so the unit is not bricked.
  void test_update_end_magic(void)
  {
    Serial.println("== T8 UPDATE_END magic validation ==");
    Serial.println("  WARNING: run only when no UPDATE_BEGIN was sent since last reset.");
    if (!set_window(0, 7, 0, 0) || !write_test_pixels(8) || !wait_queue_empty())
    {
      Serial.println("  pre-write FAIL");
      return;
    }
    uint8_t before[8 * 3] = {0};
    if (!cmd_then_read_stop(CMD_READ_RAW_24, before, sizeof(before)))
    {
      Serial.println("  pre-read FAIL");
      return;
    }

    const uint8_t bad[4] = { CMD_UPDATE_END, 0x00, 0x00, CMD_UPDATE_END }; // the correct sequence is F2 77 89 F2
    Serial.print("  send: ");
    print_hex(bad, sizeof(bad));
    Serial.println("  (correct magic is F2 77 89 F2, so this is invalid)");
    i2c_write(bad, sizeof(bad));

    delay(2000);

    uint8_t id[4] = {0};
    const bool id_ok = cmd_then_read_stop(CMD_READ_ID, id, sizeof(id));
    uint8_t after[8 * 3] = {0};
    const bool rd_ok = cmd_then_read_stop(CMD_READ_RAW_24, after, sizeof(after));
    const bool same = rd_ok && (memcmp(before, after, sizeof(before)) == 0);

    Serial.printf("  responds at 0x%02X: %s / written pixels intact: %s\n",
                  SLAVE_ADDR, id_ok ? "yes" : "NO", same ? "yes" : "NO");
    Serial.printf("  spec (no side effect) -> %s / predicted-bug (panic reboot) -> %s\n",
                  same ? "PASS" : "FAIL", same ? "differ" : "MATCH");
  }

  /// T9: is READ_BUFCOUNT recomputed on every FIFO refill?
  /// The released host sends 0x09 once and then repeats plain reads while waiting for
  /// the queue to recover. A test that resends the command each time rebuilds the reply
  /// each time and cannot detect the latch-at-command-completion defect, so mirror the host.
  /// It takes about 6 seconds, so it is excluded from the automatic suite and runs only on '9'.
  void test_bufcount_live(void)
  {
    Serial.println("== T9 READ_BUFCOUNT live response ==");

    // Queue 300 heavy full-screen 1 px COPYRECTs to create a window where the value changes.
    const uint8_t copyrect[7] = { CMD_COPYRECT, 0, 0, 133, 239, 1, 0 };
    for (int i = 0; i < 300; ++i)
    {
      i2c_write(copyrect, sizeof(copyrect));
    }

    // Send 0x09 exactly once here; the loop below must not resend it.
    if (!i2c_write1(CMD_READ_BUFCOUNT))
    {
      Serial.println("  READ_BUFCOUNT command write FAIL");
      return;
    }

    uint8_t values[80] = {0};
    int received = 0;
    // Draining the COPYRECTs takes 300 x ~7 ms = 2.1 s. A short window would end before
    // recovery, so keep observing for 80 x 50 ms = 4 s until it returns to 0xFF.
    for (int i = 0; i < 80; ++i)
    {
      if (Wire.requestFrom((int)SLAVE_ADDR, 1, (int)true) == 1)
      {
        values[received++] = Wire.read();
      }
      delay(50);
    }

    Serial.print("  values: ");
    for (int i = 0; i < received; ++i)
    {
      Serial.printf("%02X ", values[i]);
    }
    Serial.println();

    int distinct = 0;
    for (int i = 0; i < received; ++i)
    {
      bool seen = false;
      for (int k = 0; k < i; ++k)
      {
        if (values[k] == values[i])
        {
          seen = true;
          break;
        }
      }
      if (!seen) ++distinct;
    }

    const bool congested = received > 0 && values[0] != 0xFF;
    const bool live = distinct >= 2;
    const bool recovered = received > 0 && values[received - 1] == 0xFF;
    Serial.printf("  distinct=%d first=0x%02X last=0x%02X\n",
                  distinct, received ? values[0] : 0, received ? values[received - 1] : 0);
    if (!congested)
    {
      Serial.println("  INVALID: queue was not congested; increase COPYRECT count");
      return;
    }
    Serial.printf("  live=%s recovered=%s -> %s\n",
                  live ? "yes" : "NO", recovered ? "yes" : "NO",
                  (live && recovered) ? "PASS" : "FAIL");
  }

  /// Lists the addresses that ACK, so a unit whose address was changed is not mistaken.
  void scan_bus(void)
  {
    Serial.print("  bus scan: ");
    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr)
    {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission(true) == 0)
      {
        Serial.printf("0x%02X ", addr);
        ++found;
      }
    }
    if (found == 0) Serial.print("(no response)");
    Serial.println();
  }

  void run_suite(uint32_t hz)
  {
    Wire.setClock(hz);
    Serial.printf("-- I2C %lu Hz --\n", (unsigned long)hz);
    scan_bus();
    test_read_id();
    test_bufcount_idle();
    report_channel_order();
    // Choose window lengths so that 8 mod N is both zero and non-zero (8 hides the defect;
    // 3/9/10/16 do not. 16 is a multiple of 8 but the shift is 8, so it does not hide it)
    static const int sizes[] = { 3, 8, 9, 10, 16 };
    for (int r = 0; r < 2; ++r)
    {
      const bool restart = (r != 0);
      for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
      {
        test_window(sizes[i], restart);
      }
    }
  }
}

void setup(void)
{
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  delay(300);
  Serial.println();
  Serial.println("############################################################");
  Serial.println("# Unit LCD Plus BasicProtocolTest");
  Serial.printf ("# I2C master SDA=GPIO%d SCL=GPIO%d  slave=0x%02X\n", PIN_SDA, PIN_SCL, SLAVE_ADDR);
  Serial.println("# protocol: docs/Unit_LCD_Plus_I2C_Protocol_zh.md");
  Serial.println("# serial input: '6'=IAP abandon test (DANGEROUS, see comments)");
  Serial.println("#               '7'=invalid CHANGE_ADDR test");
  Serial.println("#               '8'=UPDATE_END magic test (read comments first)");
  Serial.println("#               '9'=READ_BUFCOUNT live response test (~6 seconds)");
  Serial.println("############################################################");
}

void loop(void)
{
  while (Serial.available())
  {
    const int c = Serial.read();
    if (c == '6') test_iap_abandon();
    if (c == '7') test_change_addr_invalid();
    if (c == '8') test_update_end_magic();
    if (c == '9') test_bufcount_live();
  }

  ++g_run;
  g_spec_pass = g_spec_fail = g_bug_match = 0;
  Serial.printf("\n===== run #%lu (uptime %lus) =====\n",
                (unsigned long)g_run, (unsigned long)(millis() / 1000));
  run_suite(100000);
  run_suite(400000);
  Serial.printf("----- total: spec PASS=%d FAIL=%d / predicted-bug MATCH=%d -----\n",
                g_spec_pass, g_spec_fail, g_bug_match);

  delay(3000);
}
