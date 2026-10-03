/*
 * Badge — the "Thanks Danny" AMOLED badge: wallpaper, cog, settings, Wi-Fi setup
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C  (ESP32-S3R8, 8 MB PSRAM, 32 MB flash)
 *
 * THE PANEL IS ROUND. 466x466 is the bounding square, not the visible area: the corners are
 * masked off by the round aperture, so anything drawn near a corner - a cog at (400,46), a
 * pill at (14,418) - is invisible on the glass while every log says it drew fine. That is
 * exactly how the first version of this sketch lost its settings button. Read insidePanel()
 * and the boot-time layout check in setup() before adding any screen furniture.
 * Rule of thumb: keep interactive things inside the inscribed circle, ideally within r-25.
 *
 * DRAWING GOES THROUGH AN OFF-SCREEN CANVAS (v2, the "sluggish" fix). `gfx` is an
 * Arduino_Canvas in PSRAM; `panel` is the real CO5300. Every screen is composed in RAM, where
 * fillScreen / text / circles cost a few ms, and then sent to the glass in ONE flush. Drawing
 * primitives straight at the panel was the slow part: fillScreen alone measured ~350 ms
 * (~0.6 Mpixel/s), and every screen change started with one - the home screen even painted it
 * black and then covered it with the wallpaper. A full-frame flush measured 56 ms at the old
 * 40 MHz bus clock; the bus now runs at LCD_QSPI_HZ (80 MHz).
 * Rule: draw on `gfx`, then call gfx->flush(). Nothing appears until the flush.
 *
 * Touch: the controller is read with getPoint() (about 2 ms on a 400 kHz bus). It is polled
 * every couple of milliseconds, and a tap fires on the press edge with a 90 ms guard. Two
 * traps cost real time here: leaving the controller asleep (touch.sleep()) reports zero points
 * forever and looks like a dead panel, and a long poll delay makes a working panel feel broken.
 *
 * What it does
 *   Home screen   : the wallpaper; the Decider coin upper left, a cog wheel button upper right
 *                   (both inside the circle), the Pictures button under the sign, and a
 *                   status pill inside the bottom of the circle
 *   Coin -> Decider : a coin toss that lands on Approved / Disapproved by Danny (Decider.ino)
 *   Settings      : "Wi-Fi setup on phone", "Close" (v2.9: Pictures and Touch test removed)
 *   Wi-Fi setup   : when the badge has no working network it raises its OWN access point and
 *                   serves a small web page. Join that AP from a phone, pick your Wi-Fi from
 *                   a scanned list, type the password on the phone's keyboard, and the badge
 *                   saves it and connects.
 *
 * Why setup happens on the phone rather than on the badge: a phone keyboard is the right tool
 * for a Wi-Fi password, and this way the password is typed straight into the device. It is
 * never printed to the serial log - only the SSID and the password's LENGTH are ever logged -
 * so it cannot leak into a chat, screenshot or pasted log.
 *
 * Two things that are easy to get wrong here and are deliberate below:
 *   - Scanning runs in AP+STA mode. A scan in pure AP mode drops the phone that is loading
 *     the setup page, so the list is scanned once and cached.
 *   - The radio runs AP+STA throughout, so a wrong password does not take the setup page
 *     away: the reply is sent, then the badge connects while the AP stays up. Only success
 *     tears the AP down.
 *
 * Credentials live in NVS (Preferences, namespace "wifi"). That is plaintext flash, as usual
 * for ESP32 sketches; the guarantee this code makes is about logs and screens.
 *
 *   partition scheme : app5M_little24M_32MB (app 4.8 MB; spiffs @ 0x910000, 23,986,176 B)
 *   filesystem image : build/wallpaper.littlefs.bin  (tools/make_fs_image.py)
 *   display/touch    : CO5300 466x466 ROUND; CST9217 @0x5A, both axes mirrored (Waveshare)
 */

#include <Arduino_GFX_Library.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>
#include "TouchDrvCSTXXX.hpp"

// The gallery does TLS work several call levels below loop() (loop -> pollTouch -> handleTap ->
// showScreen -> showGallery -> syncPictures), and mbedTLS needs a few kB of stack of its own.
// The default 8 kB loop stack is not generous enough for that nesting, so raise it. Must be at
// file scope before setup().
SET_LOOP_TASK_STACK_SIZE(32 * 1024);

// ---------------------------------------------------------------- pins
#define LCD_SDIO0  4
#define LCD_SDIO1  5
#define LCD_SDIO2  6
#define LCD_SDIO3  7
#define LCD_SCLK   38
#define LCD_RESET  1
#define LCD_CS     12
#define LCD_WIDTH  466
#define LCD_HEIGHT 466
// QSPI clock for the panel. The library default is 40 MHz. 80 MHz roughly halves the time of
// every frame sent to the glass. IF THE SCREEN SHOWS NOISE, STRIPES OR WRONG COLOURS after
// flashing, set this back to 40000000 - that is the only thing that changes with it.
#define LCD_QSPI_HZ 80000000

#define TOUCH_SDA  15
#define TOUCH_SCL  14
#define TOUCH_RST  2
#define TOUCH_INT  11
#define TOUCH_ADDR 0x5A
#define TOUCH_HZ   400000                 // the default 100 kHz is needlessly slow for reads

#define WALLPAPER_FILE "/wallpaper/son_of_sudo.jpg"
#define AP_SSID   "ThanksDanny-setup"     // open AP, alive only while there is no working network
#define CONNECT_TIMEOUT_MS 20000
#define MAX_SCAN 20

#define TAP_GUARD_MS  90                  // between taps; was 220, which ate quick taps
#define POLL_MS        2                  // was 20 ms, which alone added visible lag

// ------------------------------------------------------- round panel geometry
#define PANEL_R (LCD_WIDTH / 2)            // 233: the glass is a circle of this radius
#define SAFE_R  (PANEL_R - 25)             // keep interactive things inside this

// Cog button: up and to the right, but well INSIDE the circle. Sized up from 33 to 42 after the
// buttons proved too small to aim at on a 1.75" screen - on a round panel, bigger means moving it
// inward as well, or the boot check below rejects it.
#define COG_CX    340
#define COG_CY    120
#define COG_R     29                      // gear body radius; the disc behind it is COG_BTN_R
#define COG_BTN_R 42

// ------------------------------------------------------- home: the app carousel (v2.3)
// One big icon in the middle, its neighbours smaller at the sides so it is obvious there is more
// to swipe to, the app's name and the Wi-Fi state in a pill underneath, and page dots. Swipe left
// / right to move through the apps; tap the big icon to open it, tap a side icon to move to it.
// Slot geometry, from far left (-2, off the glass) to far right (+2). The centre icon (R 60) sits
// under the Sons of Sudo sign (its text ends at y~235, measured by tools/measure_wallpaper.py).
// Every visible slot is checked against the round mask at boot.
static const int SLOT_X[5] = { -40, 104, 233, 362, 506 };
static const int SLOT_Y[5] = { 318, 302, 290, 302, 318 };
static const int SLOT_R[5] = {  20,  36,  60,  36,  20 };
#define APP_COUNT   4
#define APP_PICTURES 0
#define APP_DECIDER  1
#define APP_RUMOURS  2
#define APP_SETTINGS 3
static const char *APP_NAME[APP_COUNT] = { "PICTURES", "DECIDER", "RUMOURS", "SETTINGS" };
#define DOTS_Y      360                  // page dots, between the icon and the pill
#define HPILL_X     118                  // home pill: app name + Wi-Fi line
#define HPILL_Y     372
#define HPILL_W     230
#define HPILL_H     46
#define SWIPE_PX    45                   // horizontal travel that makes a press a swipe
#define RELEASE_MS  35                   // no finger reported for this long = lifted (see pollTouch)
#define SWIPE_FRAMES 5                   // carousel slide, ~60 ms per frame
#define CAROUSEL_Y0 214                  // rows the carousel can touch: restored from the
#define CAROUSEL_Y1 424                  //   wallpaper each animation frame

// Bottom pill - the home status line and the "Back" buttons share it. Enlarged from 240x44 to
// 280x62 with the text at size 2, because at size 1 the Wi-Fi address could not be read on the
// real glass. 280 is as wide as fits: its corners stay inside the round mask at this height.
#define PILL_X 93
#define PILL_Y 320
#define PILL_W 280
#define PILL_H 62

// Settings rows, inset so their corners stay on the glass. Four rows now have to fit between
// y=120 and the bottom of the safe area, so they are shorter than they were back at three.
#define ROW_X 63
#define ROW_W 340
#define ROW_H 64
#define ROW_Y0 150
#define ROW_GAP 10
#define ROW_COUNT 2                       // v2.9: Wi-Fi setup + Close (Pictures and Touch test removed)

// Gallery.ino (the picture app) is concatenated AFTER this file, so its globals are not yet
// declared here. This file's JPEG callback needs to know when the gallery wants blocks packed
// into a PSRAM frame instead of drawn straight at the panel.
extern uint16_t *decDest;
extern int       decDestW;
bool jpegBlockTo(uint16_t *dest, int destW, int16_t x, int16_t y, uint16_t w, uint16_t h,
                 uint16_t *bitmap);

// ------------------------------------------------------------------ screens
#define ST_HOME         0
#define ST_MENU         1
#define ST_APINFO       2
// The gallery's screen ids live here rather than in Gallery.ino: Arduino concatenates this
// file FIRST, so anything this file references must be declared above it.
#define ST_GALLERY      4
#define ST_GALLERY_SYNC 5
#define ST_DECIDER      6               // Decider.ino: the coin toss
#define ST_RUMOURS      7               // Rumours.ino: a random rumour MP3
static int homeApp = APP_PICTURES;      // which app is in the middle of the carousel

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300  *panel = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Everything draws here, in PSRAM; flush() sends the whole frame to the panel in one go.
Arduino_Canvas  *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

// Send only part of the canvas to the glass (v2.6). A full flush is ~46 ms; a button highlight
// is a 140x140 patch, ~5 ms. The CO5300 wants its window on even columns/rows with even sizes,
// so the rectangle is widened to that. Big areas just use the full flush.
static uint16_t *flushTmp = nullptr;
#define FLUSH_TMP_PX ((long)LCD_WIDTH * LCD_HEIGHT * 6 / 10)
void flushRect(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
  if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
  if (w <= 0 || h <= 0) return;
  if (x & 1) { x--; w++; }
  if (y & 1) { y--; h++; }
  if (w & 1) w++;                       // 466 is even, so x even + w odd never reaches the edge
  if (h & 1) h++;
  if ((long)w * h > FLUSH_TMP_PX) { gfx->flush(); return; }
  if (!flushTmp) flushTmp = (uint16_t *)ps_malloc((size_t)FLUSH_TMP_PX * 2);
  if (!flushTmp) { gfx->flush(); return; }
  uint16_t *fb = gfx->getFramebuffer();
  for (int r = 0; r < h; r++)
    memcpy(flushTmp + (size_t)r * w, fb + (size_t)(y + r) * LCD_WIDTH + x, (size_t)w * 2);
  panel->draw16bitRGBBitmap(x, y, flushTmp, w, h);
}
TouchDrvCST92xx touch;
WebServer       server(80);
Preferences     prefs;

static int  screen       = ST_HOME;
static int  touchOK      = 0;
static bool touchDown    = false;
static uint32_t lastTap  = 0;
static uint32_t blocks   = 0;
static uint32_t touchSeen = 0;
static uint32_t connectStarted = 0;
static bool connecting   = false;
static bool apRunning    = false;
static String wifiSSID;                  // saved network name (safe to log)
static String apAddress  = "192.168.4.1";
static String scanList[MAX_SCAN];        // cached scan: see the note about AP mode above
static int    scanCount  = 0;

// Timings, logged per tap so responsiveness can be measured rather than argued about
// rather than argued about. Written every tap.
static uint32_t lastReadUs = 0;          // one controller read
static uint32_t lastActMs  = 0;          // tap detected -> response finished

// ------------------------------------------------------------------ drawing

// Is this point on the glass? The panel is round, so a corner coordinate draws "fine" and is
// then masked away - the bug that put the cog off-screen in the first version.
bool insidePanel(int x, int y) {
  float dx = x - LCD_WIDTH / 2.0f, dy = y - LCD_HEIGHT / 2.0f;
  return (dx * dx + dy * dy) <= ((float)PANEL_R - 4) * ((float)PANEL_R - 4);
}

bool rectInsidePanel(int x, int y, int w, int h) {
  return insidePanel(x, y) && insidePanel(x + w, y) &&
         insidePanel(x, y + h) && insidePanel(x + w, y + h);
}

// For round buttons: the true test is the furthest point of the drawn circle, not the corner
// of its bounding box. Testing the bounding box wrongly failed a button that is well inside.
bool circleInsidePanel(int cx, int cy, int r) {
  float dx = cx - LCD_WIDTH / 2.0f, dy = cy - LCD_HEIGHT / 2.0f;
  return (sqrtf(dx * dx + dy * dy) + r) <= ((float)PANEL_R - 4);
}

bool jpegBlock(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  blocks++;
  // When the gallery is decoding, blocks are packed into a PSRAM frame so the picture can be
  // scrolled afterwards; otherwise they go straight at the panel, as every other screen does.
  if (decDest) return jpegBlockTo(decDest, decDestW, x, y, w, h, bitmap);
  gfx->draw16bitRGBBitmap(x, y, bitmap, w, h);
  return 1;
}

void drawCog(int cx, int cy, int r, uint16_t body, uint16_t hole) {
  // A gear: eight teeth as quads around a hub, then a hole punched in the middle with the
  // panel's own colour so it reads as a cog rather than a blob.
  const int teeth = 8;
  for (int i = 0; i < teeth; i++) {
    float a  = (float)i * 2.0f * PI / teeth;
    float a1 = a - 0.16f, a2 = a + 0.16f;
    int x1 = cx + (int)(cosf(a1) * r),       y1 = cy + (int)(sinf(a1) * r);
    int x2 = cx + (int)(cosf(a2) * r),       y2 = cy + (int)(sinf(a2) * r);
    int x3 = cx + (int)(cosf(a2) * (r + 8)), y3 = cy + (int)(sinf(a2) * (r + 8));
    int x4 = cx + (int)(cosf(a1) * (r + 8)), y4 = cy + (int)(sinf(a1) * (r + 8));
    gfx->fillTriangle(x1, y1, x2, y2, x3, y3, body);
    gfx->fillTriangle(x1, y1, x3, y3, x4, y4, body);
  }
  gfx->fillCircle(cx, cy, r, body);
  gfx->fillCircle(cx, cy, r / 3, hole);
}

void button(int x, int y, int w, int h, uint16_t fill, uint16_t outline) {
  gfx->fillRoundRect(x, y, w, h, 12, fill);
  gfx->drawRoundRect(x, y, w, h, 12, outline);
}

void centred(int y, const char *s, uint8_t size, uint16_t colour) {
  int16_t w = strlen(s) * 6 * size;
  gfx->setTextSize(size);
  gfx->setTextColor(colour);
  gfx->setCursor((LCD_WIDTH - w) / 2, y);
  gfx->print(s);
}

// ------------------------------------------------------------------ screens

// One app icon: a dark disc with a ring, and the app's glyph scaled to the disc. `focus` is the
// big middle one; the side ones get a quieter ring.
void drawAppIcon(int app, int cx, int cy, int R, bool focus) {
  if (R < 6) return;
  uint16_t ring = focus ? RGB565(120, 200, 255) : RGB565(70, 96, 130);
  gfx->fillCircle(cx, cy, R, RGB565(12, 12, 18));
  gfx->drawCircle(cx, cy, R, ring);
  if (focus) gfx->drawCircle(cx, cy, R - 1, ring);
  uint16_t fg = focus ? RGB565(150, 230, 255) : RGB565(110, 170, 200);
  void drawDeciderIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg);   // Decider.ino
  void drawRumoursIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg);   // Rumours.ino
  switch (app) {
    case APP_PICTURES: drawPicturesIcon(cx, cy, (R * 76) / 100, fg, RGB565(12, 12, 18)); break;
    case APP_DECIDER:  drawDeciderIcon(cx, cy, (R * 72) / 100, fg, RGB565(12, 12, 18)); break;
    case APP_RUMOURS:  drawRumoursIcon(cx, cy, (R * 72) / 100, fg, RGB565(12, 12, 18)); break;
    case APP_SETTINGS: drawCog(cx, cy, (R * 52) / 100, fg, RGB565(12, 12, 18));        break;
  }
}

// Position of slot f (fractional, -2..2) by linear interpolation of the slot table.
void slotAt(float f, int *x, int *y, int *r) {
  if (f < -2) f = -2;
  if (f > 2) f = 2;
  int i = (int)floorf(f + 2.0f);
  if (i > 3) i = 3;
  float t = (f + 2.0f) - i;
  *x = (int)(SLOT_X[i] + (SLOT_X[i + 1] - SLOT_X[i]) * t);
  *y = (int)(SLOT_Y[i] + (SLOT_Y[i + 1] - SLOT_Y[i]) * t);
  *r = (int)(SLOT_R[i] + (SLOT_R[i + 1] - SLOT_R[i]) * t);
}

// The carousel at offset `p` (0 = settled; 0..1 sliding to the next app, 0..-1 to the previous),
// plus the dots and the pill. Draws into the canvas only.
void drawCarousel(float p) {
  // Far ones first, so the middle icon is drawn on top of anything it overlaps.
  const int order[5] = { -2, 2, -1, 1, 0 };
  for (int n = 0; n < 5; n++) {
    int k = order[n];
    int app = ((homeApp + k) % APP_COUNT + APP_COUNT) % APP_COUNT;
    int x, y, r;
    slotAt(k - p, &x, &y, &r);
    bool focus = fabsf(k - p) < 0.5f;
    drawAppIcon(app, x, y, r, focus);
  }
  // Page dots: which of the apps is in the middle.
  int shown = ((homeApp + (int)roundf(p)) % APP_COUNT + APP_COUNT) % APP_COUNT;
  for (int i = 0; i < APP_COUNT; i++) {
    int dx = 233 + (i - (APP_COUNT - 1) / 2.0f) * 16;
    if (i == shown) gfx->fillCircle(dx, DOTS_Y, 4, RGB565(150, 230, 255));
    else { gfx->fillCircle(dx, DOTS_Y, 3, RGB565(12, 12, 18)); gfx->drawCircle(dx, DOTS_Y, 3, RGB565(110, 140, 170)); }
  }
  // Name pill: the app in the middle, and the network state on a second line.
  gfx->fillRoundRect(HPILL_X, HPILL_Y, HPILL_W, HPILL_H, 14, RGB565(10, 10, 14));
  gfx->drawRoundRect(HPILL_X, HPILL_Y, HPILL_W, HPILL_H, 14, RGB565(70, 70, 86));
  centred(HPILL_Y + 7, APP_NAME[shown], 2, RGB565_WHITE);
  char net[40];
  uint16_t nc;
  if (WiFi.status() == WL_CONNECTED) { snprintf(net, sizeof(net), "wifi %s", WiFi.localIP().toString().c_str()); nc = RGB565(150, 240, 160); }
  else if (apRunning)                { snprintf(net, sizeof(net), "setup: join %s", AP_SSID); nc = RGB565(150, 220, 255); }
  else if (connecting)               { snprintf(net, sizeof(net), "wifi: connecting..."); nc = RGB565(255, 220, 120); }
  else                               { snprintf(net, sizeof(net), "wifi: not connected"); nc = RGB565(255, 200, 120); }
  centred(HPILL_Y + 29, net, 1, nc);
}

void drawHome() {
  // The decoded wallpaper is cached in PSRAM and copied into the canvas (a memcpy, a few ms).
  // The cache, and the fallback if it fails, both live inside wallpaperBlit() (Gallery.ino).
  gfx->fillScreen(RGB565_BLACK);
  void wallpaperBlit();
  wallpaperBlit();
  drawCarousel(0);
  Serial.printf("ui     : home - carousel on %s (swipe for more)\n", APP_NAME[homeApp]);
}

// Slide the carousel one app left (dir +1) or right (dir -1). Each frame restores only the band
// of wallpaper the carousel covers, redraws, and flushes.
void homeSlide(int dir) {
  void wallpaperRows(int y0, int y1);            // Gallery.ino
  uint32_t t0 = millis();
  for (int f = 1; f <= SWIPE_FRAMES; f++) {
    float t = (float)f / SWIPE_FRAMES;
    float e = 1.0f - (1.0f - t) * (1.0f - t);     // ease out: quick start, gentle stop
    wallpaperRows(CAROUSEL_Y0, CAROUSEL_Y1);
    if (f < SWIPE_FRAMES) drawCarousel(dir * e);
    else { homeApp = ((homeApp + dir) % APP_COUNT + APP_COUNT) % APP_COUNT; drawCarousel(0); }
    flushRect(0, CAROUSEL_Y0, LCD_WIDTH, CAROUSEL_Y1 - CAROUSEL_Y0 + 1);   // the band only
  }
  Serial.printf("ui     : carousel -> %s (%lu ms)\n", APP_NAME[homeApp], (unsigned long)(millis() - t0));
}

void openApp(int app) {
  Serial.printf("         -> opening %s\n", APP_NAME[app]);
  switch (app) {
    case APP_PICTURES:
      // The first JPEG decode takes ~300 ms: show the press first.
      gfx->drawCircle(SLOT_X[2], SLOT_Y[2], SLOT_R[2] + 5, RGB565_WHITE);
      gfx->drawCircle(SLOT_X[2], SLOT_Y[2], SLOT_R[2] + 6, RGB565(120, 200, 255));
      flushRect(SLOT_X[2] - SLOT_R[2] - 8, SLOT_Y[2] - SLOT_R[2] - 8, 2 * SLOT_R[2] + 16, 2 * SLOT_R[2] + 16);
      showScreen(ST_GALLERY);
      break;
    case APP_DECIDER:  showScreen(ST_DECIDER); break;
    case APP_RUMOURS:  showScreen(ST_RUMOURS); break;
    case APP_SETTINGS: showScreen(ST_MENU);    break;
  }
}

// Which carousel slot (1 left, 2 middle, 3 right) a point is on, or 0.
int homeSlotAt(int x, int y) {
  for (int i = 1; i <= 3; i++) {
    int dx = x - SLOT_X[i], dy = y - SLOT_Y[i], r = SLOT_R[i] + (i == 2 ? 18 : 16);
    if (dx * dx + dy * dy <= r * r) return i;
  }
  return 0;
}

// The moment a finger lands on a home icon it lights up - a ring and a brighter disc edge,
// flushed as a small patch (~5 ms). The action itself waits for the lift, because the same press
// may still turn into a swipe; the highlight is what makes that wait feel like nothing.
static int homePressed = 0;
void homePressFeedback(int x, int y) {
  int i = homeSlotAt(x, y);
  if (!i) return;
  homePressed = i;
  int r = SLOT_R[i];
  gfx->drawCircle(SLOT_X[i], SLOT_Y[i], r + 4, RGB565_WHITE);
  gfx->drawCircle(SLOT_X[i], SLOT_Y[i], r + 5, RGB565_WHITE);
  gfx->drawCircle(SLOT_X[i], SLOT_Y[i], r + 6, RGB565(120, 200, 255));
  flushRect(SLOT_X[i] - r - 8, SLOT_Y[i] - r - 8, 2 * r + 16, 2 * r + 16);
}

// Take the highlight off again (a press that became a drag): redraw the carousel band.
void homePressClear() {
  if (!homePressed) return;
  homePressed = 0;
  void wallpaperRows(int y0, int y1);
  wallpaperRows(CAROUSEL_Y0, CAROUSEL_Y1);
  drawCarousel(0);
  flushRect(0, CAROUSEL_Y0, LCD_WIDTH, CAROUSEL_Y1 - CAROUSEL_Y0 + 1);
}

// A horizontal swipe. Only the home screen uses it for now.
void handleSwipe(int dir) {
  Serial.printf("swipe  : %s on screen %d\n", dir > 0 ? "left (next)" : "right (previous)", screen);
  if (screen == ST_HOME) { homePressed = 0; homeSlide(dir); }   // the slide redraws the band
}

void drawMenu() {
  gfx->fillScreen(RGB565(12, 12, 16));
  centred(26, "Settings", 3, RGB565_WHITE);
  gfx->drawFastHLine(ROW_X, 78, ROW_W, RGB565(60, 60, 72));

  // v2.9: only settings live here. Pictures is on the home carousel; the touch test is gone.
  int y = ROW_Y0;
  button(ROW_X, y, ROW_W, ROW_H, RGB565(28, 28, 36), RGB565(80, 80, 96));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(ROW_X + 18, y + 12);
  gfx->print("Wi-Fi setup on phone");
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565(150, 200, 255));
  gfx->setCursor(ROW_X + 18, y + 38);
  if (WiFi.status() == WL_CONNECTED)      gfx->printf("connected: %s", WiFi.localIP().toString().c_str());
  else if (wifiSSID.length())             gfx->printf("saved: %s (not connected)", wifiSSID.c_str());
  else                                    gfx->print("no network saved yet - pictures need this");

  y += ROW_H + ROW_GAP;
  button(ROW_X, y, ROW_W, ROW_H, RGB565(28, 28, 36), RGB565(80, 80, 96));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(ROW_X + 18, y + 24);
  gfx->print("Close");

  Serial.printf("ui     : settings (%d rows, last ends at y=%d)\n", ROW_COUNT, y + ROW_H);
}

void drawAPInfo() {
  gfx->fillScreen(RGB565(10, 14, 20));
  centred(30, "Wi-Fi setup", 3, RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565(150, 220, 255));
  gfx->setCursor(92, 116);
  gfx->print("1. On the phone, join");
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(92, 152);
  gfx->print(AP_SSID);
  gfx->setTextColor(RGB565(150, 220, 255));
  gfx->setCursor(92, 210);
  gfx->print("2. Then open");
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(92, 246);
  gfx->print(apAddress);
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565(160, 160, 175));
  gfx->setCursor(92, 298);
  gfx->print("Password is typed on the");
  gfx->setCursor(92, 312);
  gfx->print("phone - never shown or");
  gfx->setCursor(92, 326);
  gfx->print("logged on the badge.");
  gfx->setCursor(92, 344);
  gfx->printf("networks seen: %d", scanCount);

  button(PILL_X, PILL_Y, PILL_W, PILL_H, RGB565(28, 28, 36), RGB565(80, 80, 96));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(PILL_X + 16, PILL_Y + 14);
  gfx->print("Back");

  Serial.printf("ui     : ap info (ap=%s http://%s, %d network(s) cached)\n",
                AP_SSID, apAddress.c_str(), scanCount);
}

void showScreen(int s) {
  void rumoursStop();                          // Rumours.ino
  if (s != ST_RUMOURS) rumoursStop();          // leaving Rumours silences it
  screen = s;
  uint32_t t0 = millis();
  switch (s) {
    case ST_HOME:         drawHome();         break;
    case ST_MENU:         drawMenu();         break;
    case ST_APINFO:       drawAPInfo();       break;
    case ST_GALLERY:      showGallery();      return;  // Gallery.ino; flushes its own frames
    case ST_GALLERY_SYNC: drawGallerySync();  return;  // flushes itself (and not when quiet)
    case ST_DECIDER:      showDecider();      return;  // Decider.ino; animates, flushes itself
    case ST_RUMOURS:      showRumours();      return;  // Rumours.ino; plays in the background
  }
  uint32_t t1 = millis();
  gfx->flush();                                // the screen was composed in RAM; send it once
  Serial.printf("ui     : screen %d composed %lu ms + flush %lu ms\n", s,
                (unsigned long)(t1 - t0), (unsigned long)(millis() - t1));
}

// Instant acknowledgement, drawn before any slow work. A full-screen repaint on this panel
// takes ~350 ms; without this the button feels dead for that whole time.
// The pictures button's glyph: a little framed photo - frame, sun, and a mountain, which reads
// as "pictures" at 48 pixels across where text would not.
void drawPicturesIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  int w = r * 2 - 8;
  int h = w * 72 / 100;
  int x = cx - w / 2, y = cy - h / 2;
  gfx->fillRoundRect(x, y, w, h, 3, bg);
  gfx->drawRoundRect(x, y, w, h, 3, fg);
  gfx->fillCircle(x + w / 4, y + h / 3, 3, fg);                 // sun
  gfx->fillTriangle(x + 3, y + h - 3, x + w / 2, y + h / 3 + 2, x + w - 3, y + h - 3, fg);
  gfx->fillTriangle(x + w / 2 - 3, y + h - 3, x + w * 3 / 4, y + h / 2 + 2, x + w - 3, y + h - 3, bg);
}

void ackRow(int y) {
  gfx->drawRoundRect(ROW_X - 3, y - 3, ROW_W + 6, ROW_H + 6, 14, RGB565(150, 230, 255));
}

// ------------------------------------------------------------------ wi-fi

void loadCredentials() {
  prefs.begin("wifi", true);
  wifiSSID = prefs.getString("ssid", "");
  prefs.end();
  Serial.printf("wifi   : saved network: %s\n", wifiSSID.length() ? wifiSSID.c_str() : "(none)");
}

void scanAndCache() {
  // In AP+STA this scans without dropping the AP client; in pure AP mode it would.
  WiFi.mode(WIFI_AP_STA);
  scanCount = 0;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n && scanCount < MAX_SCAN; i++) scanList[scanCount++] = WiFi.SSID(i);
  WiFi.scanDelete();
  Serial.printf("wifi   : scan found %d network(s)\n", scanCount);
}

void beginConnect() {
  if (!wifiSSID.length()) return;
  prefs.begin("wifi", true);
  String pass = prefs.getString("pass", "");
  prefs.end();
  WiFi.mode(WIFI_AP_STA);           // keep the setup page reachable if this fails
  WiFi.begin(wifiSSID.c_str(), pass.c_str());
  connecting = true;
  connectStarted = millis();
  Serial.printf("wifi   : connecting to '%s' (password %u chars, not logged)\n",
                wifiSSID.c_str(), (unsigned)pass.length());
}

String buildPage() {
  String page = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                  "<title>Thanks Danny</title>"
                  "<body style='font-family:system-ui;background:#0e0e12;color:#eee;margin:0;padding:26px'>"
                  "<h2 style='margin:0 0 6px'>Thanks Danny</h2>"
                  "<p style='color:#9ab'>Pick your Wi-Fi, type its password, and the badge will "
                  "save it and connect. Nothing is sent anywhere else.</p>"
                  "<form method='POST' action='/save'>"
                  "<p><select name='ssid' style='width:100%;padding:14px;font-size:16px;border-radius:8px'>");
  if (scanCount <= 0) {
    page += F("<option value=''>no networks seen yet - tap Rescan networks below</option>");
  }
  for (int i = 0; i < scanCount; i++) {
    page += "<option value='" + scanList[i] + "'>" + scanList[i] + "</option>";
  }
  page += F("</select></p>"
            "<p><input name='pass' type='password' placeholder='Wi-Fi password' autocomplete='off' "
            "style='width:100%;padding:14px;font-size:16px;border-radius:8px;box-sizing:border-box'></p>"
            "<p><button style='padding:14px 22px;font-size:16px;border-radius:8px'>Save and connect</button></p>"
            "</form>");
  // The badge scans once at boot so a tap never waits ~3.4 s for a scan. That means a network
  // that appeared since boot is missing from the list, so rescuing that case is a phone-side
  // action: this link rescans on demand without putting the delay back on the touch path.
  page += "<p style='font-size:15px'><a href='/rescan' style='color:#8cf'>Rescan networks</a>"
          "<span style='color:#667;font-size:13px'> - " + String(scanCount) +
          " in the list; rescan if yours is missing</span></p>";
  page += F("<p style='color:#667;font-size:13px'>Leave the password empty for an open network. "
            "The badge does not display or log it.</p></body>");
  return page;
}

void handleRoot() {
  server.send(200, "text/html", buildPage());
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  if (!ssid.length()) {
    server.send(200, "text/html", F("<body style='font-family:system-ui;background:#0e0e12;color:#eee;padding:26px'>"
                                    "<p>No network was selected.</p><p><a style='color:#8cf' href='/'>Back</a></p></body>"));
    return;
  }
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  wifiSSID = ssid;
  // Only the name and the length are logged: never the password itself.
  Serial.printf("wifi   : saved '%s', password %u chars (not logged)\n", ssid.c_str(), (unsigned)pass.length());

  server.send(200, "text/html",
    F("<body style='font-family:system-ui;background:#0e0e12;color:#eee;padding:26px'>"
      "<h2>Saved</h2><p>The badge is connecting now - watch its screen.</p>"
      "<p style='color:#9ab'>If it fails, this setup network stays up: reload this page and check "
      "the password.</p></body>"));
  beginConnect();                       // after the reply, so the page is not cut short
}

void handleRescan() {
  // On-demand rescan from the phone. It costs ~3.4 s, which is exactly why it lives here
  // rather than on the badge's touch path. Scanning in AP+STA keeps the phone connected.
  Serial.println("wifi   : rescan requested from the setup page");
  scanAndCache();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void startSetupAP() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);                 // open, and only while there is no working network
  apAddress = WiFi.softAPIP().toString();
  if (!apRunning) {
    server.on("/", handleRoot);
    server.on("/rescan", handleRescan);
    server.on("/save", HTTP_POST, handleSave);
    server.onNotFound([]() { server.send(200, "text/html", buildPage()); });
    server.begin();
    apRunning = true;
  }
  Serial.printf("ap     : '%s' up, setup page at http://%s\n", AP_SSID, apAddress.c_str());
}

// ------------------------------------------------------------------ touch

void handleTap(int16_t x, int16_t y) {
  touchSeen++;
  uint32_t t0 = millis();
  Serial.printf("tap    : x=%d y=%d on screen %d\n", x, y, screen);

  // Acknowledge before working. The screen change itself can take ~350 ms on this panel.
  if (screen == ST_MENU && x >= ROW_X && x <= ROW_X + ROW_W) {
    int y1 = ROW_Y0, y2 = ROW_Y0 + ROW_H + ROW_GAP;
    if (y >= y1 && y <= y1 + ROW_H)       ackRow(y1);
    else if (y >= y2 && y <= y2 + ROW_H)  ackRow(y2);
  } else if (screen == ST_APINFO) {
    if (x >= PILL_X && x <= PILL_X + PILL_W && y >= PILL_Y && y <= PILL_Y + PILL_H)
      gfx->drawRoundRect(PILL_X - 3, PILL_Y - 3, PILL_W + 6, PILL_H + 6, 16, RGB565(150, 230, 255));
  }

  if (screen == ST_HOME) {
    // Generous targets: the middle icon opens, a side icon brings that app to the middle.
    int cx = x - SLOT_X[2], cy = y - SLOT_Y[2];
    int lx = x - SLOT_X[1], ly = y - SLOT_Y[1];
    int rx = x - SLOT_X[3], ry = y - SLOT_Y[3];
    if (cx * cx + cy * cy <= (SLOT_R[2] + 18) * (SLOT_R[2] + 18))      openApp(homeApp);
    else if (lx * lx + ly * ly <= (SLOT_R[1] + 16) * (SLOT_R[1] + 16)) homeSlide(-1);
    else if (rx * rx + ry * ry <= (SLOT_R[3] + 16) * (SLOT_R[3] + 16)) homeSlide(+1);
    else Serial.println("         -> (home: not on an icon - swipe left/right to change app)");
    lastActMs = millis() - t0;
    return;
  }

  if (screen == ST_MENU) {
    int y1 = ROW_Y0;
    int y2 = y1 + ROW_H + ROW_GAP;
    if (x >= ROW_X && x <= ROW_X + ROW_W) {
      if (y >= y1 && y <= y1 + ROW_H) {
        // Use the scan cached at boot. Rescanning here measured 3363 ms on the tap path -
        // the single worst delay in the UI - and the list only changes when networks do.
        // Say something immediately anyway: an acked tap must never look ignored.
        if (scanCount == 0) {
          centred(392, "scanning for networks...", 1, RGB565(255, 220, 120));
          gfx->flush();                         // the scan takes seconds: say so first
          scanAndCache();
        }
        Serial.println("         -> menu: wi-fi setup (cached scan, then raise the AP)");
        startSetupAP();
        showScreen(ST_APINFO);
      } else if (y >= y2 && y <= y2 + ROW_H) {
        Serial.println("         -> menu: close");      showScreen(ST_HOME);
      }
    }
    lastActMs = millis() - t0;
    return;
  }

  if (screen == ST_GALLERY_SYNC) {
    Serial.println("         -> gallery sync: tap to continue");
    gallerySyncTap();
    lastActMs = millis() - t0;
    return;
  }

  if (screen == ST_GALLERY) {
    // The gallery runs its own loop and consumes taps while it scrolls. Reaching here means it
    // is parked on the "nothing cached" screen, where a tap means "go and fetch them".
    extern int picCount;                      // Gallery.ino, same translation unit
    if (picCount == 0) {
      Serial.println("         -> gallery: fetching pictures from GitHub");
      syncPictures();
    }
    lastActMs = millis() - t0;
    return;
  }

  if (screen == ST_RUMOURS) {
    void rumoursTap(int x, int y);            // Rumours.ino
    rumoursTap(x, y);
    lastActMs = millis() - t0;
    return;
  }

  if (screen == ST_DECIDER) {
    void deciderTap(int x, int y);            // Decider.ino
    deciderTap(x, y);
    lastActMs = millis() - t0;
    return;
  }

  if (screen == ST_APINFO) {
    if (x >= PILL_X && x <= PILL_X + PILL_W && y >= PILL_Y && y <= PILL_Y + PILL_H) {
      Serial.println("         -> back"); showScreen(ST_MENU);
    }
    lastActMs = millis() - t0;
    return;
  }

  lastActMs = millis() - t0;
}

// ------------------------------------------------------------------ hardware buttons

// The BOOT button is a plain **GPIO0** on this board - Waveshare's own repo lists a "Button Test
// with direct PWR/EXIO4 and BOOT/GPIO0 level diagnostics". PWR is deliberately not used: it goes
// through the AXP2101 PMIC, where a long press is power control in hardware.
//
// Presses are caught by an interrupt rather than polled, and the flag is only cleared when some
// code path consumes it. That matters here: the gallery blocks for up to a second drawing a
// transition, and the gallery also writes flash - a poll-only button would drop presses in those
// windows, and an ISR that is not IRAM_ATTR would fault during a flash write.
#define BTN_BOOT 0

volatile bool     btnPressed  = false;    // set by the ISR, consumed by bootButtonHit()
static volatile uint32_t btnLastIrq = 0;

void IRAM_ATTR onBootPress() {
  uint32_t now = millis();
  if (now - btnLastIrq > 30) { btnLastIrq = now; btnPressed = true; }   // light debounce
}

// True once per press. Self-arming, so it needs no line in setup().
bool bootButtonHit() {
  static bool armed = false;
  if (!armed) {
    pinMode(BTN_BOOT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BTN_BOOT), onBootPress, FALLING);
    armed = true;
    Serial.printf("btn    : BOOT (GPIO0) armed, reads %s - press it for the main screen\n",
                  digitalRead(BTN_BOOT) ? "high (released)" : "LOW (held down)");
  }
  if (!btnPressed) return false;
  btnPressed = false;
  return true;
}

// Taps and swipes (v2.6).
//
// HOME (the carousel): a press becomes a SWIPE as soon as it has moved SWIPE_PX sideways (and
// more sideways than up/down) - that fires mid-gesture. A press lifted without becoming a swipe
// is a TAP at the point where it went down. The icon under the finger lights up the instant it
// lands (homePressFeedback), so the wait for the lift is not felt.
//
// EVERY OTHER SCREEN has no swipe, so a tap fires on the PRESS, the instant the finger lands -
// as it did before the carousel. That is most of the "sluggish" fix: v2.3 made every tap wait
// for the lift, everywhere.
//
// LIFT is time-based: no finger reported for RELEASE_MS. The CST9217 driver returns "no points"
// for any read that falls between two of the controller's own reports (it answers ACK when it
// has nothing new - checked in SensorLib's TouchDrvCST92xx.cpp), so at a 2 ms poll a finger that
// is still down reads as "no finger" several times in a row. v2.3 counted 3 empty reads (~9 ms)
// as a lift, which could split one press in two.
static int16_t tStartX = 0, tStartY = 0, tLastX = 0, tLastY = 0;
static bool    tSwiped = false;              // this press is used up (swiped, or tapped on press)
static uint32_t tLastSeen = 0, tDownAt = 0;

void pollTouch() {
  if (!touchOK) return;
  int16_t xs[1], ys[1];
  uint32_t r0 = micros();
  uint8_t n = touch.getPoint(xs, ys, 1);
  lastReadUs = micros() - r0;
  uint32_t now = millis();

  if (n > 0) {
    tLastSeen = now;
    if (!touchDown) {
      touchDown = true;
      tDownAt = now;
      tStartX = tLastX = xs[0];
      tStartY = tLastY = ys[0];
      tSwiped = false;
      if (screen == ST_HOME) {
        homePressFeedback(tStartX, tStartY);
      } else if (now - lastTap > TAP_GUARD_MS) {
        tSwiped = true;                      // consumed: nothing more happens on the lift
        lastTap = now;
        uint32_t a0 = micros();
        handleTap(tStartX, tStartY);
        Serial.printf("         -> responded in %lu ms on press (read %lu us)\n", (micros() - a0) / 1000, lastReadUs);
      }
      return;
    }
    tLastX = xs[0];
    tLastY = ys[0];
    int dx = tLastX - tStartX, dy = tLastY - tStartY;
    if (!tSwiped && screen == ST_HOME && abs(dx) >= SWIPE_PX && abs(dx) > abs(dy) * 13 / 10) {
      tSwiped = true;
      handleSwipe(dx < 0 ? +1 : -1);         // finger moving left = next app
    }
    return;
  }

  if (!touchDown || now - tLastSeen < RELEASE_MS) return;
  touchDown = false;
  if (tSwiped) return;
  int mx = tLastX - tStartX, my = tLastY - tStartY;
  if (mx * mx + my * my > 30 * 30) {         // wandered too far for a tap, too little for a swipe
    Serial.printf("touch  : drag of %d,%d ignored\n", mx, my);
    homePressClear();
    return;
  }
  lastTap = now;
  uint32_t a0 = micros();
  homePressed = 0;
  handleTap(tStartX, tStartY);
  Serial.printf("         -> responded in %lu ms after a %lu ms press (read %lu us)\n",
                (micros() - a0) / 1000, (unsigned long)(tLastSeen - tDownAt), lastReadUs);
}

// ------------------------------------------------------------------ setup/loop

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Thanks Danny badge v2.9 (" __DATE__ " " __TIME__ "): app carousel - pictures, decider, rumours, settings ===");

  // Canvas begin() starts the panel at LCD_QSPI_HZ and allocates the 434 kB frame in PSRAM.
  if (!gfx->begin(LCD_QSPI_HZ)) { Serial.println("! gfx->begin FAILED (panel or PSRAM canvas)"); return; }
  gfx->fillScreen(RGB565_BLACK);
  uint32_t f0 = millis();
  gfx->flush();
  uint32_t flushMs = millis() - f0;
  panel->setBrightness(170);                   // the panel's own command, not the canvas's
  Serial.printf("panel  : CO5300 up, %dx%d round (visible circle r=%d), QSPI %d MHz, full frame %lu ms\n",
                LCD_WIDTH, LCD_HEIGHT, PANEL_R, LCD_QSPI_HZ / 1000000, (unsigned long)flushMs);

  // Check the layout against the round mask at boot. This is the check that would have caught
  // the cog being drawn in a masked corner, where it draws without error and is never seen.
  // The cog is still used by the gallery; the carousel's three visible slots and its pill are new.
  bool cogOK  = true;                        // v2.7: no cog is drawn anywhere any more
  bool appsOK = true;
  for (int i = 1; i <= 3; i++) appsOK = appsOK && circleInsidePanel(SLOT_X[i], SLOT_Y[i], SLOT_R[i] + 6);
  bool hpillOK = rectInsidePanel(HPILL_X, HPILL_Y, HPILL_W, HPILL_H);
  bool pillOK = rectInsidePanel(PILL_X, PILL_Y, PILL_W, PILL_H) && hpillOK;
  bool rowsOK = rectInsidePanel(ROW_X, ROW_Y0, ROW_W, ROW_H) &&
                rectInsidePanel(ROW_X, ROW_Y0 + (ROW_COUNT - 1) * (ROW_H + ROW_GAP), ROW_W, ROW_H);
  Serial.printf("layout : app carousel %s | pills %s | rows %s\n",
                appsOK ? "yes" : "NO",
                pillOK ? "yes" : "NO", rowsOK ? "yes" : "NO");
  if (!cogOK || !appsOK || !pillOK || !rowsOK) Serial.println("! a control sits outside the round aperture");

  if (!LittleFS.begin(false)) {
    Serial.println("! LittleFS mount FAILED - is the filesystem image flashed at 0x910000?");
    return;
  }
  Serial.printf("fs     : LittleFS mounted, %u kB used of %u kB\n",
                (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  TJpgDec.setCallback(jpegBlock);

  Wire.begin(TOUCH_SDA, TOUCH_SCL, TOUCH_HZ);   // 400 kHz: reads are ~2 ms here
  touch.setPins(TOUCH_RST, TOUCH_INT);
  touchOK = touch.begin(Wire, TOUCH_ADDR, TOUCH_SDA, TOUCH_SCL) ? 1 : 0;
  if (touchOK) {
    Serial.printf("touch  : %s at 0x%02X\n", touch.getModelName(), TOUCH_ADDR);
    // No sleep() here: it is part of the vendor's state-clearing dance, and leaving the
    // controller asleep reports zero points forever - which looks exactly like a dead touch
    // panel. reset() alone gives a known-good starting state.
    touch.reset();
    delay(50);
    touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
    touch.setMirrorXY(true, true);      // Waveshare's own setting for this panel
    Serial.printf("touch  : range %dx%d, mirrorX %d mirrorY %d, bus %d kHz\n",
                  touch.getResolutionX(), touch.getResolutionY(),
                  (int)touch.isMirrorX(), (int)touch.isMirrorY(), TOUCH_HZ / 1000);
  } else {
    Serial.println("! touch init FAILED - the cog will not respond");
  }

  loadCredentials();
  if (wifiSSID.length()) {
    beginConnect();                     // connect first; the AP only comes up if this fails
  } else {
    Serial.println("wifi   : nothing saved, so raising the setup network");
    scanAndCache();
    startSetupAP();
  }

  showScreen(ST_HOME);
  Serial.printf("touch  : polled every %d ms, tap guard %d ms\n", POLL_MS, TAP_GUARD_MS);
  Serial.println("ready");
}

void loop() {
  server.handleClient();               // only does anything while the setup AP is up
  pollTouch();                         // cheap: ~2 ms per read, so it runs at ~200 Hz

  // The BOOT button is the way back to the main screen from anywhere: one press, always the
  // same destination, no hunting for a control on a round panel.
  if (bootButtonHit() && screen != ST_HOME) {
    Serial.println("btn    : BOOT pressed -> main screen");
    showScreen(ST_HOME);
  }

  if (connecting) {
    if (WiFi.status() == WL_CONNECTED) {
      connecting = false;
      Serial.printf("wifi   : connected to '%s', ip %s\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
      // TLS needs a sane clock. This board has no RTC, so without SNTP it thinks it is 1970 and
      // every certificate in the CA bundle looks "not yet valid" - which shows up as a download
      // that simply never happens. Ask for the correct time once per boot.
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      Serial.println("time   : asked SNTP for the correct time");
      Serial.println("ap     : tearing down the setup network");
      WiFi.softAPdisconnect(true);
      apRunning = false;
      // Fetch pictures now that we are online, so the app has content before it is opened and
      // new pictures appear after a reboot. It also exercises the download path at every boot
      // without needing a tap, which is how it gets tested.
      void galleryBootFetch();          // Gallery.ino, later in the same translation unit
      delay(500);                       // let DNS and the route settle before the first handshake
      galleryBootFetch();
      void rumoursBootFetch();          // Rumours.ino: the rumour clips, quietly
      rumoursBootFetch();
      // Redraw the home screen so its Wi-Fi line shows the address (a redraw is ~80 ms now), and
      // come back to it if a first-boot sync took the screen over.
      if (screen == ST_HOME || screen == ST_GALLERY_SYNC) showScreen(ST_HOME);
    } else if (millis() - connectStarted > CONNECT_TIMEOUT_MS) {
      connecting = false;
      Serial.println("wifi   : could not connect - the setup network stays up");
      startSetupAP();
      if (screen == ST_APINFO) showScreen(ST_APINFO);
    }
  }
  if (screen == ST_RUMOURS) { void rumoursTick(); rumoursTick(); }   // the sound bars
  delay(POLL_MS);
}
