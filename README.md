# Unit LCD Plus Internal Firmware

Arduino/PlatformIO firmware for M5Stack Unit LCD Plus with ESP32-C61.

This project is adapted from
[`M5Unit-LCD-Internal-FW`](https://github.com/m5stack/M5Unit-LCD-Internal-FW)
at commit `6b345872f033dc85fd56e7af722b31583a587a88`. The Unit LCD I2C protocol
and default address (`0x3E`) remain compatible. Unit LCD Plus (`READ_ID`
byte 2 `MODEL_ID` = 1) adds a compatible encoder and button protocol in the
previously unused `0xB0` through `0xB5` range.

## Build

PlatformIO environments:

| Environment | Purpose |
| --- | --- |
| `release`, `esp32_release` | Production internal firmware, shared version 5 |
| `iap_test`, `esp32_iap_test` | Temporary shared version 6 image for online-update validation |
| `debug`, `esp32_debug` | Debug build of shared version 5 (`READ_ID` reports `0x85`) |

```bash
pio run -e release
pio run -e iap_test
```

Direct USB flashing uses 921600 baud:

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
| Encoder A / B / button | 2 / 3 / 4 |

The internal canvas uses a three-byte RGB stream for compatibility with the
original firmware. M5GFX programs the ST7789 with `COLMOD=0x66`, so the panel
interface is RGB666 even though the internal API color depth is named 24-bit.
External controllers can continue to use the RGB332, RGB565, and RGB888
protocol commands.
