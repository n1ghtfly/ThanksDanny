/*
 * Screensaver (v3.2) - kind to the AMOLED and to the battery.
 *
 * After SAVER_AFTER_MS without a touch the badge dims and shows a black screen with a clock,
 * the date and the three Sons of Sudo (a dot each: lit = online). The whole block moves to a new
 * spot every minute, so nothing sits still long enough to burn in - on an AMOLED black pixels
 * are simply off. After SAVER_OFF_MS more, the panel goes to sleep altogether.
 *
 * Wakes on: a tap (that tap is swallowed - it does not also press whatever is under it), BOOT,
 * picking the badge up (the gyro), or an incoming slap (which takes over the screen as usual).
 * A tap/BOOT/lift brings back exactly the screen that was up: the frame is copied to PSRAM when
 * the saver starts and copied back on wake, so no app has to know how to redraw itself.
 *
 * Only starts on screens that are just sitting there: home, settings, a picture, a decider
 * result, a slap you received, Rumours when nothing plays, and SLAP while choosing a victim
 * (never while armed for a swing). Not on the setup-network page or during a picture sync.
 *
 * Hooks in Badge.ino: loop() hands over to saverLoop() while the saver is up and calls
 * saverCheck() otherwise; pollTouch() calls saverPoke(); showScreen() asks saverIntercept().
 */

#define SAVER_AFTER_MS   60000UL          // idle time before the saver starts
#define SAVER_OFF_MS     300000UL         // then this long before the panel sleeps
#define SAVER_BRIGHT     40               // dimmed panel brightness (0-255)
#define SAVER_NORMAL     170              // keep equal to setBrightness() in Badge.ino setup()
#define SAVER_WAKE_DPS   120              // turning faster than this (picking it up) wakes it

#define SV_OFF   0
#define SV_ON    1
#define SV_SLEEP 2

static uint8_t   saverState = SV_OFF;
static uint32_t  saverLastInput = 0, saverSince = 0, saverLastCheck = 0, saverLastMotion = 0;
static uint16_t *saverFrame = nullptr;    // the screen that was up, kept in PSRAM (434 kB)
static bool      saverRedraw = false;     // that screen changed meanwhile: redraw instead of restore
static int       saverOx = 0, saverOy = 0, saverLastKey = -1, saverLastMask = -1;

void saverPoke()   { saverLastInput = millis(); }
bool saverActive() { return saverState != SV_OFF; }

static bool saverAllowed() {
  if (touchDown) return false;
  switch (screen) {
    case ST_HOME: case ST_MENU: case ST_GALLERY: case ST_DECIDER: case ST_SLAPPED: return true;
    case ST_RUMOURS: return !rumBusy;
    case ST_SLAP:    return slapIdle();
    default:         return false;
  }
}

// ---------------------------------------------------------------- drawing

static void svText(int cx, int y, const char *s, uint8_t size, uint16_t c) {
  gfx->setTextSize(size);
  gfx->setTextColor(c);
  gfx->setCursor(cx - (int)strlen(s) * 3 * size, y);
  gfx->print(s);
}

// Seven-segment digits with rounded bars: reads better at this size than a scaled 6x8 font.
#define SV_DW 50
#define SV_DH 92
#define SV_DT 11
static void svSeg(int x, int y, int w, int h, uint16_t c) { gfx->fillRoundRect(x, y, w, h, (w < h ? w : h) / 2, c); }
static void svDigit(int x, int y, int d, uint16_t c) {
  static const uint8_t M[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };  // bits a..g
  const int W = SV_DW, H = SV_DH, T = SV_DT, hh = H / 2, vh = hh - T / 2 - 4;
  uint8_t m = M[d % 10];
  if (m & 0x01) svSeg(x + T / 2 + 2, y,              W - T - 4, T, c);    // a top
  if (m & 0x02) svSeg(x + W - T,     y + T / 2 + 2,  T, vh, c);           // b top right
  if (m & 0x04) svSeg(x + W - T,     y + hh + 2,     T, vh, c);           // c bottom right
  if (m & 0x08) svSeg(x + T / 2 + 2, y + H - T,      W - T - 4, T, c);    // d bottom
  if (m & 0x10) svSeg(x,             y + hh + 2,     T, vh, c);           // e bottom left
  if (m & 0x20) svSeg(x,             y + T / 2 + 2,  T, vh, c);           // f top left
  if (m & 0x40) svSeg(x + T / 2 + 2, y + hh - T / 2, W - T - 4, T, c);    // g middle
}

static int svPresenceMask() {
  int m = 0;
  for (int i = 0; i < 3; i++) if (sosIsOnline(i)) m |= 1 << i;
  return m;
}

static void saverNewSpot() {
  saverOx = (int)(esp_random() % 61) - 30;
  saverOy = (int)(esp_random() % 61) - 30;
}

static void saverDraw() {
  static const char *DAY[7]  = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
  static const char *MON[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
  const uint16_t dim = RGB565(110, 110, 135), clock = RGB565(90, 200, 255);
  gfx->fillScreen(RGB565_BLACK);
  int cx = LCD_WIDTH / 2 + saverOx, top = 130 + saverOy;
  svText(cx, top, "SONS OF SUDO", 2, dim);

  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int y = top + 40;
  if (t.tm_year >= 124) {                                    // the clock is set (SNTP after Wi-Fi)
    const int gap = 14, colon = 30, total = 4 * SV_DW + 2 * gap + colon;
    int x = cx - total / 2;
    svDigit(x, y, t.tm_hour / 10, clock);  x += SV_DW + gap;
    svDigit(x, y, t.tm_hour % 10, clock);  x += SV_DW;
    gfx->fillCircle(x + colon / 2, y + SV_DH * 3 / 10, 6, clock);
    gfx->fillCircle(x + colon / 2, y + SV_DH * 7 / 10, 6, clock);
    x += colon;
    svDigit(x, y, t.tm_min / 10, clock);   x += SV_DW + gap;
    svDigit(x, y, t.tm_min % 10, clock);
    char date[24];
    snprintf(date, sizeof(date), "%s %d %s", DAY[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
    svText(cx, y + SV_DH + 18, date, 2, dim);
  } else {
    svText(cx, y + 20, "zzz", 7, clock);                     // no time yet (no Wi-Fi)
  }

  // The three of us: a lit dot = online.
  static const char *INI[3] = { "C", "D", "W" };
  int dy = y + SV_DH + 66;
  for (int i = 0; i < 3; i++) {
    int dx = cx + (i - 1) * 46;
    bool on = sosIsOnline(i);
    if (on) gfx->fillCircle(dx, dy, 15, RGB565(40, 190, 90));
    else    gfx->drawCircle(dx, dy, 15, RGB565(70, 70, 80));
    svText(dx + 1, dy - 7, INI[i], 2, on ? RGB565_BLACK : RGB565(90, 90, 100));
  }
}

// ---------------------------------------------------------------- on / off

static void saverEnter() {
  if (!saverFrame) saverFrame = (uint16_t *)ps_malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2);
  uint16_t *fb = gfx->getFramebuffer();
  saverRedraw = !(saverFrame && fb);
  if (!saverRedraw) memcpy(saverFrame, fb, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);
  saverState = SV_ON;
  saverSince = saverLastCheck = millis();
  saverNewSpot();
  time_t tt = time(nullptr);
  struct tm t;
  localtime_r(&tt, &t);
  saverLastKey = t.tm_hour * 60 + t.tm_min;
  saverLastMask = svPresenceMask();
  panel->setBrightness(SAVER_BRIGHT);
  saverDraw();
  gfx->flush();
  Serial.printf("saver  : on (screen %d idle for %lu s)\n", screen, SAVER_AFTER_MS / 1000);
}

// restore = put the old screen back; false when the caller is about to draw a new one (a slap).
void saverWake(bool restore, const char *why) {
  if (!saverState) return;
  bool slept = (saverState == SV_SLEEP);
  saverState = SV_OFF;                       // first, so the showScreen() below is a normal one
  saverPoke();
  if (slept) panel->displayOn();
  if (restore) {
    panel->setBrightness(0);                 // do not flash the clock at full brightness
    uint16_t *fb = gfx->getFramebuffer();
    if (saverRedraw || !saverFrame || !fb) showScreen(screen);
    else { memcpy(fb, saverFrame, (size_t)LCD_WIDTH * LCD_HEIGHT * 2); gfx->flush(); }
  }
  saverRedraw = false;
  panel->setBrightness(SAVER_NORMAL);
  Serial.printf("saver  : off - %s%s\n", why, slept ? " (panel was asleep)" : "");
}

// Called by showScreen(). True = skip this draw (it was only a refresh of the hidden screen).
bool saverIntercept(int s) {
  if (!saverState) {
    if (s != screen) saverPoke();            // a new screen counts as activity
    return false;
  }
  if (s == screen && s != ST_SLAPPED) {      // e.g. a presence change refreshing home
    if (s == ST_HOME || s == ST_MENU || s == ST_SLAP) saverRedraw = true;
    return true;
  }
  saverWake(false, "new screen");            // a slap arrived: wake up and let it take over
  return false;
}

// While idle on a quiet screen: start the saver when it is time.
void saverCheck() {
  if (millis() - saverLastInput < SAVER_AFTER_MS) return;
  if (!saverAllowed()) { saverPoke(); return; }   // busy screens restart the count
  saverEnter();
}

// While the saver is up: watch for a reason to wake, keep the clock current, sleep the panel.
void saverLoop() {
  uint32_t now = millis();
  int16_t xs[1], ys[1];
  bool touched = touchOK && touch.getPoint(xs, ys, 1) > 0;
  bool boot = bootButtonHit();
  bool lifted = false;
  if (now - saverLastMotion >= 50) { saverLastMotion = now; lifted = sosTurnRate() > SAVER_WAKE_DPS; }

  if (touched || boot || lifted) {
    saverWake(true, touched ? "tap" : boot ? "BOOT" : "picked up");
    if (touched) {                           // swallow the waking finger until it lifts
      uint32_t last = millis(), t0 = last;
      while (millis() - last < 150 && millis() - t0 < 3000) {
        if (touch.getPoint(xs, ys, 1) > 0) last = millis();
        delay(5);
      }
      lastTap = millis();
    }
    return;
  }

  if (saverState == SV_ON && now - saverSince >= SAVER_OFF_MS) {
    panel->displayOff();
    saverState = SV_SLEEP;
    Serial.println("saver  : panel asleep");
    return;
  }

  if (saverState == SV_ON && now - saverLastCheck >= 1000) {
    saverLastCheck = now;
    time_t tt = time(nullptr);
    struct tm t;
    localtime_r(&tt, &t);
    int key = t.tm_hour * 60 + t.tm_min, mask = svPresenceMask();
    if (key != saverLastKey || mask != saverLastMask) {
      if (key != saverLastKey && saverLastKey >= 0) saverNewSpot();   // move every minute
      saverLastKey = key;
      saverLastMask = mask;
      saverDraw();
      gfx->flush();
    }
  }
}
