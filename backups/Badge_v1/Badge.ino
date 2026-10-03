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
 * This panel is SLOW to fill: measured ~0.6 Mpixel/s, so a full-screen repaint costs roughly
 * 350 ms and the wallpaper (a JPEG decode plus 900 block blits) about 390 ms. That is the
 * floor, and it is why a tap acknowledges INSTEAD of redrawing: the response is drawn
 * immediately, and the expensive screen change lands a third of a second later. Do not add
 * full-screen redraws to a tap path - it reads as a laggy button rather than a slow screen.
 *
 * Touch: the controller is read with getPoint() (about 2 ms on a 400 kHz bus). It is polled
 * every couple of milliseconds, and a tap fires on the press edge with a 90 ms guard. Two
 * traps cost real time here: leaving the controller asleep (touch.sleep()) reports zero points
 * forever and looks like a dead panel, and a long poll delay makes a working panel feel broken.
 *
 * What it does
 *   Home screen   : the wallpaper, a cog wheel button inside the circle upper right, and a
 *                   status pill inside the bottom of the circle
 *   Cog -> menu   : "Wi-Fi setup on phone", "Touch test", "Close"
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

// Pictures button: centred UNDER the Sons of Sudo sign, which is where the user asked for it.
// Measured off the wallpaper rather than eyeballed (tools/measure_wallpaper.py): the sign's
// bright text runs to y~235 and the status pill starts at 320, so a 42-radius disc centred at
// 275 fills the dark gap between them without covering either.
#define PIC_CX    233
#define PIC_CY    275
#define PIC_R     32                      // the photo glyph inside the disc
#define PIC_BTN_R 42

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
#define ROW_H 57
#define ROW_Y0 120
#define ROW_GAP 8
#define ROW_COUNT 4

// Gallery.ino (the picture app) is concatenated AFTER this file, so its globals are not yet
// declared here. This file's JPEG callback needs to know when the gallery wants blocks packed
// into a PSRAM frame instead of drawn straight at the panel.
extern uint16_t *decDest;
extern int       decDestW;
bool jpegBlockTo(uint16_t *dest, int destW, int16_t x, int16_t y, uint16_t w, uint16_t h,
                 uint16_t *bitmap);

// Touch test targets, comfortably inside the circle.
static const int MARK[4][2] = {{120, 130}, {346, 130}, {120, 336}, {346, 336}};

// ------------------------------------------------------------------ screens
#define ST_HOME         0
#define ST_MENU         1
#define ST_APINFO       2
#define ST_TOUCHTEST    3
// The gallery's screen ids live here rather than in Gallery.ino: Arduino concatenates this
// file FIRST, so anything this file references must be declared above it.
#define ST_GALLERY      4
#define ST_GALLERY_SYNC 5

Arduino_DataBus *bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300  *gfx = new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
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

// Timings, shown on the touch test screen so responsiveness can be measured on the glass
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

void drawHome() {
  uint32_t t0 = millis();
  gfx->fillScreen(RGB565_BLACK);

  // A held, static background: nothing redraws under the menu later, so no flicker. On a
  // round panel the corners of this photo are masked off, which is why the crop keeps its
  // subject central.
  // One full-frame blit from the PSRAM cache (56 ms measured) instead of the 900 per-block JPEG
  // draw (~390 ms). The cache, and the fallback if it fails, both live inside wallpaperBlit().
  void wallpaperBlit();                      // Gallery.ino, later in the same translation unit
  wallpaperBlit();

  // The cog sits on its own dark disc with a ring, so it reads as a button against any
  // artwork. Position is inside the circle - see the note at the top of this file.
  gfx->fillCircle(COG_CX, COG_CY, COG_BTN_R, RGB565(12, 12, 18));
  gfx->drawCircle(COG_CX, COG_CY, COG_BTN_R, RGB565(120, 200, 255));
  drawCog(COG_CX, COG_CY, COG_R, RGB565(150, 230, 255), RGB565(12, 12, 18));

  // Pictures gets its own button on the home screen, mirroring the cog on the other side: one
  // control for what the badge shows, one for how it is set up.
  gfx->fillCircle(PIC_CX, PIC_CY, PIC_BTN_R, RGB565(12, 12, 18));
  gfx->drawCircle(PIC_CX, PIC_CY, PIC_BTN_R, RGB565(120, 200, 255));
  drawPicturesIcon(PIC_CX, PIC_CY, PIC_R, RGB565(150, 230, 255), RGB565(12, 12, 18));

  // Status pill, centred near the bottom so it stays on the glass.
  gfx->fillRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, 12, RGB565(10, 10, 14));
  gfx->drawRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, 12, RGB565(70, 70, 86));
  // Size 2, not 1: at size 1 the address was unreadable on the real glass. Each line is kept
  // under 21 characters so it fits the 280-wide pill at this size.
  // Labels are UPPERCASE on purpose: the 5x7 classic font's lowercase glyphs are thin and sit
  // in the same 5-pixel cell, so they read much worse at this size. Anything case-sensitive
  // (the SSID, the IP) is left exactly as it is.
  gfx->setTextSize(2);
  if (WiFi.status() == WL_CONNECTED) {
    gfx->setTextColor(RGB565(150, 240, 160));
    gfx->setCursor(PILL_X + 12, PILL_Y + 10);
    gfx->printf("WIFI: %s", WiFi.localIP().toString().c_str());
    gfx->setTextColor(RGB565(150, 150, 165));
    gfx->setCursor(PILL_X + 12, PILL_Y + 34);
    gfx->printf("TOUCH: %s (%lu)", touchOK ? "OK" : "FAILED", touchSeen);
  } else if (apRunning) {
    // Exactly the two facts a phone needs, and nothing else: which network to join, where to go.
    gfx->setTextColor(RGB565(150, 220, 255));
    gfx->setCursor(PILL_X + 12, PILL_Y + 10);
    gfx->print("ThanksDanny-setup");
    gfx->setTextColor(RGB565(180, 230, 255));
    gfx->setCursor(PILL_X + 12, PILL_Y + 34);
    gfx->print("OPEN 192.168.4.1");
  } else if (wifiSSID.length()) {
    gfx->setTextColor(RGB565(255, 200, 120));
    gfx->setCursor(PILL_X + 12, PILL_Y + 10);
    gfx->print("WIFI: NO LINK");
    gfx->setTextColor(RGB565(150, 150, 165));
    gfx->setCursor(PILL_X + 12, PILL_Y + 34);
    gfx->print("TAP THE COG");
  } else {
    gfx->setTextColor(RGB565(255, 200, 120));
    gfx->setCursor(PILL_X + 12, PILL_Y + 10);
    gfx->print("WIFI: NOT SET UP");
    gfx->setTextColor(RGB565(150, 150, 165));
    gfx->setCursor(PILL_X + 12, PILL_Y + 34);
    gfx->print("TAP THE COG");
  }

  Serial.println("ui     : home (pictures under the sign, cog upper right, inside the circle)");
}

void drawMenu() {
  gfx->fillScreen(RGB565(12, 12, 16));
  centred(26, "Settings", 3, RGB565_WHITE);
  gfx->drawFastHLine(ROW_X, 78, ROW_W, RGB565(60, 60, 72));

  int y = ROW_Y0;

  // Pictures first: it is the app, the rest are settings.
  button(ROW_X, y, ROW_W, ROW_H, RGB565(28, 28, 36), RGB565(80, 80, 96));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(ROW_X + 18, y + 9);
  gfx->print("Pictures");
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565(150, 200, 255));
  gfx->setCursor(ROW_X + 18, y + 31);
  gfx->print("scroll random pictures from GitHub");

  y += ROW_H + ROW_GAP;
  button(ROW_X, y, ROW_W, ROW_H, RGB565(28, 28, 36), RGB565(80, 80, 96));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(ROW_X + 18, y + 9);
  gfx->print("Wi-Fi setup on phone");
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565(150, 200, 255));
  gfx->setCursor(ROW_X + 18, y + 31);
  if (WiFi.status() == WL_CONNECTED)      gfx->printf("connected: %s", WiFi.localIP().toString().c_str());
  else if (wifiSSID.length())             gfx->printf("saved: %s (not connected)", wifiSSID.c_str());
  else                                    gfx->print("no network saved yet - pictures need this");

  y += ROW_H + ROW_GAP;
  button(ROW_X, y, ROW_W, ROW_H, RGB565(28, 28, 36), RGB565(80, 80, 96));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(ROW_X + 18, y + 9);
  gfx->print("Touch test");
  gfx->setTextSize(1);
  gfx->setTextColor(RGB565(150, 200, 255));
  gfx->setCursor(ROW_X + 18, y + 31);
  gfx->printf("check where taps land (%lu seen)", touchSeen);

  y += ROW_H + ROW_GAP;
  button(ROW_X, y, ROW_W, ROW_H, RGB565(28, 28, 36), RGB565(80, 80, 96));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(ROW_X + 18, y + 19);
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

void drawTouchTest() {
  gfx->fillScreen(RGB565_BLACK);
  centred(24, "Touch test", 2, RGB565_WHITE);
  for (int i = 0; i < 4; i++) {
    gfx->drawCircle(MARK[i][0], MARK[i][1], 22, RGB565(90, 200, 255));
    gfx->drawFastHLine(MARK[i][0] - 26, MARK[i][1], 52, RGB565(90, 200, 255));
    gfx->drawFastVLine(MARK[i][0], MARK[i][1] - 26, 52, RGB565(90, 200, 255));
  }
  centred(236, "tap the four circles", 1, RGB565(180, 180, 190));

  // Raw coordinates of the last tap, drawn big on the glass. If the mapping is wrong these
  // numbers will not match where the finger went - and that is the answer.
  gfx->fillRoundRect(PILL_X, 292, PILL_W, 44, 12, RGB565(14, 14, 20));
  gfx->drawRoundRect(PILL_X, 292, PILL_W, 44, 12, RGB565(90, 90, 110));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565(255, 220, 120));
  gfx->setCursor(PILL_X + 16, PILL_Y - 82);
  gfx->print("tap: none yet");

  // Responsiveness, measured on the device. First number is one controller read; second is
  // the whole tap-to-response path. If the badge feels slow, read these off and they say why.
  char perf[48];
  snprintf(perf, sizeof(perf), "read %lu us / act %lu ms", lastReadUs, lastActMs);
  centred(270, perf, 1, RGB565(140, 140, 155));

  button(PILL_X, PILL_Y, PILL_W, PILL_H, RGB565(28, 28, 36), RGB565(80, 80, 96));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(PILL_X + 16, PILL_Y + 14);
  gfx->print("Back");

  Serial.printf("ui     : touch test - tap the four circles (read %lu us, act %lu ms)\n",
                lastReadUs, lastActMs);
}

void showScreen(int s) {
  screen = s;
  switch (s) {
    case ST_HOME:         drawHome();         break;
    case ST_MENU:         drawMenu();         break;
    case ST_APINFO:       drawAPInfo();       break;
    case ST_TOUCHTEST:    drawTouchTest();    break;
    case ST_GALLERY:      showGallery();      break;   // Gallery.ino; runs until the cog is used
    case ST_GALLERY_SYNC: drawGallerySync();  break;
  }
}

// Instant acknowledgement, drawn before any slow work. A full-screen repaint on this panel
// takes ~350 ms; without this the button feels dead for that whole time.
void ackCog() {
  gfx->drawCircle(COG_CX, COG_CY, COG_BTN_R + 5, RGB565_WHITE);
  gfx->drawCircle(COG_CX, COG_CY, COG_BTN_R + 6, RGB565(120, 200, 255));
}

void ackPictures() {
  gfx->drawCircle(PIC_CX, PIC_CY, PIC_BTN_R + 5, RGB565_WHITE);
  gfx->drawCircle(PIC_CX, PIC_CY, PIC_BTN_R + 6, RGB565(120, 200, 255));
}

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
  if (screen == ST_HOME) {
    int dx = x - COG_CX, dy = y - COG_CY;
    if (dx * dx + dy * dy <= (COG_BTN_R + 32) * (COG_BTN_R + 32)) ackCog();
    int px = x - PIC_CX, py = y - PIC_CY;
    if (px * px + py * py <= (PIC_BTN_R + 32) * (PIC_BTN_R + 32)) ackPictures();
  } else if (screen == ST_MENU && x >= ROW_X && x <= ROW_X + ROW_W) {
    int y1 = ROW_Y0, y2 = ROW_Y0 + ROW_H + ROW_GAP, y3 = y2 + ROW_H + ROW_GAP;
    if (y >= y1 && y <= y1 + ROW_H)       ackRow(y1);
    else if (y >= y2 && y <= y2 + ROW_H)  ackRow(y2);
    else if (y >= y3 && y <= y3 + ROW_H)  ackRow(y3);
  } else if (screen == ST_APINFO || screen == ST_TOUCHTEST) {
    if (x >= PILL_X && x <= PILL_X + PILL_W && y >= PILL_Y && y <= PILL_Y + PILL_H)
      gfx->drawRoundRect(PILL_X - 3, PILL_Y - 3, PILL_W + 6, PILL_H + 6, 16, RGB565(150, 230, 255));
  }

  if (screen == ST_HOME) {
    // Generous targets: a first-time tap should not depend on hitting a 21-pixel gear exactly.
    int dx = x - COG_CX, dy = y - COG_CY;
    bool onCog = (dx * dx + dy * dy <= (COG_BTN_R + 32) * (COG_BTN_R + 32));
    int px = x - PIC_CX, py = y - PIC_CY;
    bool onPictures = (px * px + py * py <= (PIC_BTN_R + 32) * (PIC_BTN_R + 32));
    if (onCog) {
      Serial.println("         -> cog: opening settings");
      showScreen(ST_MENU);
    } else if (onPictures) {
      // The pictures were fetched at boot, so this opens instantly from cache.
      Serial.println("         -> pictures: opening the gallery");
      showScreen(ST_GALLERY);
    } else {
      Serial.println("         -> (home: tap missed the cog, showing coordinates in the pill)");
      // Print where the finger actually landed, right on the glass. This makes the touch
      // diagnostic reachable WITHOUT going through the menu, which matters because the menu
      // is only reachable by touching the cog - the thing under suspicion.
      gfx->fillRoundRect(PILL_X + 2, PILL_Y + 2, PILL_W - 4, PILL_H - 4, 12, RGB565(14, 14, 20));
      gfx->setTextSize(1);
      gfx->setTextColor(RGB565(255, 220, 120));
      gfx->setCursor(PILL_X + 16, PILL_Y + 8);
      gfx->printf("tap: x=%d y=%d", x, y);
      gfx->setTextColor(RGB565(150, 150, 165));
      gfx->setCursor(PILL_X + 16, PILL_Y + 24);
      gfx->printf("cog is at %d,%d r=%d", COG_CX, COG_CY, COG_BTN_R + 32);
    }
    lastActMs = millis() - t0;
    return;
  }

  if (screen == ST_MENU) {
    int y1 = ROW_Y0;
    int y2 = y1 + ROW_H + ROW_GAP;
    int y3 = y2 + ROW_H + ROW_GAP;
    int y4 = y3 + ROW_H + ROW_GAP;
    if (x >= ROW_X && x <= ROW_X + ROW_W) {
      if (y >= y1 && y <= y1 + ROW_H) {
        Serial.println("         -> menu: pictures (fetch if needed, then scroll)");
        showScreen(ST_GALLERY);
      } else if (y >= y2 && y <= y2 + ROW_H) {
        // Use the scan cached at boot. Rescanning here measured 3363 ms on the tap path -
        // the single worst delay in the UI - and the list only changes when networks do.
        // Say something immediately anyway: an acked tap must never look ignored.
        if (scanCount == 0) {
          centred(392, "scanning for networks...", 1, RGB565(255, 220, 120));
          scanAndCache();
        }
        Serial.println("         -> menu: wi-fi setup (cached scan, then raise the AP)");
        startSetupAP();
        showScreen(ST_APINFO);
      } else if (y >= y3 && y <= y3 + ROW_H) {
        Serial.println("         -> menu: touch test"); showScreen(ST_TOUCHTEST);
      } else if (y >= y4 && y <= y4 + ROW_H) {
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
    int dx = x - COG_CX, dy = y - COG_CY;
    if (dx * dx + dy * dy <= (COG_BTN_R + 32) * (COG_BTN_R + 32)) {
      Serial.println("         -> gallery: cog, back to settings");
      showScreen(ST_MENU);
    } else if (picCount == 0) {
      Serial.println("         -> gallery: fetching pictures from GitHub");
      syncPictures();
    }
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

  if (screen == ST_TOUCHTEST) {
    if (x >= PILL_X && x <= PILL_X + PILL_W && y >= PILL_Y && y <= PILL_Y + PILL_H) {
      Serial.println("         -> back"); showScreen(ST_MENU);
      lastActMs = millis() - t0;
      return;
    }
    // Mark where the tap landed, so a wrong mapping is visible on the panel, not just in a log.
    gfx->fillCircle(x, y, 6, RGB565(255, 90, 90));
    gfx->drawCircle(x, y, 11, RGB565_WHITE);

    // Replace the placeholder with the live numbers: read them off the glass and they say
    // exactly how the controller's frame differs from the panel's.
    gfx->fillRoundRect(PILL_X + 2, 294, PILL_W - 4, 40, 10, RGB565(14, 14, 20));
    gfx->drawRoundRect(PILL_X, 292, PILL_W, 44, 12, RGB565(90, 90, 110));
    gfx->setTextSize(2);
    gfx->setTextColor(RGB565(255, 220, 120));
    gfx->setCursor(PILL_X + 16, PILL_Y - 82);
    gfx->printf("x=%d y=%d", x, y);
    for (int i = 0; i < 4; i++) {
      int dx = x - MARK[i][0], dy = y - MARK[i][1];
      if (dx * dx + dy * dy <= 26 * 26) {
        Serial.printf("         -> landed inside circle %d\n", i + 1);
        break;
      }
    }
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

void pollTouch() {
  if (!touchOK) return;
  int16_t xs[1], ys[1];
  uint32_t r0 = micros();
  uint8_t n = touch.getPoint(xs, ys, 1);
  lastReadUs = micros() - r0;
  bool pressed = (n > 0);
  // One tap per press: a held finger must not fire repeatedly. The guard is deliberately
  // short - a long one silently swallows quick taps and feels like a slow panel.
  if (pressed && !touchDown && millis() - lastTap > TAP_GUARD_MS) {
    touchDown = true;
    lastTap = millis();
    uint32_t a0 = micros();
    handleTap(xs[0], ys[0]);
    // Logged so the tap-to-response time can be measured from here rather than guessed at.
    Serial.printf("         -> responded in %lu ms (read %lu us)\n",
                  (micros() - a0) / 1000, lastReadUs);
  } else if (!pressed) {
    touchDown = false;
  }
}

// ------------------------------------------------------------------ setup/loop

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== Thanks Danny badge: pictures + cog + Wi-Fi setup ===");

  if (!gfx->begin()) { Serial.println("! gfx->begin FAILED"); return; }
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(170);
  Serial.printf("panel  : CO5300 up, %dx%d round (visible circle r=%d)\n",
                LCD_WIDTH, LCD_HEIGHT, PANEL_R);

  // Check the layout against the round mask at boot. This is the check that would have caught
  // the cog being drawn in a masked corner, where it draws without error and is never seen.
  float cogDist = sqrtf(powf(COG_CX - LCD_WIDTH / 2.0f, 2) + powf(COG_CY - LCD_HEIGHT / 2.0f, 2));
  float picDist = sqrtf(powf(PIC_CX - LCD_WIDTH / 2.0f, 2) + powf(PIC_CY - LCD_HEIGHT / 2.0f, 2));
  bool cogOK  = circleInsidePanel(COG_CX, COG_CY, COG_BTN_R);
  bool picOK  = circleInsidePanel(PIC_CX, PIC_CY, PIC_BTN_R);
  bool pillOK = rectInsidePanel(PILL_X, PILL_Y, PILL_W, PILL_H);
  bool rowsOK = rectInsidePanel(ROW_X, ROW_Y0, ROW_W, ROW_H) &&
                rectInsidePanel(ROW_X, ROW_Y0 + (ROW_COUNT - 1) * (ROW_H + ROW_GAP), ROW_W, ROW_H);
  Serial.printf("layout : cog %s (centre %.0f + disc %d of %d) | pictures %s (centre %.0f) | pill %s | rows %s\n",
                cogOK ? "yes" : "NO", cogDist, COG_BTN_R, PANEL_R - 4,
                picOK ? "yes" : "NO", picDist,
                pillOK ? "yes" : "NO", rowsOK ? "yes" : "NO");
  if (!cogOK || !picOK || !pillOK || !rowsOK) Serial.println("! a control sits outside the round aperture");

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
      // Redraw only if the sync actually took the screen. A quiet sync left the home screen
      // untouched, and redrawing it would flash the panel for nothing.
      if (screen != ST_HOME) showScreen(ST_HOME);
    } else if (millis() - connectStarted > CONNECT_TIMEOUT_MS) {
      connecting = false;
      Serial.println("wifi   : could not connect - the setup network stays up");
      startSetupAP();
      if (screen == ST_APINFO) showScreen(ST_APINFO);
    }
  }
  delay(POLL_MS);
}
