# Unit LCD Plus Internal Firmware

Arduino/PlatformIO firmware for M5Stack Unit LCD Plus with ESP32-C61.

This project is adapted from
[`M5Unit-LCD-Internal-FW`](https://github.com/m5stack/M5Unit-LCD-Internal-FW)
at commit `6b345872f033dc85fd56e7af722b31583a587a88`. The Unit LCD I2C protocol
and default address (`0x3E`) remain compatible. Unit LCD Plus adds a compatible encoder and button protocol in the
previously unused `0xB0` through `0xB5` range.

## Build

PlatformIO environments:

| Environment | Purpose |
| --- | --- |
| `release`, `esp32_release` | Production internal firmware, version 1.1 |
| `iap_test`, `esp32_iap_test` | Temporary version 1.2 image for online-update validation |
| `debug`, `esp32_debug` | Debug build of version 1.1 (`READ_ID` minor byte reports `0x81`) |

```bash
pio run -e release
pio run -e iap_test
```

Direct USB flashing uses 921600 baud. The encoder button is wired to GPIO9,
the ESP32-C61 download-boot strapping pin (active low). Hold the encoder
button while powering on to enter Joint Download Boot if USB auto-download
is unavailable:

```bash
pio run -e release -t upload --upload-port /dev/ttyACM0
pio device monitor --port /dev/ttyACM0 --baud 115200
```

See `docs/Unit_LCD_Plus_I2C_Protocol_zh.md` for the complete display, input,
address, reset, and IAP protocol. See `docs/encoder_button_protocol_zh.md` or
`docs/encoder_protocol.md` for additional encoder event and ACK details.

## Unit LCD Plus Pins

| Function | ESP32-C61 GPIO |
| --- | ---: |
| Grove SDA / SCL | 5 / 6 |
| UART0 RX / TX | 10 / 11 |
| LCD RST / D-C | 23 / 22 |
| LCD MOSI / SCLK / CS | 28 / 27 / 26 |
| LCD backlight enable | 25, active high |
| Encoder A / B / button | 2 / 3 / 9 |

The internal canvas uses a three-byte RGB stream for compatibility with the
original firmware. M5GFX programs the ST7789 with `COLMOD=0x66`, so the panel
interface is RGB666 even though the internal API color depth is named 24-bit.
External controllers can continue to use the RGB332, RGB565, and RGB888
protocol commands.

## Firmware version 1.1

Current release uses major version 1 and minor version 1. READ_ID returns
`77 89 01 01`; byte 2 is the major version, not MODEL_ID. Byte 3 contains
the minor version (with bit 7 retained for debug builds). Boot and Device Info
read these same version values. IAP test builds use 1.2. Older serial-version
5/6 and model-based updater documentation describes the previous protocol.
