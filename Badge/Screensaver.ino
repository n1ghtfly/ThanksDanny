/*
 * Screensaver (v3.3: SOS MARKETS) - a Bloomberg-style market screen, kind to the AMOLED and the battery.
 *
 * After SAVER_AFTER_MS without a touch the badge dims and shows:
 *   - a header: "SOS MARKETS 20:37";
 *   - one big MAIN QUOTE in the centre - symbol, price, change and a one-line analyst quip - which
 *     stays put and moves on to the next index every SV_QUOTE_MS;
 *   - a CRAWL along the bottom edge: an amber band scrolling every index and the odd headline.
 * The prices are a random walk with a personality per index ($DANNY only ever goes up, $MEETING
 * only down, $COFFEE is permanently LIMIT DOWN). Some are live: $CLAUDIO/$DANNY/$WALTER show
 * MKT CLOSED while that badge is offline, $SLAP is this badge's real slap count, and $BEER is in
 * PRE-MARKET until 17:00.
 *
 * Burn-in: everything except the crawl band moves to a random spot within +-30 px every minute,
 * the crawl itself never stands still, and the panel goes to sleep after SAVER_OFF_MS anyway.
 * The crawl is cheap: a 466x30 strip is redrawn and sent with flushRect() ~30 times a second.
 *
 * Wakes on: a tap (swallowed - it does not also press whatever is under it), BOOT, picking the
 * badge up (the gyro), or an incoming slap (which takes over the screen as usual). Tap/BOOT/lift
 * bring back exactly the screen that was up: the frame is copied to PSRAM when the saver starts
 * and copied back on wake, so no app has to know how to redraw itself.
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

#define SV_QUOTE_MS      12000UL          // main quote: next index this often (prices tick too)
#define SV_CRAWL_MS      33               // crawl frame time (~30 fps)
#define SV_CRAWL_PX      2                // pixels per frame -> ~60 px/s, ~5 characters a second
#define SV_BAND_Y        372              // the crawl band: on a round panel the bottom edge is a
#define SV_BAND_H        30               //   chord ~340 px wide here, text slides in from the curve

#define SV_OFF   0
#define SV_ON    1
#define SV_SLEEP 2

static uint8_t   saverState = SV_OFF;
static uint32_t  saverLastInput = 0, saverSince = 0, saverLastCheck = 0, saverLastMotion = 0;
static uint32_t  saverLastQuote = 0, saverLastCrawl = 0;
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

// ---------------------------------------------------------------- the market

#define SV_AMBER RGB565(255, 160, 0)
#define SV_GREEN RGB565(0, 220, 90)
#define SV_RED   RGB565(255, 60, 60)
#define SV_GREY  RGB565(130, 130, 140)
#define SV_WHITE RGB565(235, 235, 235)
#define SV_UP    "\x1E"                   // the built-in font's up/down triangles (CP437 30/31)
#define SV_DOWN  "\x1F"

enum { SK_WALK, SK_MEMBER, SK_STATUS, SK_SLAP, SK_BEER };

struct SvStock {
  const char *sym;
  float open, price;
  float bias, vol;          // per tick: drift and noise, as a fraction of the price
  uint8_t kind;
  int8_t member;            // SK_MEMBER: index into Sos.ino's members (0 claudio, 1 danny, 2 walter)
  const char *status;       // SK_STATUS: shown instead of the change
  const char *quip;         // under the main quote
};

static SvStock SV[] = {
  { "$SUDO",    420.69, 420.69,  0.0010, 0.004, SK_WALK,   -1, nullptr,       "Sons of Sudo Composite" },
  { "$DANNY",  1337.00, 1337.00, 0.0040, 0.002, SK_MEMBER,  1, nullptr,       "analysts: still a legend" },
  { "$COFFEE",    3.20,   0.32,  0,      0,     SK_STATUS, -1, "LIMIT DOWN",  "machine empty again" },
  { "$CLAUDIO", 256.00, 256.00,  0.0005, 0.005, SK_MEMBER,  0, nullptr,       "steady, lightly caffeinated" },
  { "$BUGS",    404.00, 404.00,  0.0080, 0.006, SK_WALK,   -1, nullptr,       "to the moon" },
  { "$WALTER",  314.15, 314.15,  0.0000, 0.008, SK_MEMBER,  2, nullptr,       "volatile after lunch" },
  { "$MEETING",   1.00,   0.12, -0.0200, 0.010, SK_WALK,   -1, nullptr,       "could have been an email" },
  { "$SLAP",      0,      0,     0,      0,     SK_SLAP,   -1, nullptr,       "slaps received, all time" },
  { "$DEPLOY",   99.00,  99.00,  0,      0,     SK_STATUS, -1, "HALTED",      "friday freeze in effect" },
  { "$BEER",      4.50,   4.50,  0.0030, 0.004, SK_BEER,   -1, nullptr,       "happy hour rally" },
  { "$RUMOUR",   66.60,  66.60,  0,      0,     SK_STATUS, -1, "UNCONFIRMED", "sources close to Danny" },
};
#define SV_N (int)(sizeof(SV) / sizeof(SV[0]))

static const char *SV_NEWS[] = {
  "BREAKING: DANNY APPROVES EVERYTHING",
  "FED HOLDS COFFEE RATE AT ZERO",
  "SEC PROBES WALTER'S SNACK DRAWER",
  "CLAUDIO ISSUES PROFIT WARNING ON SLEEP",
  "RUMOURS SPIKE AFTER LEAKED MP3",
  "DECIDER OUTLOOK: MAYBE",
  "SUDO GRANTS ITSELF A BONUS",
};
#define SV_NEWS_N (int)(sizeof(SV_NEWS) / sizeof(SV_NEWS[0]))

static int svMain = 0;                    // the index shown in the centre
static int svNews = 0;                    // next headline for the crawl

static float svRand() { return (float)(esp_random() % 20001) / 10000.0f - 1.0f; }   // -1..1

static int svHour() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return t.tm_year >= 124 ? t.tm_hour : -1;
}

// One market tick: every walking index takes a step.
static void svTick() {
  for (int i = 0; i < SV_N; i++) {
    SvStock &s = SV[i];
    if (s.kind == SK_STATUS || s.kind == SK_SLAP) continue;
    s.price *= 1.0f + s.bias + s.vol * svRand();
    if (s.price < s.open * 0.001f) s.price = s.open * 0.001f;     // $MEETING bottoms out at -99.9%
  }
}

// The text for one index: price, change (or a status), and the colour of the change.
static void svQuote(int i, char *price, size_t pn, char *chg, size_t cn, uint16_t *col) {
  const SvStock &s = SV[i];
  int h = svHour();
  if (s.kind == SK_MEMBER && !sosIsOnline(s.member)) {
    snprintf(price, pn, "%.2f", s.price);
    snprintf(chg, cn, "MKT CLOSED");
    *col = SV_GREY;
    return;
  }
  if (s.kind == SK_BEER && h >= 0 && h < 17) {
    snprintf(price, pn, "%.2f", s.price);
    snprintf(chg, cn, "PRE-MARKET");
    *col = SV_GREY;
    return;
  }
  if (s.kind == SK_STATUS) {
    snprintf(price, pn, "%.2f", s.price);
    snprintf(chg, cn, "%s", s.status);
    *col = !strcmp(s.status, "LIMIT DOWN") ? SV_RED : SV_AMBER;
    return;
  }
  if (s.kind == SK_SLAP) {
    uint32_t n = sosSlapsGot();
    snprintf(price, pn, "%lu", (unsigned long)n);
    snprintf(chg, cn, n ? "ALL-TIME HIGH" : "NO VOLUME");
    *col = n ? SV_GREEN : SV_GREY;
    return;
  }
  float pct = (s.price / s.open - 1.0f) * 100.0f;
  snprintf(price, pn, "%.2f", s.price);
  if (pct >= 0) { snprintf(chg, cn, SV_UP "+%.1f%%", pct); *col = SV_GREEN; }
  else          { snprintf(chg, cn, SV_DOWN "%.1f%%", pct); *col = SV_RED; }
}

// ---------------------------------------------------------------- the crawl

// The crawl is a row of coloured pieces of text, rebuilt (with fresh prices) every time it has
// scrolled all the way through.
#define SV_SEGS 64
static char     svSegText[SV_SEGS][44];
static uint16_t svSegCol[SV_SEGS];
static int      svSegN = 0, svCrawlW = 0, svCrawlX = LCD_WIDTH;

static void svSeg(const char *s, uint16_t c) {
  if (svSegN >= SV_SEGS) return;
  snprintf(svSegText[svSegN], sizeof(svSegText[0]), "%s", s);
  svSegCol[svSegN] = c;
  svCrawlW += strlen(svSegText[svSegN]) * 12;               // text size 2: 12 px per character
  svSegN++;
}

static void svBuildCrawl() {
  svSegN = 0;
  svCrawlW = 0;
  char price[16], chg[20], buf[44];
  uint16_t col;
  for (int i = 0; i < SV_N; i++) {
    svQuote(i, price, sizeof(price), chg, sizeof(chg), &col);
    snprintf(buf, sizeof(buf), "%s ", SV[i].sym);   svSeg(buf, SV_AMBER);
    snprintf(buf, sizeof(buf), "%s ", price);       svSeg(buf, SV_WHITE);
    snprintf(buf, sizeof(buf), "%s   ", chg);       svSeg(buf, col);
    if (i % 4 == 3) {                                // a headline every few quotes
      snprintf(buf, sizeof(buf), "%s   ", SV_NEWS[svNews]);
      svSeg(buf, SV_WHITE);
      svNews = (svNews + 1) % SV_NEWS_N;
    }
  }
  svCrawlX = LCD_WIDTH;                              // enters from the right edge
}

static void svDrawCrawl() {
  gfx->fillRect(0, SV_BAND_Y, LCD_WIDTH, SV_BAND_H, RGB565(28, 18, 0));
  gfx->drawFastHLine(0, SV_BAND_Y, LCD_WIDTH, SV_AMBER);
  gfx->setTextSize(2);
  int x = svCrawlX;
  for (int i = 0; i < svSegN && x < LCD_WIDTH; i++) {
    int w = strlen(svSegText[i]) * 12;
    if (x + w > 0) {
      gfx->setTextColor(svSegCol[i]);
      gfx->setCursor(x, SV_BAND_Y + 8);
      gfx->print(svSegText[i]);
    }
    x += w;
  }
}

// ---------------------------------------------------------------- the screen

static void svText(int cx, int y, const char *s, uint8_t size, uint16_t c) {
  gfx->setTextSize(size);
  gfx->setTextColor(c);
  gfx->setCursor(cx - (int)strlen(s) * 3 * size, y);
  gfx->print(s);
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
  gfx->fillScreen(RGB565_BLACK);
  int cx = LCD_WIDTH / 2 + saverOx, oy = saverOy;

  // Header: SOS MARKETS and the time (just the name until Wi-Fi has set the clock).
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char head[32];
  if (t.tm_year >= 124) snprintf(head, sizeof(head), "SOS MARKETS %02d:%02d", t.tm_hour, t.tm_min);
  else                  snprintf(head, sizeof(head), "SOS MARKETS");
  svText(cx, 100 + oy, head, 2, SV_AMBER);
  gfx->drawFastHLine(cx - 110, 124 + oy, 220, RGB565(90, 60, 0));

  // The main quote.
  char price[16], chg[20];
  uint16_t col;
  svQuote(svMain, price, sizeof(price), chg, sizeof(chg), &col);
  svText(cx, 146 + oy, SV[svMain].sym, 4, SV_AMBER);
  svText(cx, 194 + oy, price, strlen(price) > 8 ? 4 : 5, SV_WHITE);
  svText(cx, 252 + oy, chg, strlen(chg) > 11 ? 2 : 3, col);
  svText(cx, 290 + oy, SV[svMain].quip, 2, SV_GREY);

  // Market breadth, Bloomberg-style: how many are up / down right now.
  int up = 0, down = 0;
  for (int i = 0; i < SV_N; i++) {
    char p[16], c[20]; uint16_t k;
    svQuote(i, p, sizeof(p), c, sizeof(c), &k);
    if (k == SV_GREEN) up++; else if (k == SV_RED) down++;
  }
  char breadth[32];
  snprintf(breadth, sizeof(breadth), SV_UP "%d  " SV_DOWN "%d", up, down);
  svText(cx, 322 + oy, breadth, 2, RGB565(90, 90, 100));

  svDrawCrawl();
}

// ---------------------------------------------------------------- on / off

static void saverEnter() {
  if (!saverFrame) saverFrame = (uint16_t *)ps_malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2);
  uint16_t *fb = gfx->getFramebuffer();
  saverRedraw = !(saverFrame && fb);
  if (!saverRedraw) memcpy(saverFrame, fb, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);
  saverState = SV_ON;
  saverSince = saverLastCheck = saverLastQuote = saverLastCrawl = millis();
  saverNewSpot();
  time_t tt = time(nullptr);
  struct tm t;
  localtime_r(&tt, &t);
  saverLastKey = t.tm_hour * 60 + t.tm_min;
  saverLastMask = svPresenceMask();
  svTick();
  svBuildCrawl();
  panel->setBrightness(SAVER_BRIGHT);
  saverDraw();
  gfx->flush();
  Serial.printf("saver  : SOS MARKETS on (screen %d idle for %lu s)\n", screen, SAVER_AFTER_MS / 1000);
}

// restore = put the old screen back; false when the caller is about to draw a new one (a slap).
void saverWake(bool restore, const char *why) {
  if (!saverState) return;
  bool slept = (saverState == SV_SLEEP);
  saverState = SV_OFF;                       // first, so the showScreen() below is a normal one
  saverPoke();
  if (slept) panel->displayOn();
  if (restore) {
    panel->setBrightness(0);                 // do not flash the market at full brightness
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

// While the saver is up: watch for a reason to wake, run the market, sleep the panel.
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
  if (saverState != SV_ON) return;           // asleep: nothing to draw

  if (now - saverSince >= SAVER_OFF_MS) {
    panel->displayOff();
    saverState = SV_SLEEP;
    Serial.println("saver  : panel asleep");
    return;
  }

  // Full redraw: next main quote (prices tick), a new minute (and a new spot), or presence.
  bool full = false;
  if (now - saverLastQuote >= SV_QUOTE_MS) {
    saverLastQuote = now;
    svMain = (svMain + 1) % SV_N;
    svTick();
    full = true;
  }
  if (now - saverLastCheck >= 1000) {
    saverLastCheck = now;
    time_t tt = time(nullptr);
    struct tm t;
    localtime_r(&tt, &t);
    int key = t.tm_hour * 60 + t.tm_min, mask = svPresenceMask();
    if (key != saverLastKey) { saverLastKey = key; saverNewSpot(); full = true; }
    if (mask != saverLastMask) { saverLastMask = mask; full = true; }
  }
  if (full) {
    saverDraw();
    gfx->flush();
    saverLastCrawl = now;
    return;
  }

  // The crawl: one strip, a couple of pixels further along.
  if (now - saverLastCrawl >= SV_CRAWL_MS) {
    saverLastCrawl = now;
    svCrawlX -= SV_CRAWL_PX;
    if (svCrawlX + svCrawlW < 0) svBuildCrawl();      // all the way through: fresh prices
    svDrawCrawl();
    flushRect(0, SV_BAND_Y, LCD_WIDTH, SV_BAND_H);
  }
}
