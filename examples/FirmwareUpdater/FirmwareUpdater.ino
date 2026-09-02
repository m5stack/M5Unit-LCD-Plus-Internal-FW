#include <Arduino.h>
#include <M5GFX.h>
#include <M5UnitLCD.h>
#include <esp_spi_flash.h>

#include "firmware_unit_lcd.h"
#include "firmware_unit_lcd_plus.h"

static constexpr std::uint8_t DEVICE_ID_0 = 0x77;
static constexpr std::uint8_t DEVICE_ID_1 = 0x89;
static constexpr std::uint32_t POST_UPDATE_REBOOT_GRACE_MS = 4000;
static constexpr int POST_UPDATE_READ_ID_RETRIES = 40;
static constexpr std::uint32_t POST_UPDATE_READ_ID_RETRY_DELAY_MS = 250;

/// The MODEL_ID table in docs/Unit_LCD_Plus_I2C_Protocol_zh.md section 6.1 is authoritative; each enumerator maps 1:1 to that value.
enum class model_id_t : std::uint8_t
{
  unit_lcd = 0,
  unit_lcd_plus = 1,
};

/// Head of esp_image_header_t. Detects a mismatched image before UPDATE_BEGIN.
static constexpr std::uint8_t ESP_IMAGE_MAGIC = 0xE9;
static constexpr std::size_t  ESP_IMAGE_CHIP_ID_OFFSET = 12;

/// Values of esp_chip_id_t. Kept as measured constants rather than the host IDF enum
/// so the check is the same whichever host the updater is built for.
static constexpr std::uint16_t CHIP_ID_ESP32     = 0;
static constexpr std::uint16_t CHIP_ID_ESP32C61  = 20;

struct firmware_image_t
{
  model_id_t model_id;
  std::uint8_t version;
  std::uint16_t chip_id;
  const char* model_name;
  const char* image_name;
  const std::uint8_t* data;
  std::size_t length;
};

static constexpr firmware_image_t firmware_images[] =
{
  { model_id_t::unit_lcd,      firmware_unit_lcd_version,      CHIP_ID_ESP32,
    "Unit LCD",      "firmware_unit_lcd",      firmware_unit_lcd,      firmware_unit_lcd_length      },
  { model_id_t::unit_lcd_plus, firmware_unit_lcd_plus_version, CHIP_ID_ESP32C61,
    "Unit LCD Plus", "firmware_unit_lcd_plus", firmware_unit_lcd_plus, firmware_unit_lcd_plus_length },
};

M5GFX display;
M5UnitLCD display2;

std::uint32_t _crc_table[256];

static void initCRCtable(void)
{
  std::size_t i = 0;
  do
  {
    std::uint32_t c = i << 24;
    std::size_t j = 0;
    do
    {
        c = ( c << 1) ^ ( ( c & 0x80000000) ? 0x04C11DB7 : 0);
    } while ( ++j < 8 );
    _crc_table[i] = c;
  } while ( ++i < 256 );
}

static uint32_t crc32(const std::uint8_t *buf, std::size_t len) {
  std::uint32_t c = 0xffffffff;
  for (std::size_t i = 0; i < len; i++)    {
    c = (c << 8) ^ _crc_table[((c >> 24) ^ buf[i]) & 0xff];
  }
  return c;
}

static const firmware_image_t* selectFirmware(std::uint8_t model_id)
{
  for (const auto& image : firmware_images)
  {
    if (static_cast<std::uint8_t>(image.model_id) == model_id)
    {
      return &image;
    }
  }
  return nullptr;
}

static bool validateSelectedImage(const firmware_image_t& image)
{
  if (image.data == nullptr || image.length == 0)
  {
    Serial.printf("selected image is empty: %s\r\n", image.image_name);
    return false;
  }

  // Besides selecting by MODEL_ID, read the header directly to catch a mixed-up build artifact.
  // Replacing the header is a manual step, so a MODEL_ID match alone cannot detect
  // an image built for a different chip being pasted in.
  if (image.length < ESP_IMAGE_CHIP_ID_OFFSET + 2 || image.data[0] != ESP_IMAGE_MAGIC)
  {
    Serial.printf("selected image is not an application image: %s\r\n", image.image_name);
    return false;
  }
  const std::uint16_t chip_id = static_cast<std::uint16_t>(
      image.data[ESP_IMAGE_CHIP_ID_OFFSET]
    | (image.data[ESP_IMAGE_CHIP_ID_OFFSET + 1] << 8));
  if (chip_id != image.chip_id)
  {
    Serial.printf("selected image targets chip id %u, but %s expects %u: update refused.\r\n",
                  chip_id, image.model_name, image.chip_id);
    return false;
  }

  return true;
}

bool update(const firmware_image_t& image)
{
  display.fillScreen(TFT_WHITE);
  display.setCursor(0, 0);
  display.setFont(&fonts::Font2);
  display.setTextColor(TFT_BLACK, TFT_WHITE);
  display.drawString(image.model_name, 0, 0);
  display.drawString(image.image_name, 0, 20);
  display.drawString("update", 0, 40);

  display.fillRect(10, 112, display.width() - 20, 17, TFT_BLACK);
  display.fillCircle(                   10, 120, 8, TFT_BLACK);
  display.fillCircle( display.width() - 10, 120, 8, TFT_BLACK);

  auto panel = (lgfx::Panel_M5UnitLCD*)display2.getPanel();
  auto bus = (lgfx::Bus_I2C*) panel->getBus();
  auto cfg = bus->config();

  std::uint8_t readbuf[8] = { 0 };
  std::size_t length = image.length;
  std::size_t block = (length + SPI_FLASH_SEC_SIZE - 1) / SPI_FLASH_SEC_SIZE;

  Serial.printf("selected image: %s, target version=%u, length=%u\r\n",
                image.image_name, image.version, static_cast<unsigned>(image.length));

  /// ファームウェア更新コマンド列
  std::uint8_t data[16] = { lgfx::Panel_M5UnitLCD::CMD_UPDATE_BEGIN, 0x77, 0x89, lgfx::Panel_M5UnitLCD::CMD_UPDATE_BEGIN };
  data[4] = length >> 24;
  data[5] = length >> 16;
  data[6] = length >>  8;
  data[7] = length >>  0;

  if (lgfx::i2c::beginTransaction(cfg.i2c_port, cfg.i2c_addr, 400000).has_error()
    || lgfx::i2c::writeBytes(cfg.i2c_port, data, 8).has_error()
    || lgfx::i2c::endTransaction(cfg.i2c_port).has_error())
  {
    return false;
  }

  delay(50);
  data[3] = data[0] = lgfx::Panel_M5UnitLCD::CMD_UPDATE_DATA;

  /// セクタブロック単位(4096Byte) でデータ送信を繰り返す
  for (std::size_t b = 0; b < block; ++b)
  {
    display.fillCircle( 10 + (display.width() - 20) * b / block, 120, 4, TFT_GREEN );
    if (!display.displayBusy())
    {
      display.display();
    }

    auto len = std::min<std::size_t>(SPI_FLASH_SEC_SIZE, length);
    auto crc = crc32(&image.data[b * SPI_FLASH_SEC_SIZE], len);
    data[4] = crc >> 24;  /// 送信するデータのCRC32
    data[5] = crc >> 16;
    data[6] = crc >>  8;
    data[7] = crc >>  0;

    Serial.printf("block %d :", b);
    /// ヘッダおよびデータブロック送信
    if (lgfx::i2c::beginTransaction(cfg.i2c_port, cfg.i2c_addr, 400000).has_error()
    || lgfx::i2c::writeBytes(cfg.i2c_port, data, 8).has_error()
    || lgfx::i2c::writeBytes(cfg.i2c_port, &image.data[b * SPI_FLASH_SEC_SIZE], len).has_error()
    || lgfx::i2c::endTransaction(cfg.i2c_port).has_error())
    {
      Serial.println("fail");
      return false;
    }
    Serial.println("ok");
    /// ビジーチェック
    int retry = 100;
    do
    {
      delay(10);
      readbuf[0] = lgfx::Panel_M5UnitLCD::UPDATE_RESULT_BUSY;
      if (!lgfx::i2c::beginTransaction(cfg.i2c_port, cfg.i2c_addr, 400000, true)
       || !lgfx::i2c::readBytes(cfg.i2c_port, readbuf, 1)
       || !lgfx::i2c::endTransaction(cfg.i2c_port))
      {
        break;
      }
    } while (lgfx::Panel_M5UnitLCD::UPDATE_RESULT_OK != readbuf[0] && --retry);
    if (readbuf[0] != lgfx::Panel_M5UnitLCD::UPDATE_RESULT_OK)
    {
      Serial.printf("fail:%02x\r\n", readbuf[0]);
      return false;
    }
    length -= len;
  }

  data[3] = data[0] = lgfx::Panel_M5UnitLCD::CMD_UPDATE_END;

  lgfx::i2c::endTransaction(cfg.i2c_port);

  if (lgfx::i2c::beginTransaction(cfg.i2c_port, cfg.i2c_addr, 400000).has_error()
    || lgfx::i2c::writeBytes(cfg.i2c_port, data, 4).has_error()
    || lgfx::i2c::endTransaction(cfg.i2c_port).has_error())
  {
    return false;
  }

  return true;
}

bool searchUnitLCD(void)
{
  auto board = display.getBoard();
  if (board == m5gfx::board_t::board_M5Stack)
  {
    if (display2.init(21, 22)) return true;
  }
  else
  if (board == m5gfx::board_t::board_M5Paper)
  {
    lgfx::gpio_hi(5);
    lgfx::pinMode(5, lgfx::pin_mode_t::output);
    if (display2.init(25, 32)) return true;
  }
  else
  if (board == m5gfx::board_t::board_M5StickC
  || board == m5gfx::board_t::board_M5StickCPlus
  || board == m5gfx::board_t::board_M5StackCore2
  || board == m5gfx::board_t::board_M5StackCoreInk
    )
  {
    if (board == m5gfx::board_t::board_M5StackCore2)
    {
      m5gfx::i2c::writeRegister8( 1 , 0x34 , 0x12, 0x40, ~0x00); // EXTEN enable
    }
    if (display2.init(32, 33)) return true;
  }
  else
  {
    if (display2.init(26, 32)) return true; // ATOM
    if (display2.init( 4, 13)) return true; // TimerCam
  }

  return false;
}

void setup(void)
{
  Serial.begin(115200);

  display.init();
  display.setEpdMode(lgfx::epd_mode_t::epd_fastest);

  initCRCtable();

  display.println("search UnitLCD.");
  Serial.println("search UnitLCD.");
  while (!searchUnitLCD())
  {
    delay(100);
  }
}

static bool halted = false;
static const char* halt_reason = "none";
static const firmware_image_t* halted_image = nullptr;
static std::uint8_t last_device_id[4] = { 0 };
static bool has_last_device_id = false;

static void rememberDeviceId(const std::uint8_t* id)
{
  for (std::size_t i = 0; i < sizeof(last_device_id); ++i)
  {
    last_device_id[i] = id[i];
  }
  has_last_device_id = true;
}

static void haltUpdater(const char* reason, const firmware_image_t* image)
{
  halt_reason = reason;
  halted_image = image;
  halted = true;
}

/// Reads READ_ID once. Post-update verification and detection at the top of loop share this path.
static bool readDeviceId(std::uint8_t* out)
{
  auto panel = (lgfx::Panel_M5UnitLCD*)display2.getPanel();
  auto bus = (lgfx::Bus_I2C*) panel->getBus();
  auto cfg = bus->config();

  out[0] = 0x04;
  return lgfx::i2c::beginTransaction(cfg.i2c_port, cfg.i2c_addr, 400000).has_value()
      && lgfx::i2c::writeBytes(cfg.i2c_port, out, 1).has_value()
      && lgfx::i2c::restart(cfg.i2c_port, cfg.i2c_addr, 400000, true).has_value()
      && lgfx::i2c::readBytes(cfg.i2c_port, out, 4).has_value()
      && lgfx::i2c::endTransaction(cfg.i2c_port).has_value();
}

void loop(void)
{
  if (halted)
  {
    // Print the halt reason periodically so it can be read even when connecting later.
    Serial.printf("halted: reason=%s, last READ_ID=%s%02x %02x %02x %02x, ",
                  halt_reason,
                  has_last_device_id ? "" : "unavailable/",
                  last_device_id[0], last_device_id[1], last_device_id[2], last_device_id[3]);
    if (halted_image == nullptr)
    {
      Serial.println("image=<none>, expected chip_id=n/a, version=n/a");
    }
    else
    {
      Serial.printf("image=%s, expected chip_id=%u, version=%u\r\n",
                    halted_image->image_name, halted_image->chip_id, halted_image->version);
    }
    delay(4096);
    return;
  }

  std::uint8_t buf[4] = { 0 };

  if (readDeviceId(buf))
  {
    rememberDeviceId(buf);
    const std::uint8_t device_version = buf[3] & 0x7F;
    const bool device_debug = (buf[3] & 0x80) != 0;
    Serial.printf("found: id=%02x %02x, model=%u, version=%u, debug=%s (raw=%02x)\r\n",
                  buf[0], buf[1], buf[2], device_version,
                  device_debug ? "yes" : "no", buf[3]);

    if (buf[0] == DEVICE_ID_0
     && buf[1] == DEVICE_ID_1)
    {
      const auto image = selectFirmware(buf[2]);
      if (image == nullptr)
      {
        Serial.printf("unknown model id %u: update refused.\r\n", buf[2]);
        display.fillScreen(TFT_WHITE);
        display.setTextColor(TFT_RED, TFT_WHITE);
        display.drawString("unknown model", 0, 0);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        haltUpdater("unknown MODEL_ID", nullptr);
        return;
      }
      else if (!validateSelectedImage(*image))
      {
        display.fillScreen(TFT_WHITE);
        display.setTextColor(TFT_RED, TFT_WHITE);
        display.drawString("invalid image", 0, 0);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        haltUpdater("selected image validation failed", image);
        return;
      }
      else if (!device_debug && device_version == image->version)
      {
        Serial.printf("detected release %s version %u; selected %s version %u: update not needed.\r\n",
                      image->model_name, device_version, image->image_name, image->version);
        delay(2048);
      }
      else
      {
        // Release images are written only when the version differs. For debug images bit 7
        // requests an update even at the same version so a release can be restored. Downgrades used for IAP validation are allowed.
        Serial.printf("detected %s version %u (debug=%s); selected %s version %u.\r\n",
                      image->model_name, device_version, device_debug ? "yes" : "no",
                      image->image_name, image->version);
        display.startWrite();
        const bool written = update(*image);
        if (!written)
        {
          display.drawString("fail", 0, 56);
          Serial.println("fail");
        }
        display.endWrite();
        if (!written)
        {
          delay(8192);
          return;
        }

        // Wait for the reboot after UPDATE_END and verify signature, model and version of the same unit.
        // Having sent everything in update() is no proof that the unit booted the right image.
        // A transaction that overlaps the I2C peripheral shutdown during reboot can block the
        // host for a long time, so start the bounded verification after that window has passed.
        delay(POST_UPDATE_REBOOT_GRACE_MS);
        std::uint8_t verify[4] = { 0 };
        std::uint8_t attempt[4] = { 0 };
        bool read_any = false;
        bool verified = false;
        for (int retry = 0; retry < POST_UPDATE_READ_ID_RETRIES; ++retry)
        {
          delay(POST_UPDATE_READ_ID_RETRY_DELAY_MS);
          if (readDeviceId(attempt))
          {
            read_any = true;
            for (std::size_t i = 0; i < sizeof(verify); ++i)
            {
              verify[i] = attempt[i];
            }
            rememberDeviceId(attempt);
            // The old firmware keeps answering for a while after UPDATE_END is accepted. Treat a
            // stale version or similar mismatch as waiting for reboot; succeed only when all four conditions hold.
            if (verify[0] == DEVICE_ID_0
             && verify[1] == DEVICE_ID_1
             && verify[2] == static_cast<std::uint8_t>(image->model_id)
             && (verify[3] & 0x7F) == image->version)
            {
              verified = true;
              break;
            }
          }
        }

        if (verified)
        {
          display.drawString("success", 0, 56);
          Serial.printf("success: READ_ID=%02x %02x %02x %02x matches %s version %u "
                        "(debug=%s).\r\n",
                        verify[0], verify[1], verify[2], verify[3],
                        image->image_name, image->version,
                        (verify[3] & 0x80) ? "yes" : "no");
        }
        else if (!read_any)
        {
          Serial.printf("READ_ID did not recover after writing %s.\r\n", image->image_name);
          display.setTextColor(TFT_RED, TFT_WHITE);
          display.drawString("no READ_ID", 0, 56);
          display.setTextColor(TFT_BLACK, TFT_WHITE);
          haltUpdater("READ_ID unavailable after update", image);
        }
        else if (verify[0] != DEVICE_ID_0 || verify[1] != DEVICE_ID_1)
        {
          Serial.printf("bad device signature after writing %s: %02x %02x.\r\n",
                        image->image_name, verify[0], verify[1]);
          display.setTextColor(TFT_RED, TFT_WHITE);
          display.drawString("bad signature", 0, 56);
          display.setTextColor(TFT_BLACK, TFT_WHITE);
          haltUpdater("device signature mismatch after update", image);
        }
        else if (verify[2] != static_cast<std::uint8_t>(image->model_id))
        {
          Serial.printf("device reports model %u after writing %s for model %u.\r\n",
                        verify[2], image->image_name,
                        static_cast<std::uint8_t>(image->model_id));
          display.setTextColor(TFT_RED, TFT_WHITE);
          display.drawString("wrong model", 0, 56);
          display.setTextColor(TFT_BLACK, TFT_WHITE);
          haltUpdater("MODEL_ID mismatch after update", image);
        }
        else if ((verify[3] & 0x7F) != image->version)
        {
          Serial.printf("device reports version %u (debug=%s, raw=%02x) after writing %s "
                        "version %u.\r\n",
                        verify[3] & 0x7F, (verify[3] & 0x80) ? "yes" : "no", verify[3],
                        image->image_name, image->version);
          display.setTextColor(TFT_RED, TFT_WHITE);
          display.drawString("wrong version", 0, 56);
          display.setTextColor(TFT_BLACK, TFT_WHITE);
          haltUpdater("firmware version mismatch after update", image);
        }
        delay(8192);
      }
    }
  }
}
