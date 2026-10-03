/*
 * Wallpaper — the screen background on the AMOLED badge
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C  (ESP32-S3R8, 8 MB PSRAM, 32 MB flash)
 *
 * The pictures live in the board's own flash (a LittleFS partition), so the badge is
 * self-contained - no SD card, nothing to lose or knock out. Layout that makes this work:
 *
 *   partition scheme : app5M_little24M_32MB   (app 4.8 MB; spiffs @ 0x910000, 23,986,176 B)
 *   filesystem image : build/wallpaper.littlefs.bin  (built by tools/make_fs_image.py)
 *   on the device    : /wallpaper/*.jpg, all 466x466 baseline JPEG (TJpg_Decoder cannot
 *                      read progressive JPEGs, so tools/prep_wallpapers.py converts them)
 *
 * SHOW below is the list of pictures actually displayed, in order:
 *   - one entry -> that picture stays up permanently: no cycling and no periodic redraw,
 *                  so the screen never flashes under a menu drawn on top of it later
 *   - several   -> they cycle every CYCLE_MS
 *   - empty {}  -> every .jpg in /wallpaper, in name order
 * The files stay on the device either way, so changing this list is a one-line edit and a
 * reflash - never a re-conversion of the images.
 */

#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
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

#define WALLPAPER_DIR "/wallpaper"
#define MAX_WALLPAPERS 32
#define CYCLE_MS 10000

// What to show, by file name, in the order it should appear. One name = a fixed
// background. To use two, list both, e.g.
//   static const char *SHOW[] = { "wp5103353-phone-cyberpunk-wallpapers.jpg", "son_of_sudo.jpg" };
static const char *SHOW[] = { "son_of_sudo.jpg" };
static const int   SHOW_COUNT = sizeof(SHOW) / sizeof(SHOW[0]);

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300  *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

static String  papers[MAX_WALLPAPERS];
static int     paperCount = 0;
static int     current    = 0;
static uint32_t lastCycle = 0;
static uint32_t blocks     = 0;      // how many blocks the decoder actually handed over

// TJpg_Decoder hands the picture over in blocks; this puts each block on the panel.
bool jpegBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  blocks++;
  gfx->draw16bitRGBBitmap(x, y, bitmap, w, h);
  return 1;
}

String baseName(const String &path) {
  int slash = path.lastIndexOf('/');
  return (slash >= 0) ? path.substring(slash + 1) : path;
}

// Decide what to display. Matches the SHOW names against what is really on the device, so
// a typo reports "not on the device" instead of leaving a black screen to puzzle over.
void buildList() {
  String found[MAX_WALLPAPERS];
  int foundCount = 0;

  File dir = LittleFS.open(WALLPAPER_DIR);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("! %s missing in the filesystem\n", WALLPAPER_DIR);
    return;
  }
  File f = dir.openNextFile();
  while (f && foundCount < MAX_WALLPAPERS) {
    String n = f.name();
    if (!n.startsWith("/")) n = String(WALLPAPER_DIR) + "/" + n;
    if (n.endsWith(".jpg") || n.endsWith(".JPG")) found[foundCount++] = n;
    f.close();                                    // close before asking for the next entry
    f = dir.openNextFile();
  }
  dir.close();

  for (int i = 1; i < foundCount; i++) {          // insertion sort by name: stable order
    String key = found[i];
    int j = i - 1;
    while (j >= 0 && found[j] > key) { found[j + 1] = found[j]; j--; }
    found[j + 1] = key;
  }

  paperCount = 0;
  if (SHOW_COUNT == 0) {
    for (int i = 0; i < foundCount && paperCount < MAX_WALLPAPERS; i++) papers[paperCount++] = found[i];
  } else {
    for (int s = 0; s < SHOW_COUNT; s++) {
      bool matched = false;
      for (int i = 0; i < foundCount; i++) {
        if (baseName(found[i]).equalsIgnoreCase(SHOW[s])) {
          papers[paperCount++] = found[i];
          matched = true;
          break;
        }
      }
      if (!matched) Serial.printf("! wanted '%s' but it is not on the device\n", SHOW[s]);
    }
  }

  Serial.printf("files  : %d on the device, %d to show\n", foundCount, paperCount);
  for (int i = 0; i < paperCount; i++) Serial.printf("         %2d. %s\n", i + 1, papers[i].c_str());
}

void showWallpaper(int idx) {
  if (idx < 0 || idx >= paperCount) return;
  uint32_t t0 = millis();
  gfx->fillScreen(RGB565_BLACK);

  // One handle for both the sanity check and the decode. The pre-read proves the file can
  // be opened and really starts with a JPEG marker, so a failure is attributable instead
  // of just "FAILED"; the seek(0) afterwards is essential, because the decode must start
  // at the SOI marker, not at byte 2.
  File f = LittleFS.open(papers[idx].c_str(), FILE_READ);
  if (!f) {
    Serial.printf("bg     : %d/%d %s  could not be opened, skipping\n",
                  idx + 1, paperCount, papers[idx].c_str());
    return;
  }
  uint8_t hdr[2] = {0, 0};
  f.read(hdr, 2);
  Serial.printf("      file: %u bytes, first two %02X %02X (FF D8 = JPEG)\n",
                (unsigned)f.size(), hdr[0], hdr[1]);
  f.seek(0);

  TJpgDec.setJpgScale(1);                         // files are already 466x466
  blocks = 0;
  JRESULT res = TJpgDec.drawFsJpg(0, 0, f);       // this overload closes the handle itself
  f.close();
  Serial.printf("bg     : %d/%d %s  result %d (%s), %lu block(s), %lu ms\n", idx + 1, paperCount,
                papers[idx].c_str(), (int)res, res == JDR_OK ? "ok" : "FAILED", blocks, millis() - t0);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Wallpaper: the screen background ===");

  if (!gfx->begin()) {
    Serial.println("! gfx->begin() FAILED - panel did not initialise");
    return;
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(170);
  Serial.printf("panel  : CO5300 up, %dx%d\n", LCD_WIDTH, LCD_HEIGHT);

  // begin(false): mount only, never format. A silent format here would wipe the flashed
  // image, so a mismatch must fail loudly instead.
  if (!LittleFS.begin(false)) {
    Serial.println("! LittleFS mount FAILED - is the filesystem image flashed at 0x910000?");
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565_RED);
    gfx->setCursor(20, 200);
    gfx->println("no filesystem");
    return;
  }
  Serial.printf("fs     : LittleFS mounted, %u kB total, %u kB used\n",
                (unsigned)(LittleFS.totalBytes() / 1024), (unsigned)(LittleFS.usedBytes() / 1024));

  TJpgDec.setCallback(jpegBlock);
  buildList();

  if (paperCount > 0) {
    current = 0;
    showWallpaper(current);
    lastCycle = millis();
    if (paperCount == 1) Serial.println("single : one picture, so it stays and never redraws");
  } else {
    Serial.println("! nothing to show - check WALLPAPER_DIR and the SHOW list");
  }
  Serial.println("ready");
}

void loop() {
  // Only cycle when there is more than one picture: redrawing a single held picture every
  // few seconds would flash the screen for nothing.
  if (paperCount > 1 && CYCLE_MS > 0 && millis() - lastCycle >= CYCLE_MS) {
    lastCycle = millis();
    current = (current + 1) % paperCount;
    showWallpaper(current);
  }
  delay(50);
}
