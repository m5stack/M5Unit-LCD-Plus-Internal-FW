//! Read the factory MAC from Unit LCD Plus (U242), firmware 1.2 or later.
//! Grove Port A on CoreS3: SDA=2, SCL=1. Adjust these pins for your host.
//! Each loop exercises both STOP-separated and repeated-START reads.
#include <Arduino.h>
#include <Wire.h>
#include <cstring>

namespace
{
  constexpr int PIN_SDA = 2;
  constexpr int PIN_SCL = 1;
  constexpr uint8_t ADDRESS = 0x3E;
  constexpr uint8_t READ_MAC = 0x0A;

  bool readBytes(uint8_t command, uint8_t* data, size_t length, bool repeated_start)
  {
    Wire.beginTransmission(ADDRESS);
    Wire.write(command);
    if (Wire.endTransmission(!repeated_start) != 0) return false;
    if (Wire.requestFrom(static_cast<int>(ADDRESS), static_cast<int>(length))
        != static_cast<int>(length)) return false;
    for (size_t i = 0; i < length; ++i) data[i] = Wire.read();
    return true;
  }

  bool validMac(const uint8_t* mac)
  {
    uint8_t nonzero = 0;
    for (size_t i = 0; i < 6; ++i) nonzero |= mac[i];
    return nonzero != 0 && (mac[0] & 1) == 0;
  }
}

void setup()
{
  Serial.begin(115200);
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  uint8_t id[4] = {};
  const bool supported = readBytes(0x04, id, sizeof(id), true)
                      && id[0] == 0x77 && id[1] == 0x89
                      && id[2] == 1 && id[3] >= 2;
  if (!supported)
  {
    Serial.println("Unit LCD Plus firmware 1.2 or later is required.");
    while (true) delay(1000);
  }
  Serial.printf("Unit LCD Plus firmware %u.%u\n", id[2], id[3]);
}

void loop()
{
  uint8_t stop_mac[6] = {};
  uint8_t restart_mac[6] = {};
  const bool stop_ok = readBytes(READ_MAC, stop_mac, sizeof(stop_mac), false);
  const bool restart_ok = readBytes(READ_MAC, restart_mac, sizeof(restart_mac), true);
  const bool pass = stop_ok && restart_ok
                 && validMac(stop_mac)
                 && memcmp(stop_mac, restart_mac, sizeof(stop_mac)) == 0;
  Serial.printf("READ_MAC STOP=%s RESTART=%s MATCH=%s\n",
                stop_ok ? "OK" : "FAIL", restart_ok ? "OK" : "FAIL",
                pass ? "PASS" : "FAIL");
  if (pass)
  {
    Serial.printf("MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
                  stop_mac[0], stop_mac[1], stop_mac[2],
                  stop_mac[3], stop_mac[4], stop_mac[5]);
  }
  delay(1000);
}
