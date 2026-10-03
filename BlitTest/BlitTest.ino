/*
 * BlitTest — measure what a slide transition can actually achieve on this panel.
 *
 * The wallpaper measurement (900 x 16x16 block blits, ~390 ms) is NOT the panel's limit: that
 * is 900 separate draw calls, each with its own setup. A slide transition needs full-frame
 * updates from a PSRAM buffer, so this measures the two numbers that decide the design:
 *
 *   1. one full-frame blit   - draw16bitRGBBitmap(0,0,buf,466,466)  (the raw panel rate)
 *   2. one slide frame       - copy a 466-wide window out of a wider image, then blit it
 *                              (what "scroll across a landscape picture" costs per frame)
 *
 * Reports ms per frame and fps for each, plus whether the buffers could be allocated at all.
 * Nothing here is a guess: if the panel cannot do ~10 fps, the transition has to be a stepped
 * wipe and this tells us before the app is written.
 *
 * Flashed to the badge on COM13. Serial 115200.
 */

#include <Arduino_GFX_Library.h>
#include <math.h>

#define LCD_SDIO0  4
#define LCD_SDIO1  5
#define LCD_SDIO2  6
#define LCD_SDIO3  7
#define LCD_SCLK   38
#define LCD_RESET  1
#define LCD_CS     12
#define LCD_WIDTH  466
#define LCD_HEIGHT 466

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300  *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

// A landscape picture prepared for the badge is 835x466 (short side = panel diameter).
#define SRC_W 835
#define SRC_H 466

uint16_t *src  = nullptr;   // the "decoded picture": SRC_W x SRC_H
uint16_t *win  = nullptr;   // the 466-wide window that gets blitted
uint32_t  fails = 0;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// How many frames of a horizontal slide does the panel manage?
void measureSlideFrames(int frames) {
  uint32_t t0 = millis();
  for (int f = 0; f < frames; f++) {
    int off = (f * 7) % (SRC_W - LCD_WIDTH);        // walk across the picture
    // Copy the 466-wide window out of the wider source: this is the cost a pan really pays,
    // because draw16bitRGBBitmap has no stride parameter.
    for (int y = 0; y < LCD_HEIGHT; y++) {
      memcpy(win + (size_t)y * LCD_WIDTH,
             src + (size_t)y * SRC_W + off,
             LCD_WIDTH * 2);
    }
    gfx->draw16bitRGBBitmap(0, 0, win, LCD_WIDTH, LCD_HEIGHT);
  }
  uint32_t ms = millis() - t0;
  Serial.printf("slide  : %d frame(s) in %lu ms -> %lu ms/frame, %.1f fps\n",
                frames, ms, ms / frames, 1000.0f * frames / (ms ? ms : 1));
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== BlitTest: what can this panel actually animate? ===");

  if (!gfx->begin()) { Serial.println("! gfx->begin FAILED"); return; }
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(170);
  Serial.printf("panel  : %dx%d, psram free %u kB, heap free %u kB\n",
                LCD_WIDTH, LCD_HEIGHT,
                (unsigned)(ESP.getFreePsram() / 1024), (unsigned)(ESP.getFreeHeap() / 1024));

  src = (uint16_t *)ps_malloc((size_t)SRC_W * SRC_H * 2);
  win = (uint16_t *)ps_malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2);
  if (!src || !win) {
    Serial.println("! could not allocate PSRAM buffers - the slide cannot work");
    return;
  }
  Serial.printf("buffers: src %u kB + win %u kB allocated in PSRAM, %u kB still free\n",
                (unsigned)((size_t)SRC_W * SRC_H * 2 / 1024),
                (unsigned)((size_t)LCD_WIDTH * LCD_HEIGHT * 2 / 1024),
                (unsigned)(ESP.getFreePsram() / 1024));

  // A picture-like pattern, so the screen visibly moves if anyone is watching.
  for (int y = 0; y < SRC_H; y++) {
    for (int x = 0; x < SRC_W; x++) {
      uint8_t r = (uint8_t)(x * 255 / SRC_W);
      uint8_t g = (uint8_t)(y * 255 / SRC_H);
      uint8_t b = (uint8_t)((x + y) * 255 / (SRC_W + SRC_H));
      src[y * SRC_W + x] = rgb565(r, g, b);
    }
  }
  memcpy(win, src, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);

  gfx->draw16bitRGBBitmap(0, 0, win, LCD_WIDTH, LCD_HEIGHT);
  delay(400);

  // 1. the raw panel rate: one full 466x466 blit per frame, no copying
  uint32_t t0 = millis();
  for (int f = 0; f < 10; f++) {
    gfx->draw16bitRGBBitmap(0, 0, src, LCD_WIDTH, LCD_HEIGHT);
  }
  uint32_t ms = millis() - t0;
  Serial.printf("blit   : 10 full frames in %lu ms -> %lu ms/frame, %.1f fps\n",
                ms, ms / 10, 1000.0f * 10 / (ms ? ms : 1));

  // 2. the honest slide cost: window copy + blit
  measureSlideFrames(10);

  // 3. and without the copy, to show how much of that is memcpy vs panel
  t0 = millis();
  for (int f = 0; f < 10; f++) {
    gfx->draw16bitRGBBitmap(0, 0, win, LCD_WIDTH, LCD_HEIGHT);
  }
  ms = millis() - t0;
  Serial.printf("blit2  : repeated blit of the SAME buffer: %lu ms/frame\n", ms / 10);

  // 4. what 20 frames of a slide would cost in wall-clock time (a 1.5 s transition budget)
  uint32_t per = ms / 10;
  Serial.printf("verdict: a 20-frame slide costs ~%lu ms of drawing (plus the window copy)\n",
                (unsigned long)per * 20);

  Serial.println("ready");
}

void loop() {
  // Keep demonstrating the slide so the panel can be watched while this is flashed.
  measureSlideFrames(20);
  delay(3000);
}
