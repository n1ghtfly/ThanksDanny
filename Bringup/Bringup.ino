/*
 * Thanks Danny — board bring-up
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C  (ESP32-S3R8, 8 MB PSRAM, 32 MB flash)
 *
 * First light for the CO5300 AMOLED, plus an I2C sweep that proves which chips are
 * actually on the bus instead of assuming. Pins are Waveshare's own, taken from
 * examples/arduino/libraries/Mylibrary/pin_config.h in their repository.
 *
 * Verified facts about this board (from the chip and the vendor, not guessed):
 *   - ESP32-S3 (QFN56) rev v0.2, 8 MB embedded octal PSRAM, 32 MB flash (c8 4019)
 *   - USB-Serial/JTAG, MAC (board-specific, not recorded here)
 *   - Display: CO5300, 466x466, QSPI. The 466-wide panel sits 6 columns into the
 *     controller's 480-wide RAM, hence col_offset1 = 6, which is what Waveshare's
 *     own HelloWorld example passes.
 *   - Touch: CST9217 @ 0x5A (both axes mirrored), PMU: AXP2101 @ 0x34,
 *     IMU: QMI8658 @ 0x6A/0x6B
 *   - Audio is a codec, not a bare DAC: ES8311 output + ES7210 mic ADC, with the
 *     amplifier enabled on GPIO 46. That needs codec setup over I2C later.
 *   - An AMOLED has no backlight pin: brightness is a panel command (0x51).
 */

#include <Arduino_GFX_Library.h>
#include <Wire.h>

#define LCD_SDIO0  4
#define LCD_SDIO1  5
#define LCD_SDIO2  6
#define LCD_SDIO3  7
#define LCD_SCLK   38
#define LCD_RESET  1
#define LCD_CS     12
#define LCD_WIDTH  466
#define LCD_HEIGHT 466

#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT  11
#define TP_RST  2
#define PA_EN   46

Arduino_DataBus  *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
// Typed as Arduino_CO5300, not Arduino_GFX: setBrightness() is the panel's own
// member (an AMOLED has no backlight pin, brightness is command 0x51).
Arduino_CO5300   *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

static int found = 0;

static const char *known(uint8_t addr) {
  switch (addr) {
    case 0x34: return "  <- AXP2101 power management";
    case 0x5A: return "  <- CST9217 touch";
    case 0x6A: return "  <- QMI8658 IMU (alt)";
    case 0x6B: return "  <- QMI8658 IMU";
    default:   return "";
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Thanks Danny bring-up: ESP32-S3-Touch-AMOLED-1.75C ===");
  Serial.printf("chip   : ESP32-S3 rev %d, %u MHz, %u core(s)\n",
                ESP.getChipRevision(), getCpuFrequencyMhz(), ESP.getChipCores());
  Serial.printf("flash  : %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("psram  : %u MB\n", ESP.getPsramSize() / (1024 * 1024));
  Serial.printf("heap   : %u kB free before the panel comes up\n", ESP.getFreeHeap() / 1024);

  // The amp enable line is worth parking low until there is audio to play.
  pinMode(PA_EN, OUTPUT);
  digitalWrite(PA_EN, LOW);

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  Serial.printf("i2c    : sweep on SDA %d / SCL %d\n", IIC_SDA, IIC_SCL);
  for (uint8_t a = 0x03; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("         0x%02X responds%s\n", a, known(a));
      found++;
    }
  }
  Serial.printf("i2c    : %d device(s) found\n", found);

  if (!gfx->begin()) {
    Serial.println("! gfx->begin() FAILED - panel did not initialise");
    Serial.println("  (if this repeats, the vendor bundles its own GFX_Library_for_Arduino)");
  } else {
    Serial.println("panel  : CO5300 up, 466x466");
    gfx->fillScreen(RGB565_BLACK);
    gfx->setBrightness(160);

    // Colour bars, then the title, so a wrong offset or a swapped colour order is
    // visible at a glance rather than needing to be inferred.
    const uint16_t bars[8] = {RGB565_WHITE, RGB565_YELLOW, RGB565_CYAN, RGB565_GREEN,
                              RGB565_MAGENTA, RGB565_RED, RGB565_BLUE, RGB565_BLACK};
    for (int i = 0; i < 8; i++) {
      gfx->fillRect(i * (LCD_WIDTH / 8), 0, LCD_WIDTH / 8 + 1, 120, bars[i]);
    }
    gfx->fillRect(0, 130, LCD_WIDTH, 4, RGB565(255, 140, 0));

    gfx->setTextColor(RGB565_WHITE);
    gfx->setTextSize(3);
    gfx->setCursor(28, 200);
    gfx->println("Thanks");

    gfx->setTextColor(RGB565(255, 200, 60));
    gfx->setTextSize(6);
    gfx->setCursor(28, 250);
    gfx->println("Danny");

    gfx->setTextSize(2);
    gfx->setTextColor(RGB565(180, 200, 255));
    gfx->setCursor(28, 350);
    gfx->println("AMOLED bring-up");
    gfx->setTextSize(1);
    gfx->setCursor(28, 386);
    gfx->printf("466x466 CO5300   %d i2c device(s)\n", found);
    gfx->setCursor(28, 404);
    gfx->println("if the bars are the wrong way round,");
    gfx->setCursor(28, 418);
    gfx->println("or the image sits off-centre, say so");
    Serial.println("screen : test pattern drawn");
  }

  Serial.println("ready");
}

void loop() {
  delay(1000);
}
