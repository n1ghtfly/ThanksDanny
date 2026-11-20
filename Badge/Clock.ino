/*
 * Clock (v3.27) - a steampunk pocket watch: the owner's gilded Roman-numeral dial, with a movement
 * behind a window in the middle whose wheels really turn.
 *
 * - The dial (bezel + numeral ring) is the owner's picture; the window inside the numerals shows a
 *   new movement plate. Both are in clock_assets.h as one JPEG, decoded once into PSRAM (clkBg).
 * - Twelve brass wheels (8 in front, 4 on a lower level sharing their arbors), a balance wheel, two
 *   steel bridges with ruby jewels and blued screws, and gilded Breguet hands. The moving parts are
 *   RGB565 sprites rotated per frame (nearest pixel, 3-level alpha). See tools/make_clock_assets.py:
 *   the train is laid out and phased there so the teeth mesh while they turn.
 * - It TICKS like a watch: 4 beats a second. On each beat the train jumps forward (ease-out over
 *   70 ms) and so does the seconds hand; the balance swings to and fro once per 2 beats (2 Hz).
 *   Wheel A turns once a minute, the others at their gear ratios.
 * - Time: the system clock (NTP via configTzTime in setup(), Europe/Amsterdam with DST). Until it
 *   has synced the hands rest at 10:08 and "WAITING FOR TIME" shows in the window.
 * - Burn-in: the AMOLED shows this dial for as long as you leave it, so the whole picture moves by
 *   a pixel or two every 2 minutes (9 positions). The screensaver does not take over this screen.
 * - Tap anywhere: home.
 *
 * Cost per frame (every CLK_FRAME_MS): copy the dial into the canvas, ~180k sprite pixels checked,
 * flush. The sprites only touch the box their visible pixels can reach (ClkSprite.reach).
 */

// clock_assets.h is included by Badge.ino (Arduino puts the prototypes above this file's includes)

#define CLK_FRAME_MS   50                  // 20 fps when a frame is cheap enough ...
#define CLK_GAP_MS     15                  // ... but always this much loop() time between frames
#define CLK_BEATS      4                   // beats per second (14400 per hour)
#define CLK_EASE       0.28f               // part of a beat the jump takes
#define CLK_BAL_AMP    2.6f                // balance amplitude, radians (~150 deg)

static uint16_t *clkBg = nullptr;          // the decoded dial, 466 x 466 RGB565 in PSRAM
static uint32_t clkLast = 0, clkEnd = 0;   // start / end of the last frame
static int clkOx = 0, clkOy = 0;           // burn-in shift
static uint32_t clkFrames = 0, clkCostMs = 0;

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

// Draw sprite s rotated by ang (radians, clockwise on screen) with its pivot at (cx, cy).
static void clkBlit(uint16_t *fb, const ClkSprite *s, float cx, float cy, float ang) {
  float c = cosf(ang), sn = sinf(ang);
  int R = (int)ceilf(s->reach) + 1;
  int x0 = (int)cx - R, x1 = (int)cx + R, y0 = (int)cy - R, y1 = (int)cy + R;
  // Long thin sprites (hands): the box of the rotated sprite rectangle is much smaller.
  if (s->h > 2 * s->w) {
    float cxs[4] = { -s->px, s->w - s->px, s->w - s->px, -s->px };
    float cys[4] = { -s->py, -s->py, s->h - s->py, s->h - s->py };
    float mnx = 1e9, mxx = -1e9, mny = 1e9, mxy = -1e9;
    for (int i = 0; i < 4; i++) {
      float X = cxs[i] * c - cys[i] * sn, Y = cxs[i] * sn + cys[i] * c;
      mnx = fminf(mnx, X); mxx = fmaxf(mxx, X); mny = fminf(mny, Y); mxy = fmaxf(mxy, Y);
    }
    x0 = (int)(cx + mnx) - 1; x1 = (int)(cx + mxx) + 1; y0 = (int)(cy + mny) - 1; y1 = (int)(cy + mxy) + 1;
  }
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > LCD_WIDTH - 1) x1 = LCD_WIDTH - 1;
  if (y1 > LCD_HEIGHT - 1) y1 = LCD_HEIGHT - 1;
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

static void clkFrame() {
  uint32_t t0 = millis();
  uint16_t *fb = gfx->getFramebuffer();
  // burn-in shift: one of 9 positions, a new one every 2 minutes
  int slot = (int)((time(nullptr) / 120) % 9);
  clkOx = (slot % 3 - 1) * 2;
  clkOy = (slot / 3 - 1) * 2;
  // 1. the dial, shifted
  for (int y = 0; y < LCD_HEIGHT; y++) {
    uint16_t *dst = fb + y * LCD_WIDTH;
    int sy = y - clkOy;
    if (sy < 0 || sy >= LCD_HEIGHT) { memset(dst, 0, LCD_WIDTH * 2); continue; }
    const uint16_t *src = clkBg + sy * LCD_WIDTH;
    if (clkOx >= 0) { memset(dst, 0, clkOx * 2); memcpy(dst + clkOx, src, (LCD_WIDTH - clkOx) * 2); }
    else { memcpy(dst, src - clkOx, (LCD_WIDTH + clkOx) * 2); memset(dst + LCD_WIDTH + clkOx, 0, -clkOx * 2); }
  }
  // 2. the time
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm lt;
  time_t now = tv.tv_sec;
  localtime_r(&now, &lt);
  bool synced = lt.tm_year >= 120;
  int hh = synced ? lt.tm_hour : 10, mm = synced ? lt.tm_min : 8, ss = synced ? lt.tm_sec : 0;
  double secOfDay = hh * 3600.0 + mm * 60.0 + ss + tv.tv_usec / 1e6;
  double T = secOfDay * CLK_BEATS;                 // beats since midnight
  double k = floor(T);
  float f = (float)(T - k);
  float e = f / CLK_EASE; if (e > 1) e = 1;
  e = 1 - (1 - e) * (1 - e) * (1 - e);             // ease-out: a crisp jump
  double beat = k + e;
  float phiA = (float)fmod(beat / (60.0 * CLK_BEATS), 1.0) * 2 * PI;   // wheel A: one turn a minute
  float cx = 233 + clkOx, cy = 233 + clkOy;
  // 3. the movement: lower wheels first, then balance, front wheels, bridges
  float bal = CLK_BAL_AMP * sinf((float)fmod(T, 2.0) * PI);
  for (int i = 0; i < 4; i++) {
    const ClkGear &g = CLK_GEARS[i];
    clkBlit(fb, g.s, cx + g.x, cy + g.y, g.phase + g.ratio * phiA);
  }
  clkBlit(fb, &CLK_BALANCE, cx + CLK_BAL_X, cy + CLK_BAL_Y, bal);
  for (int i = 4; i < CLK_NGEARS; i++) {
    const ClkGear &g = CLK_GEARS[i];
    clkBlit(fb, g.s, cx + g.x, cy + g.y, g.phase + g.ratio * phiA);
  }
  clkBlitStatic(fb, &CLK_BRIDGES, CLK_BRIDGES_l, CLK_BRIDGES_r, (int)cx - (int)CLK_BRIDGES.px, (int)cy - (int)CLK_BRIDGES.py);
  if (!synced) uiCentred(233 - 110 + clkOy, "WAITING FOR TIME", &UiSmall, RGB565(230, 210, 160));
  // 4. the hands (the seconds hand ticks with the beats)
  float secs = (float)fmod(beat / CLK_BEATS, 60.0);
  float aH = ((hh % 12) + mm / 60.0f + ss / 3600.0f) / 12.0f * 2 * PI;
  float aM = (mm + ss / 60.0f) / 60.0f * 2 * PI;
  float aS = secs / 60.0f * 2 * PI;
  clkBlit(fb, &CLK_HAND_H, cx, cy, aH);
  clkBlit(fb, &CLK_HAND_M, cx, cy, aM);
  clkBlit(fb, &CLK_HAND_S, cx, cy, aS);
  int icx = (int)cx, icy = (int)cy;
  gfx->fillCircle(icx, icy, 9, RGB565(60, 36, 10));      // centre cap with a ruby
  gfx->fillCircle(icx, icy, 7, RGB565(226, 186, 96));
  gfx->fillCircle(icx, icy, 3, RGB565(170, 20, 45));
  gfx->drawPixel(icx - 1, icy - 1, RGB565(240, 150, 170));
  gfx->flush();
  clkCostMs += millis() - t0;
  if (++clkFrames % 400 == 0) {                          // every ~20 s
    Serial.printf("clock  : %02d:%02d:%02d%s, frame %lu ms avg\n", hh, mm, ss, synced ? "" : " (not synced)",
                  (unsigned long)(clkCostMs / 400));
    clkCostMs = 0;
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
  clkFrames = 0; clkCostMs = 0;
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
