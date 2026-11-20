/*
 * Clock (v3.27, v3.28) - a steampunk pocket watch: the owner's gilded Roman-numeral dial, with a movement
 * behind a window in the middle whose wheels really turn.
 *
 * - The dial (bezel + numeral ring) is the owner's picture; the window inside the numerals shows a
 *   new movement plate. Both are in clock_assets.h as one JPEG, decoded once into PSRAM (clkBg).
 * - Twelve brass wheels (8 in front, 4 on a lower level sharing their arbors), a balance wheel, two
 *   steel bridges with ruby jewels and blued screws, and gilded Breguet hands. The moving parts are
 *   RGB565 sprites rotated per frame (nearest pixel, 3-level alpha). See tools/make_clock_assets.py:
 *   the train is laid out and phased there so the teeth mesh while they turn.
 * - v3.28: the seconds hand SWEEPS (continuous, like a fine mechanical watch) and the wheels turn
 *   smoothly; v3.27 ticked 4 times a second, and with the frame rate not a multiple of that the ticks
 *   came out uneven. The balance swings to and fro at 2 Hz. Wheel A turns once a minute.
 * - Time: the system clock (NTP via configTzTime in setup(), Europe/Amsterdam with DST). Until it
 *   has synced the hands rest at 10:08 and "WAITING FOR TIME" shows in the window.
 * - Burn-in: the AMOLED shows this dial for as long as you leave it, so the whole picture moves by
 *   a pixel or two every 2 minutes (9 positions). The screensaver does not take over this screen.
 * - Tap anywhere: home.
 *
 * Cost per frame (every CLK_FRAME_MS). v3.28 made it cheaper:
 *   - only the DIRTY rectangle is restored from the dial and sent to the panel: the movement window
 *     plus where the hands are now and where they were last frame (full-width band, no copy);
 *   - the sprites are copied from flash into PSRAM when the app opens (octal PSRAM fills cache
 *     misses ~4x faster than quad flash, and the rotations read the sprites all over the place);
 *   - the serial log breaks the frame down: restore / movement / hands / flush, in ms.
 */

// clock_assets.h is included by Badge.ino (Arduino puts the prototypes above this file's includes)

#define CLK_FRAME_MS   50                  // 20 fps when a frame is cheap enough ...
#define CLK_GAP_MS     15                  // ... but always this much loop() time between frames
#define CLK_BAL_HZ     2.0f                // balance swings per second
#define CLK_BAL_AMP    2.6f                // balance amplitude, radians (~150 deg)

static uint16_t *clkBg = nullptr;          // the decoded dial, 466 x 466 RGB565 in PSRAM
static uint32_t clkLast = 0, clkEnd = 0;   // start / end of the last frame
static int clkOx = 0, clkOy = 0;           // burn-in shift
static uint32_t clkFrames = 0, clkCostMs = 0;
static uint32_t clkUs[4];                  // restore / movement / hands / flush, summed (us)
static int clkPx0, clkPy0, clkPx1, clkPy1; // where the hands were last frame
static int clkLastSlot = -1;               // burn-in position of the last frame (-1: full redraw)

// v3.28: PSRAM copies of every sprite (same order as below), made when the app first opens.
#define CLK_NSPR (CLK_NGEARS + 5)          // gears, balance, bridges, 3 hands
static ClkSprite clkS[CLK_NSPR];
static bool clkSprReady = false;

static const ClkSprite *clkSrc(int i) {
  if (i < CLK_NGEARS) return CLK_GEARS[i].s;
  static const ClkSprite *rest[5] = { &CLK_BALANCE, &CLK_BRIDGES, &CLK_HAND_H, &CLK_HAND_M, &CLK_HAND_S };
  return rest[i - CLK_NGEARS];
}

static void clkLoadSprites() {
  if (clkSprReady) return;
  uint32_t t0 = millis(), bytes = 0;
  for (int i = 0; i < CLK_NSPR; i++) {
    const ClkSprite *f = clkSrc(i);
    clkS[i] = *f;                                       // falls back to the flash copy if no PSRAM
    size_t np = (size_t)f->w * f->h, na = (np + 3) / 4;
    uint16_t *p = (uint16_t *)heap_caps_malloc(np * 2, MALLOC_CAP_SPIRAM);
    uint8_t *a = (uint8_t *)heap_caps_malloc(na, MALLOC_CAP_SPIRAM);
    if (p && a) {
      memcpy(p, f->pix, np * 2); memcpy(a, f->a, na);
      clkS[i].pix = p; clkS[i].a = a;
      bytes += np * 2 + na;
    } else { if (p) heap_caps_free(p); if (a) heap_caps_free(a); }
  }
  clkSprReady = true;
  Serial.printf("clock  : sprites in PSRAM: %lu kB in %lu ms\n", (unsigned long)(bytes / 1024), (unsigned long)(millis() - t0));
}

static bool clkLoadDial() {
  if (clkBg) return true;
  clkBg = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
  if (!clkBg) { Serial.println("clock  : no PSRAM for the dial"); return false; }
  memset(clkBg, 0, LCD_WIDTH * LCD_HEIGHT * 2);
  uint32_t t0 = millis();
  decDest = clkBg;                         // jpegBlock() packs the blocks into our buffer
  decDestW = LCD_WIDTH;
  TJpgDec.setJpgScale(1);
  JRESULT r = TJpgDec.drawJpg(0, 0, CLK_DIAL_JPG, sizeof(CLK_DIAL_JPG));
  decDest = nullptr;
  if (r != JDR_OK) {
    Serial.printf("clock  : dial JPEG failed (%d)\n", (int)r);
    heap_caps_free(clkBg); clkBg = nullptr;
    return false;
  }
  Serial.printf("clock  : dial decoded in %lu ms\n", (unsigned long)(millis() - t0));
  return true;
}

static inline uint16_t clkHalf(uint16_t a, uint16_t b) {   // 50% mix of two RGB565 colours
  return ((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1);
}

// The screen box sprite s can touch when rotated by ang about (cx, cy), clipped to the panel.
static void clkBox(const ClkSprite *s, float cx, float cy, float ang, int *bx0, int *by0, int *bx1, int *by1) {
  float c = cosf(ang), sn = sinf(ang);
  int R = (int)ceilf(s->reach) + 1;
  int x0 = (int)cx - R, x1 = (int)cx + R, y0 = (int)cy - R, y1 = (int)cy + R;
  if (s->h > 2 * s->w) {                                // long thin (hands): box of the rotated rectangle
    float cxs[4] = { -s->px, s->w - s->px, s->w - s->px, -s->px };
    float cys[4] = { -s->py, -s->py, s->h - s->py, s->h - s->py };
    float mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9;
    for (int i = 0; i < 4; i++) {
      float X = cxs[i] * c - cys[i] * sn, Y = cxs[i] * sn + cys[i] * c;
      mnx = fminf(mnx, X); mxx = fmaxf(mxx, X); mny = fminf(mny, Y); mxy = fmaxf(mxy, Y);
    }
    x0 = (int)(cx + mnx) - 1; x1 = (int)(cx + mxx) + 1; y0 = (int)(cy + mny) - 1; y1 = (int)(cy + mxy) + 1;
  }
  *bx0 = x0 < 0 ? 0 : x0; *by0 = y0 < 0 ? 0 : y0;
  *bx1 = x1 > LCD_WIDTH - 1 ? LCD_WIDTH - 1 : x1; *by1 = y1 > LCD_HEIGHT - 1 ? LCD_HEIGHT - 1 : y1;
}

// Draw sprite s rotated by ang (radians, clockwise on screen) with its pivot at (cx, cy).
static void clkBlit(uint16_t *fb, const ClkSprite *s, float cx, float cy, float ang) {
  float c = cosf(ang), sn = sinf(ang);
  int x0, y0, x1, y1;
  clkBox(s, cx, cy, ang, &x0, &y0, &x1, &y1);
  const int32_t C = (int32_t)(c * 65536), S = (int32_t)(sn * 65536);
  const uint32_t w = s->w, h = s->h;
  for (int y = y0; y <= y1; y++) {
    float dy = y - cy, dx = x0 - cx;
    int32_t u = (int32_t)((dx * c + dy * sn + s->px) * 65536);   // sprite coords of (x0, y)
    int32_t v = (int32_t)((-dx * sn + dy * c + s->py) * 65536);
    uint16_t *row = fb + y * LCD_WIDTH;
    for (int x = x0; x <= x1; x++, u += C, v -= S) {
      uint32_t iu = (uint32_t)(u >> 16), iv = (uint32_t)(v >> 16);   // negative -> huge -> skipped
      if (iu >= w || iv >= h) continue;
      uint32_t i = iv * w + iu;
      uint8_t a = (s->a[i >> 2] >> ((i & 3) * 2)) & 3;
      if (a == 2) row[x] = s->pix[i];
      else if (a == 1) row[x] = clkHalf(row[x], s->pix[i]);
    }
  }
}

// The bridges never turn: walk only the visible part of each row.
static void clkBlitStatic(uint16_t *fb, const ClkSprite *s, const uint16_t *L, const uint16_t *Rr, int ox, int oy) {
  for (int j = 0; j < s->h; j++) {
    int y = oy + j;
    if (y < 0 || y >= LCD_HEIGHT || L[j] > Rr[j]) continue;
    uint16_t *row = fb + y * LCD_WIDTH;
    for (int i = L[j]; i <= Rr[j]; i++) {
      int x = ox + i;
      if (x < 0 || x >= LCD_WIDTH) continue;
      uint32_t k = j * s->w + i;
      uint8_t a = (s->a[k >> 2] >> ((k & 3) * 2)) & 3;
      if (a == 2) row[x] = s->pix[k];
      else if (a == 1) row[x] = clkHalf(row[x], s->pix[k]);
    }
  }
}

static inline void clkGrow(int *u0, int *v0, int *u1, int *v1, int x0, int y0, int x1, int y1) {
  if (x0 < *u0) *u0 = x0;
  if (y0 < *v0) *v0 = y0;
  if (x1 > *u1) *u1 = x1;
  if (y1 > *v1) *v1 = y1;
}

static void clkFrame() {
  uint32_t t0 = millis(), u0 = micros();
  uint16_t *fb = gfx->getFramebuffer();
  // the time - continuous, to the microsecond
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm lt;
  time_t now = tv.tv_sec;
  localtime_r(&now, &lt);
  bool synced = lt.tm_year >= 120;
  int hh = synced ? lt.tm_hour : 10, mm = synced ? lt.tm_min : 8, ss = synced ? lt.tm_sec : 0;
  double sec = ss + (synced ? tv.tv_usec / 1e6 : 0);   // 0 .. 60, smooth
  double secOfDay = hh * 3600.0 + mm * 60.0 + sec;
  // burn-in shift: one of 9 positions, a new one every 2 minutes (then the whole screen is redrawn)
  int slot = (int)((tv.tv_sec / 120) % 9);
  bool full = slot != clkLastSlot;
  clkLastSlot = slot;
  clkOx = (slot % 3 - 1) * 2;
  clkOy = (slot / 3 - 1) * 2;
  float cx = 233 + clkOx, cy = 233 + clkOy;
  // the hands now
  float aH = (float)(fmod(secOfDay / 43200.0, 1.0) * 2 * PI);
  float aM = (float)((mm + sec / 60.0) / 60.0 * 2 * PI);
  float aS = (float)(sec / 60.0 * 2 * PI);
  const ClkSprite *hs[3] = { &clkS[CLK_NGEARS + 2], &clkS[CLK_NGEARS + 3], &clkS[CLK_NGEARS + 4] };
  float ha[3] = { aH, aM, aS };
  int hx0 = LCD_WIDTH, hy0 = LCD_HEIGHT, hx1 = 0, hy1 = 0;
  for (int i = 0; i < 3; i++) {
    int a0, b0, a1, b1;
    clkBox(hs[i], cx, cy, ha[i], &a0, &b0, &a1, &b1);
    clkGrow(&hx0, &hy0, &hx1, &hy1, a0, b0, a1, b1);
  }
  // dirty = the movement window + the hands now + the hands last frame
  int dx0 = (int)cx - CLK_WIN - 2, dy0 = (int)cy - CLK_WIN - 2, dx1 = (int)cx + CLK_WIN + 2, dy1 = (int)cy + CLK_WIN + 2;
  clkGrow(&dx0, &dy0, &dx1, &dy1, hx0, hy0, hx1, hy1);
  if (!full) clkGrow(&dx0, &dy0, &dx1, &dy1, clkPx0, clkPy0, clkPx1, clkPy1);
  else { dx0 = 0; dy0 = 0; dx1 = LCD_WIDTH - 1; dy1 = LCD_HEIGHT - 1; }
  if (dx0 < 0) dx0 = 0;
  if (dy0 < 0) dy0 = 0;
  if (dx1 > LCD_WIDTH - 1) dx1 = LCD_WIDTH - 1;
  if (dy1 > LCD_HEIGHT - 1) dy1 = LCD_HEIGHT - 1;
  clkPx0 = hx0; clkPy0 = hy0; clkPx1 = hx1; clkPy1 = hy1;
  // 1. restore the dial (shifted) inside the dirty box
  for (int y = dy0; y <= dy1; y++) {
    uint16_t *dst = fb + y * LCD_WIDTH;
    int sy = y - clkOy;
    int a = dx0, b = dx1;                               // dst columns a..b come from src a-ox..b-ox
    if (sy < 0 || sy >= LCD_HEIGHT) { memset(dst + a, 0, (b - a + 1) * 2); continue; }
    if (a - clkOx < 0) { memset(dst + a, 0, (clkOx - a) * 2); a = clkOx; }
    if (b - clkOx > LCD_WIDTH - 1) { memset(dst + LCD_WIDTH + clkOx, 0, (b - (LCD_WIDTH - 1 + clkOx)) * 2); b = LCD_WIDTH - 1 + clkOx; }
    if (b >= a) memcpy(dst + a, clkBg + sy * LCD_WIDTH + (a - clkOx), (b - a + 1) * 2);
  }
  uint32_t u1 = micros();
  // 2. the movement: lower wheels first, then balance, front wheels, bridges
  float phiA = (float)(fmod(secOfDay / 60.0, 1.0) * 2 * PI);   // wheel A: one turn a minute
  float bal = CLK_BAL_AMP * sinf((float)fmod(secOfDay * CLK_BAL_HZ, 1.0) * 2 * PI);
  for (int i = 0; i < 4; i++) {
    const ClkGear &g = CLK_GEARS[i];
    clkBlit(fb, &clkS[i], cx + g.x, cy + g.y, g.phase + g.ratio * phiA);
  }
  clkBlit(fb, &clkS[CLK_NGEARS], cx + CLK_BAL_X, cy + CLK_BAL_Y, bal);
  for (int i = 4; i < CLK_NGEARS; i++) {
    const ClkGear &g = CLK_GEARS[i];
    clkBlit(fb, &clkS[i], cx + g.x, cy + g.y, g.phase + g.ratio * phiA);
  }
  const ClkSprite *br = &clkS[CLK_NGEARS + 1];
  clkBlitStatic(fb, br, CLK_BRIDGES_l, CLK_BRIDGES_r, (int)cx - (int)br->px, (int)cy - (int)br->py);
  if (!synced) uiCentred(233 - 110 + clkOy, "WAITING FOR TIME", &UiSmall, RGB565(230, 210, 160));
  uint32_t u2 = micros();
  // 3. the hands, and the centre cap
  for (int i = 0; i < 3; i++) clkBlit(fb, hs[i], cx, cy, ha[i]);
  int icx = (int)cx, icy = (int)cy;
  gfx->fillCircle(icx, icy, 9, RGB565(60, 36, 10));      // centre cap with a ruby
  gfx->fillCircle(icx, icy, 7, RGB565(226, 186, 96));
  gfx->fillCircle(icx, icy, 3, RGB565(170, 20, 45));
  gfx->drawPixel(icx - 1, icy - 1, RGB565(240, 150, 170));
  uint32_t u3 = micros();
  // 4. send only the rows that changed (a full-width band goes out without a copy)
  flushRect(0, dy0, LCD_WIDTH, dy1 - dy0 + 1);
  uint32_t u4 = micros();
  clkUs[0] += u1 - u0; clkUs[1] += u2 - u1; clkUs[2] += u3 - u2; clkUs[3] += u4 - u3;
  clkCostMs += millis() - t0;
  if (++clkFrames % 400 == 0) {                          // every ~20 s
    Serial.printf("clock  : %02d:%02d:%02d%s, frame %lu ms avg = restore %lu + movement %lu + hands %lu + flush %lu\n",
                  hh, mm, ss, synced ? "" : " (not synced)", (unsigned long)(clkCostMs / 400),
                  (unsigned long)(clkUs[0] / 400000), (unsigned long)(clkUs[1] / 400000),
                  (unsigned long)(clkUs[2] / 400000), (unsigned long)(clkUs[3] / 400000));
    clkCostMs = 0; memset(clkUs, 0, sizeof(clkUs));
  }
}

void showClock() {
  if (!clkLoadDial()) {
    gfx->fillScreen(RGB565_BLACK);
    uiCentred(200, "CLOCK", &UiTitle, RGB565_WHITE);
    uiCentred(250, "no memory for the dial", &UiSmall, RGB565(200, 200, 200));
    gfx->flush();
    return;
  }
  clkLoadSprites();
  clkFrames = 0; clkCostMs = 0; memset(clkUs, 0, sizeof(clkUs));
  clkLastSlot = -1;                                    // first frame: the whole screen
  Serial.println("clock  : open - tap anywhere for home");
  clkLast = millis();
  clkFrame();
  clkEnd = millis();
}

void clockTick() {
  if (!clkBg) return;
  uint32_t now = millis();
  if (now - clkLast < CLK_FRAME_MS || now - clkEnd < CLK_GAP_MS) return;   // touch keeps its turn
  clkLast = now;
  clkFrame();
  clkEnd = millis();
}

void clockTap(int x, int y) {
  showScreen(ST_HOME);
}

// Carousel icon: a little gilt pocket watch with a gear in it.
void drawClockIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  gfx->fillCircle(cx, cy, r, RGB565(176, 132, 52));                 // bezel
  gfx->fillCircle(cx, cy, r - r / 7 - 1, RGB565(236, 222, 186));    // cream dial
  for (int i = 0; i < 12; i++) {                                    // hour marks
    float a = i * PI / 6;
    int r1 = r - r / 7 - 2, r2 = r1 - (i % 3 ? r / 9 : r / 5);
    gfx->drawLine(cx + (int)(cosf(a) * r1), cy + (int)(sinf(a) * r1), cx + (int)(cosf(a) * r2), cy + (int)(sinf(a) * r2), RGB565(30, 24, 18));
  }
  drawCog(cx, cy, r / 3, RGB565(190, 140, 50), RGB565(236, 222, 186));   // the movement
  gfx->drawLine(cx, cy, cx - r / 3, cy - r / 2, RGB565(30, 24, 18));      // hands at 10:08
  gfx->drawLine(cx + 1, cy, cx - r / 3 + 1, cy - r / 2, RGB565(30, 24, 18));
  gfx->drawLine(cx, cy, cx + r / 2, cy - r / 3, RGB565(30, 24, 18));
  gfx->drawLine(cx, cy + 1, cx + r / 2, cy - r / 3 + 1, RGB565(30, 24, 18));
  gfx->fillCircle(cx, cy, 2, RGB565(170, 20, 45));
  gfx->fillRect(cx - r / 8, cy - r - r / 5, r / 4 + 1, r / 5 + 1, RGB565(176, 132, 52));   // crown
}
