/*
 * Clock (v3.27 steampunk; v3.29 CYBERPUNK) - a green/amber radar HUD that tells the time.
 *
 * Static parts (rings, ticks, arcs, polar grid, minute numbers) are one background JPEG in
 * clock_assets.h, decoded once into PSRAM. Every frame the badge draws on top of it:
 *   - the radar SWEEP: green light ADDED to the background behind a turning beam (90 degree trail),
 *     using a per-pixel angle table built when the app opens - grid and numbers shine through;
 *   - the outer TEXT RING: a pre-rendered strip of text (SONS OF SUDO * SYS.LEVEL: ROOT * ... the
 *     Socrates quote in orange) mapped onto the ring via a second table (v3.30: no longer scrolls);
 *   - the SECONDS RING: 120 amber bars filling up through the minute, the newest ones brightest;
 *   - the TIME in big amber stencil digits with their glow, also added like light;
 *   - live HUD text: network status (MQTT link), badge name, Wi-Fi signal, uptime, date, CET/CEST.
 * Time: the system clock (NTP via configTzTime in setup(), Europe/Amsterdam). Until synced the date
 * line says WAITING FOR NTP. Tap anywhere: home. Burn-in: the whole picture moves by up to 2 px
 * every 2 minutes; the screensaver does not take over this screen.
 * Assets: tools/make_clock_assets.py (fonts: Black Ops One + Share Tech Mono, SIL OFL).
 *
 * v3.30 - smoother: a full-screen flush costs ~46 ms on this panel, and v3.29 sent the whole screen
 * every frame (~12 fps, and anything else running at the same time showed as a hang). Now:
 *   - every frame only the centre (the sweep disk with the time and HUD text, ~314 x 314) is
 *     restored, drawn and sent;
 *   - a seconds bar is redrawn and sent (a small patch) only when it changes - twice a second;
 *     at the top of the minute all of them, as one full flush;
 *   - the text ring is drawn on full frames only (first frame, burn-in shift every 2 minutes);
 *   - Wi-Fi RSSI, link state and badge name are read once a second, not every frame;
 *   - a frame slower than 120 ms is logged on the spot with its breakdown.
 */

// clock_assets.h is included by Badge.ino (Arduino puts the prototypes above this file's includes)

#define CLK_FRAME_MS   50                  // 20 fps when a frame is cheap enough ...
#define CLK_GAP_MS     15                  // ... but always this much loop() time between frames
#define CLK_SWEEP_S    6.0                 // one turn of the radar beam
#define CLK_TRAIL      64                  // trail length in 1/256 turns (90 degrees)

static uint16_t *clkBg = nullptr;          // the background, 466 x 466 RGB565 in PSRAM
static uint32_t clkLast = 0, clkEnd = 0;
static uint32_t clkFrames = 0, clkCostMs = 0, clkUs[4];

// Sweep disk: per row its x range, and one angle byte per pixel (0..255 = clockwise from 12).
static int16_t clkSwX0[2 * CLK_SWEEP_R + 1], clkSwX1[2 * CLK_SWEEP_R + 1];
static uint8_t *clkSwAng = nullptr;
// Text ring: per row up to two x spans; per pixel the strip column (at scroll 0) and strip row.
#define CLK_RING_ROWS (2 * CLK_TXT_R1 + 1)
static int16_t clkRgA0[CLK_RING_ROWS], clkRgA1[CLK_RING_ROWS], clkRgB0[CLK_RING_ROWS], clkRgB1[CLK_RING_ROWS];
static uint16_t *clkRgCol = nullptr;
static uint8_t *clkRgRow = nullptr;
static uint16_t clkTrail[CLK_TRAIL];       // light added at each trail position
static int clkLastSlot = -1;               // burn-in position of the last frame (-1: next frame is full)
static uint8_t clkBarSt[120];              // what each seconds bar shows now (0 dim, 1 lit, 2 newest)
static uint32_t clkInfoAt = 0;             // HUD info, refreshed once a second
static char clkWifi[24], clkUp[24], clkName[40];
static bool clkLink = false;

static inline uint16_t clkAdd(uint16_t a, uint16_t b) {      // saturating RGB565 add
  uint32_t r = (a >> 11) + (b >> 11), g = ((a >> 5) & 63) + ((b >> 5) & 63), bl = (a & 31) + (b & 31);
  if (r > 31) r = 31;
  if (g > 63) g = 63;
  if (bl > 31) bl = 31;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

static float clkAngle(float dx, float dy) {                 // clockwise from 12, 0 .. 2 pi
  float a = atan2f(dx, -dy);
  return a < 0 ? a + 2 * PI : a;
}

static bool clkLoad() {
  if (clkBg && clkRgRow) return true;
  uint32_t t0 = millis();
  if (!clkBg) {
    clkBg = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (!clkBg) { Serial.println("clock  : no PSRAM for the background"); return false; }
    memset(clkBg, 0, LCD_WIDTH * LCD_HEIGHT * 2);
    decDest = clkBg;                                        // jpegBlock() packs into our buffer
    decDestW = LCD_WIDTH;
    TJpgDec.setJpgScale(1);
    JRESULT r = TJpgDec.drawJpg(0, 0, CLK_BG_JPG, sizeof(CLK_BG_JPG));
    decDest = nullptr;
    if (r != JDR_OK) {
      Serial.printf("clock  : background JPEG failed (%d)\n", (int)r);
      heap_caps_free(clkBg); clkBg = nullptr;
      return false;
    }
  }
  // sweep table
  int n = 0;
  for (int j = 0; j <= 2 * CLK_SWEEP_R; j++) {
    int dy = j - CLK_SWEEP_R;
    int hw = (int)sqrtf((float)(CLK_SWEEP_R * CLK_SWEEP_R - dy * dy));
    clkSwX0[j] = -hw; clkSwX1[j] = hw;
    n += 2 * hw + 1;
  }
  // text ring spans
  int m = 0;
  for (int j = 0; j < CLK_RING_ROWS; j++) {
    int dy = j - CLK_TXT_R1;
    float o2 = (float)CLK_TXT_R1 * CLK_TXT_R1 - dy * dy, i2 = (float)CLK_TXT_R0 * CLK_TXT_R0 - dy * dy;
    clkRgA0[j] = 1; clkRgA1[j] = 0; clkRgB0[j] = 1; clkRgB1[j] = 0;   // empty
    if (o2 < 0) continue;
    int ho = (int)sqrtf(o2);
    if (i2 <= 0) { clkRgA0[j] = -ho; clkRgA1[j] = ho; }               // above / below the hole
    else {
      int hi = (int)sqrtf(i2);
      clkRgA0[j] = -ho; clkRgA1[j] = -hi; clkRgB0[j] = hi; clkRgB1[j] = ho;
    }
    if (clkRgA1[j] >= clkRgA0[j]) m += clkRgA1[j] - clkRgA0[j] + 1;
    if (clkRgB1[j] >= clkRgB0[j]) m += clkRgB1[j] - clkRgB0[j] + 1;
  }
  if (!clkSwAng) clkSwAng = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!clkRgCol) clkRgCol = (uint16_t *)heap_caps_malloc(m * 2, MALLOC_CAP_SPIRAM);
  uint8_t *rows = (uint8_t *)heap_caps_malloc(m, MALLOC_CAP_SPIRAM);
  if (!clkSwAng || !clkRgCol || !rows) { Serial.println("clock  : no PSRAM for the tables"); return false; }
  n = 0;
  for (int j = 0; j <= 2 * CLK_SWEEP_R; j++)
    for (int x = clkSwX0[j]; x <= clkSwX1[j]; x++)
      clkSwAng[n++] = (uint8_t)(clkAngle(x + 0.5f, j - CLK_SWEEP_R + 0.5f) / (2 * PI) * 256.0f);
  m = 0;
  for (int j = 0; j < CLK_RING_ROWS; j++) {
    for (int span = 0; span < 2; span++) {
      int a = span ? clkRgB0[j] : clkRgA0[j], b = span ? clkRgB1[j] : clkRgA1[j];
      for (int x = a; x <= b; x++, m++) {
        float dx = x + 0.5f, dy = j - CLK_TXT_R1 + 0.5f, r = sqrtf(dx * dx + dy * dy);
        int row = (int)(CLK_TXT_R1 - r);
        if (row > CLK_STRIP_H - 1) row = CLK_STRIP_H - 1;
        rows[m] = (r >= CLK_TXT_R0 && r < CLK_TXT_R1 && row >= 0) ? row : 255;   // 255: outside the band
        clkRgCol[m] = (uint16_t)(clkAngle(dx, dy) / (2 * PI) * CLK_STRIP_W) % CLK_STRIP_W;
      }
    }
  }
  for (int i = 0; i < CLK_TRAIL; i++) {                     // green light, fading along the trail
    float k = 1.0f - (float)i / CLK_TRAIL;
    k *= k;
    clkTrail[i] = RGB565((int)(20 * k), (int)(125 * k), (int)(45 * k));
  }
  clkRgRow = rows;                                          // last: marks the tables complete
  Serial.printf("clock  : ready in %lu ms (sweep %d px, text ring %d px)\n", (unsigned long)(millis() - t0), n, m);
  return true;
}

// HUD text: anchor 0 = centred, -1 = right-aligned at x, 1 = left-aligned at x; y = baseline.
static void clkText(int x, int y, const char *s, const GFXfont *f, uint16_t col, int anchor) {
  gfx->setFont(f);
  gfx->setTextSize(1);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(s, 0, y, &x1, &y1, &w, &h);
  int cx = anchor == 0 ? x - (int)w / 2 - x1 : anchor < 0 ? x - (int)w - x1 : x - x1;
  gfx->setTextColor(col);
  gfx->setCursor(cx, y);
  gfx->print(s);
  gfx->setFont();
}

// The big time: each character in a fixed slot (so the digits do not jiggle), added like light.
static void clkTime(uint16_t *fb, const char *t, int cx, int cy) {
  int total = 0;
  for (const char *p = t; *p; p++) total += *p == ':' ? CLK_COLON_ADV : CLK_DIG_ADV;
  int x = cx - total / 2, y0 = cy - CLK_DIG_H / 2;
  for (const char *p = t; *p; p++) {
    int idx = *p == ':' ? 10 : *p - '0';
    if (idx < 0 || idx > 10) continue;
    const ClkDigit &d = CLK_DIGITS[idx];
    int slot = *p == ':' ? CLK_COLON_ADV : CLK_DIG_ADV;
    int sx = x + (slot - (d.w - 2 * CLK_DIG_PAD)) / 2 - CLK_DIG_PAD;
    for (int j = 0; j < CLK_DIG_H; j++) {
      int y = y0 + j;
      if (y < 0 || y >= LCD_HEIGHT) continue;
      uint16_t *row = fb + y * LCD_WIDTH;
      const uint16_t *src = d.px + j * d.w;
      for (int i = 0; i < d.w; i++) {
        int xx = sx + i;
        if (src[i] && xx >= 0 && xx < LCD_WIDTH) row[xx] = clkAdd(row[xx], src[i]);
      }
    }
    x += slot;
  }
}

static void clkFrame() {
  uint32_t t0 = millis(), u0 = micros();
  uint16_t *fb = gfx->getFramebuffer();
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm lt;
  time_t now = tv.tv_sec;
  localtime_r(&now, &lt);
  bool synced = lt.tm_year >= 120;
  double sec = lt.tm_sec + tv.tv_usec / 1e6;
  double ms = millis() / 1000.0;
  // burn-in shift: one of 9 positions, a new one every 2 minutes (then a full frame)
  int slot = (int)((tv.tv_sec / 120) % 9);
  bool full = slot != clkLastSlot;
  clkLastSlot = slot;
  int ox = (slot % 3 - 1) * 2, oy = (slot / 3 - 1) * 2;
  int cx = 233 + ox, cy = 233 + oy;
  // 1. restore: everything on a full frame, otherwise only the sweep disk
  if (full) {
    for (int y = 0; y < LCD_HEIGHT; y++) {
      uint16_t *dst = fb + y * LCD_WIDTH;
      int sy = y - oy;
      if (sy < 0 || sy >= LCD_HEIGHT) { memset(dst, 0, LCD_WIDTH * 2); continue; }
      const uint16_t *src = clkBg + sy * LCD_WIDTH;
      if (ox >= 0) { memset(dst, 0, ox * 2); memcpy(dst + ox, src, (LCD_WIDTH - ox) * 2); }
      else { memcpy(dst, src - ox, (LCD_WIDTH + ox) * 2); memset(dst + LCD_WIDTH + ox, 0, -ox * 2); }
    }
    memset(clkBarSt, 0xFF, sizeof(clkBarSt));               // all bars get drawn
  } else {
    for (int j = 0; j <= 2 * CLK_SWEEP_R; j++) {
      int x0 = clkSwX0[j], x1 = clkSwX1[j];
      memcpy(fb + (cy - CLK_SWEEP_R + j) * LCD_WIDTH + cx + x0,
             clkBg + (233 - CLK_SWEEP_R + j) * LCD_WIDTH + 233 + x0, (x1 - x0 + 1) * 2);
    }
  }
  uint32_t u1 = micros();
  // 2. radar sweep: light behind the beam
  int beam = (int)(fmod(ms / CLK_SWEEP_S, 1.0) * 256.0);
  int n = 0;
  for (int j = 0; j <= 2 * CLK_SWEEP_R; j++) {
    uint16_t *row = fb + (cy - CLK_SWEEP_R + j) * LCD_WIDTH + cx;
    for (int x = clkSwX0[j]; x <= clkSwX1[j]; x++, n++) {
      uint8_t d = (uint8_t)(beam - clkSwAng[n]);
      if (d < CLK_TRAIL) row[x] = d == 0 ? clkAdd(row[x], RGB565(90, 255, 140)) : clkAdd(row[x], clkTrail[d]);
    }
  }
  // 3. text ring (full frames only)
  if (full) {
    int m = 0;
    for (int j = 0; j < CLK_RING_ROWS; j++) {
      uint16_t *row = fb + (cy - CLK_TXT_R1 + j) * LCD_WIDTH + cx;
      for (int span = 0; span < 2; span++) {
        int a = span ? clkRgB0[j] : clkRgA0[j], b = span ? clkRgB1[j] : clkRgA1[j];
        for (int x = a; x <= b; x++, m++) {
          uint8_t r = clkRgRow[m];
          if (r != 255) row[x] = CLK_STRIP[r * CLK_STRIP_W + clkRgCol[m]];
        }
      }
    }
  }
  // 4. seconds ring: redraw only the bars that change; remember their box
  float lit = synced ? (float)(sec * 2) : -1;
  int bx0 = LCD_WIDTH, by0 = LCD_HEIGHT, bx1 = -1, by1 = -1;
  for (int i = 0; i < 120; i++) {
    uint8_t st = i <= lit ? (i > lit - 4 ? 2 : 1) : 0;
    if (st == clkBarSt[i]) continue;
    clkBarSt[i] = st;
    float a0 = (i * 3 - 0.8f) * PI / 180, a1 = (i * 3 + 0.8f) * PI / 180;
    float s0 = sinf(a0), c0 = cosf(a0), s1 = sinf(a1), c1 = cosf(a1);
    uint16_t col = st == 2 ? RGB565(255, 210, 90) : st == 1 ? RGB565(255, 140, 20) : RGB565(70, 30, 4);
    int px[4] = { cx + (int)(s0 * CLK_BAR_R0), cx + (int)(s0 * CLK_BAR_R1), cx + (int)(s1 * CLK_BAR_R1), cx + (int)(s1 * CLK_BAR_R0) };
    int py[4] = { cy - (int)(c0 * CLK_BAR_R0), cy - (int)(c0 * CLK_BAR_R1), cy - (int)(c1 * CLK_BAR_R1), cy - (int)(c1 * CLK_BAR_R0) };
    gfx->fillTriangle(px[0], py[0], px[1], py[1], px[2], py[2], col);
    gfx->fillTriangle(px[0], py[0], px[2], py[2], px[3], py[3], col);
    for (int k = 0; k < 4; k++) {
      if (px[k] < bx0) bx0 = px[k];
      if (px[k] > bx1) bx1 = px[k];
      if (py[k] < by0) by0 = py[k];
      if (py[k] > by1) by1 = py[k];
    }
  }
  uint32_t u2 = micros();
  // 5. HUD text and the time
  char buf[48];
  if (full || millis() - clkInfoAt >= 1000) {               // Wi-Fi calls go through the Wi-Fi task: not every frame
    clkInfoAt = millis();
    clkLink = sosLinkUp();                                  // Sos.ino
    const char *me = sosMeIndex() >= 0 ? sosMeLabel() : "";          // v3.39: the display name (Claud-I/O)
    snprintf(clkName, sizeof(clkName), "SONS OF SUDO // %s", me && *me ? me : "BADGE");
    for (char *p = clkName; *p; p++) *p = toupper((unsigned char)*p);
    if (WiFi.status() == WL_CONNECTED) snprintf(clkWifi, sizeof(clkWifi), "WIFI %ddBm", (int)WiFi.RSSI());
    else snprintf(clkWifi, sizeof(clkWifi), "WIFI DOWN");
    uint32_t up = millis() / 60000;
    snprintf(clkUp, sizeof(clkUp), "UP %luh%02lum", (unsigned long)(up / 60), (unsigned long)(up % 60));
  }
  clkText(cx, cy - 93, clkLink ? "[ SYSTEM STATUS: SECURE ]" : "[ SYSTEM STATUS: NO LINK ]", &HudSmall,
          clkLink ? RGB565(150, 255, 170) : RGB565(255, 170, 40), 0);
  clkText(cx, cy - 71, clkName, &HudMid, RGB565(70, 255, 120), 0);
  clkText(cx - 10, cy - 37, clkWifi, &HudSmall, RGB565(60, 220, 110), -1);
  clkText(cx + 10, cy - 37, clkUp, &HudSmall, RGB565(60, 220, 110), 1);
  if (synced) {
    static const char *WD[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
    snprintf(buf, sizeof(buf), "STARDATE %04d.%02d.%02d %s", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, WD[lt.tm_wday]);
    clkText(cx, cy + 53, buf, &HudMid, RGB565(70, 255, 120), 0);
    clkText(cx, cy + 73, lt.tm_isdst ? "CEST" : "CET", &HudSmall, RGB565(255, 170, 40), 0);
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
  } else {
    clkText(cx, cy + 53, "WAITING FOR NTP", &HudMid, RGB565(255, 170, 40), 0);
    snprintf(buf, sizeof(buf), "00:00:00");
  }
  clkTime(fb, buf, cx, cy + 4);
  uint32_t u3 = micros();
  // 6. send: everything on a full frame; otherwise the centre box, plus the changed bars
  if (full) gfx->flush();
  else {
    int R = CLK_SWEEP_R + 2;
    flushRect(cx - R, cy - R, 2 * R + 2, 2 * R + 2);
    if (bx1 >= 0) {
      if (bx0 >= cx - R && bx1 <= cx + R && by0 >= cy - R && by1 <= cy + R) {}   // already sent
      else flushRect(bx0 - 2, by0 - 2, bx1 - bx0 + 6, by1 - by0 + 6);
    }
  }
  uint32_t u4 = micros();
  if (u4 - u0 > 120000)                                     // a slow frame: say where the time went
    Serial.printf("clock  : slow frame %lu ms%s = background %lu + sweep/ring/bars %lu + text/time %lu + flush %lu\n",
                  (unsigned long)((u4 - u0) / 1000), full ? " (full)" : "", (unsigned long)((u1 - u0) / 1000),
                  (unsigned long)((u2 - u1) / 1000), (unsigned long)((u3 - u2) / 1000), (unsigned long)((u4 - u3) / 1000));
  clkUs[0] += u1 - u0; clkUs[1] += u2 - u1; clkUs[2] += u3 - u2; clkUs[3] += u4 - u3;
  clkCostMs += millis() - t0;
  if (++clkFrames % 400 == 0) {                             // every ~20 s
    Serial.printf("clock  : %02d:%02d:%02d%s, frame %lu ms avg = restore %lu + sweep/ring/bars %lu + text/time %lu + flush %lu\n",
                  lt.tm_hour, lt.tm_min, lt.tm_sec, synced ? "" : " (not synced)", (unsigned long)(clkCostMs / 400),
                  (unsigned long)(clkUs[0] / 400000), (unsigned long)(clkUs[1] / 400000),
                  (unsigned long)(clkUs[2] / 400000), (unsigned long)(clkUs[3] / 400000));
    clkCostMs = 0; memset(clkUs, 0, sizeof(clkUs));
  }
}

void showClock() {
  if (!clkLoad()) {
    gfx->fillScreen(RGB565_BLACK);
    uiCentred(200, "CLOCK", &UiTitle, RGB565_WHITE);
    uiCentred(250, "not enough memory", &UiSmall, RGB565(200, 200, 200));
    gfx->flush();
    return;
  }
  clkFrames = 0; clkCostMs = 0; memset(clkUs, 0, sizeof(clkUs));
  clkLastSlot = -1;                                         // first frame: everything
  Serial.println("clock  : open - tap anywhere for home");
  clkLast = millis();
  clkFrame();
  clkEnd = millis();
}

void clockTick() {
  if (!clkBg || !clkRgRow) return;
  uint32_t now = millis();
  if (now - clkLast < CLK_FRAME_MS || now - clkEnd < CLK_GAP_MS) return;   // touch keeps its turn
  clkLast = now;
  clkFrame();
  clkEnd = millis();
}

void clockTap(int x, int y) {
  showScreen(ST_HOME);
}

// Carousel icon: a little radar HUD - orange rim, amber seconds arc, green ring, a sweep, the time.
void drawClockIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  gfx->fillCircle(cx, cy, r, RGB565(4, 14, 8));
  gfx->drawCircle(cx, cy, r, RGB565(255, 120, 20));
  gfx->drawCircle(cx, cy, r - 1, RGB565(255, 120, 20));
  for (int i = 0; i < 60; i += 2) {                         // seconds arc, two thirds lit
    float a = i * 2 * PI / 60;
    uint16_t c = i < 40 ? RGB565(255, 150, 20) : RGB565(70, 30, 4);
    gfx->drawLine(cx + (int)(sinf(a) * (r - 4)), cy - (int)(cosf(a) * (r - 4)),
                  cx + (int)(sinf(a) * (r - 9)), cy - (int)(cosf(a) * (r - 9)), c);
  }
  gfx->drawCircle(cx, cy, r - 12, RGB565(60, 255, 110));
  for (int i = 0; i < 12; i++) {                            // the sweep: a fading wedge
    float a0 = (40 - i * 5) * PI / 180, a1 = (45 - i * 5) * PI / 180;
    int k = 200 - i * 16;
    gfx->fillTriangle(cx, cy, cx + (int)(sinf(a0) * (r - 13)), cy - (int)(cosf(a0) * (r - 13)),
                      cx + (int)(sinf(a1) * (r - 13)), cy - (int)(cosf(a1) * (r - 13)), RGB565(k / 8, k / 2, k / 5));
  }
  gfx->drawLine(cx, cy, cx + (int)(sinf(45 * PI / 180) * (r - 13)), cy - (int)(cosf(45 * PI / 180) * (r - 13)), RGB565(120, 255, 160));
  gfx->setFont(&HudSmall);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds("12:00", 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(RGB565(255, 190, 50));
  gfx->setCursor(cx - (int)w / 2 - x1, cy + (int)h / 2);
  gfx->print("12:00");
  gfx->setFont();
}
