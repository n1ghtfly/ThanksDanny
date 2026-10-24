/*
 * Oracle (v3.18, v3.19: bigger text + new quotes) - "Network Socrates", a magic 8-ball for network engineers.
 *
 * Tap the sphere or SHAKE the badge (the QMI8658's accelerometer): the marbled blue sphere spins
 * and swirls, then a dark window opens in the middle and a sarcastic network aphorism / excuse /
 * Socratic rebuke floats up into it. HOME (bottom) or BOOT = home screen.
 *
 * The sphere is drawn without any per-frame maths: on first use a 256x256 marble texture is
 * generated (8 brightness levels + 2 highlight levels, ~1.3 MB of PSRAM) and a table maps every
 * pixel of the disc to a texture spot and a shade (longitude/latitude of a lit sphere). A frame is
 * then one table walk: texture[shade][v][u + spin + twist(v)] - spinning shifts u, swirling twists
 * each row by a different amount.
 */

#define ORA_R        180                  // sphere radius
#define ORA_CX       233
#define ORA_CY       236
#define ORA_WIN      162                  // answer window radius (v3.19: was 118)
#define ORA_TXT_R    150                  // text stays inside this radius
#define ORA_SPIN_MS  1600                 // how long it swirls before answering
#define ORA_SHADES   10
#define ORA_HOME_Y   432

static const char *ORA_SAYS[] = {
  // Epistemology & pure Socratic scorn
  "I know only one thing: that you forgot 'commit confirmed'.",
  "Did you ping before you thought, or did you think before you pinged?",
  "The SFP is seated only in your mind.",
  "He who trusts an empty '/var/log' has never known inodes.",
  "To blame the firewall without looking at the route is to fear shadows in a cave.",
  "An unlogged maintenance window is a life lived in deceit.",
  "Is the box truly dead, or does your packet merely lack the will to find it?",
  "Speak not to me of 99.999% uptime; speak to me of your last tested backup.",
  "The unexamined config is not worth running.",
  "You stare at Wireshark as a child stares at thunder: full of fear, understanding nothing.",
  // Hardware, optics & layer 1 truths
  "Go forth and push the transceiver another three millimeters.",
  "A clean ferrule brings clarity to the soul; wipe the fiber.",
  "The rack door was shut with malice. Layer 1 is broken.",
  "Do not mistake an amber LED for the anger of the gods; your cable is loose.",
  "He who searches for BGP neighbors on an unseated patch lead seeks fish in a tree.",
  "The PDU hath given, and the cleaning crew hath unplugged.",
  "Thy serial cable resteth in 'AUX 0'. The CLI shall remain silent.",
  "An SFP shines in the dark, yet no one is there to receive its light.",
  // Routing, protocols & illusions
  "It is DNS. It has always been DNS. It shall forever be DNS.",
  "Your default gateway knoweth you not.",
  "Greed for jumbo frames hath brought down a plague of path MTU drops.",
  "A '/16' upon five office hosts is not benevolence, but madness.",
  "Why seekest thou the route target in an empty VRF?",
  "The Spanning Tree emperor hath fallen to a five-euro desktop switch.",
  "The carrier whispered 'zero-impact maintenance', and the BGP session wept.",
  "Thou hast redistributed the lab into the core. Repent.",
  "The loopback exists, yet it rejects your existence.",
  // Operating systems & the daemons
  "SELinux woke up, observed your daemon, and chose violence.",
  "'systemd' giveth life, and 'systemd' taketh away without remorse.",
  "Seek the ghost of 'dnf.pid', for it still walketh among the living.",
  "The OOM killer smelled your telemetry script from three subnets away.",
  "NetworkManager hath seen your static routes, and hath cast them into the sea.",
  "A reboot will heal the device, but reveal the config you failed to save.",
  // Direct oracular decrees
  "SIGNS POINT TO DNS.",
  "LAYER 1 LAUGHS AT THEE.",
  "SEEK CONSOLE. DESPAIR.",
  "NO ROUTE TO HOST.",
  "CHECK THINE ARP TABLE.",
  "THE CARRIER LIED.",
  "SELINUX SAYS NO.",
  "COMMIT WAS NOT SAVED.",
  "THE BUFFER OVERFLOWETH.",
  "INODES EXHAUSTED.",
};
#define ORA_N (int)(sizeof(ORA_SAYS) / sizeof(ORA_SAYS[0]))

static uint16_t *oraTex = nullptr;        // ORA_SHADES x 256 x 256, PSRAM
static uint32_t *oraMap = nullptr;        // one entry per disc pixel: u | v<<8 | shade<<16
static int       oraMapN = 0;
static int8_t    oraTwist[256];           // swirl amount per texture row
static int       oraLast = -1;            // last answer, never repeated twice in a row
static int       oraNow = -1;             // the answer on screen (-1 = the "ask me" screen)
static uint32_t  oraReadyAt = 0;          // shake cooldown
static uint32_t  oraLastShake = 0;
static int       oraSpin = 0;

// ---------------------------------------------------------------- the sphere

static bool oraBuild() {
  if (oraTex && oraMap) return true;
  uint32_t t0 = millis();
  if (!oraTex) oraTex = (uint16_t *)ps_malloc((size_t)ORA_SHADES * 256 * 256 * 2);
  if (!oraMap) oraMap = (uint32_t *)ps_malloc((size_t)(3.15f * ORA_R * ORA_R + 4 * ORA_R) * 4);
  if (!oraTex || !oraMap) { Serial.println("oracle : ! no PSRAM for the sphere"); return false; }

  // Marble: wavy bands, tileable in u (all u frequencies are whole turns of 256).
  const float K = 2.0f * PI / 256.0f;
  for (int v = 0; v < 256; v++) {
    oraTwist[v] = (int8_t)(sinf(v * K * 2.0f) * 60.0f);
    for (int u = 0; u < 256; u++) {
      float w = sinf(u * K * 3 + 2.6f * sinf(v * K * 2 + 1.7f * sinf(u * K * 2)) + 1.2f * sinf(v * K * 5));
      float t = 0.5f + 0.5f * w;                               // 0..1
      float vein = powf(fabsf(sinf(u * K * 5 + v * K * 3 + 3.0f * t)), 18.0f);   // thin light veins
      float r = 6 + 40 * t + 170 * vein, g = 18 + 90 * t + 190 * vein, b = 60 + 170 * t + 120 * vein;
      if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
      for (int s = 0; s < ORA_SHADES; s++) {
        float f = s < 8 ? 0.22f + 0.78f * s / 7.0f : 1.0f;
        float add = s == 8 ? 60 : s == 9 ? 140 : 0;                // highlight levels
        int R = (int)(r * f + add), G = (int)(g * f + add), B = (int)(b * f + add);
        if (R > 255) R = 255; if (G > 255) G = 255; if (B > 255) B = 255;
        oraTex[((size_t)s << 16) | (v << 8) | u] = RGB565(R, G, B);
      }
    }
  }
  // Disc -> (u, v, shade): longitude / latitude of a sphere lit from the upper left, front.
  const float lx = -0.45f, ly = -0.55f, lz = 0.70f;
  oraMapN = 0;
  for (int y = -ORA_R; y <= ORA_R; y++) {
    int span = (int)sqrtf((float)(ORA_R * ORA_R - y * y));
    for (int x = -span; x <= span; x++) {
      float nx = (float)x / ORA_R, ny = (float)y / ORA_R, nz2 = 1.0f - nx * nx - ny * ny;
      float nz = nz2 > 0 ? sqrtf(nz2) : 0;
      int u = (int)(atan2f(nx, nz) / PI * 128.0f) & 255;
      int v = (int)((asinf(ny) / (PI / 2) + 1.0f) * 127.5f);
      if (v < 0) v = 0; if (v > 255) v = 255;
      float lam = nx * lx + ny * ly + nz * lz;
      if (lam < 0) lam = 0;
      int shade = (int)(lam * 7.99f);
      float hx = nx - lx * 0.9f, hy = ny - ly * 0.9f;            // highlight spot
      float hd = hx * hx + hy * hy;
      if (hd < 0.010f) shade = 9; else if (hd < 0.035f) shade = 8;
      oraMap[oraMapN++] = (uint32_t)u | ((uint32_t)v << 8) | ((uint32_t)shade << 16);
    }
  }
  Serial.printf("oracle : sphere ready in %lu ms (%d px)\n", (unsigned long)(millis() - t0), oraMapN);
  return true;
}

// The sphere into the canvas. spin shifts every row; twist (0..256) swirls rows against each other.
static void oraSphere(int spin, int twist) {
  uint16_t *fb = gfx->getFramebuffer();
  const uint32_t *m = oraMap;
  for (int y = -ORA_R; y <= ORA_R; y++) {
    int span = (int)sqrtf((float)(ORA_R * ORA_R - y * y));
    uint16_t *row = fb + (size_t)(ORA_CY + y) * LCD_WIDTH + ORA_CX;
    for (int x = -span; x <= span; x++) {
      uint32_t e = *m++;
      int v = (e >> 8) & 255;
      int u = (int)(e & 255) + spin + ((oraTwist[v] * twist) >> 8);
      row[x] = oraTex[((e >> 16) << 16) | (v << 8) | (u & 255)];
    }
  }
}

static void oraHomePill() {
  gfx->fillRoundRect(188, ORA_HOME_Y, 90, 26, 10, RGB565(6, 10, 26));
  gfx->drawRoundRect(188, ORA_HOME_Y, 90, 26, 10, RGB565(70, 110, 150));
  centred(ORA_HOME_Y + 6, "HOME", 2, RGB565(220, 220, 230));
}

// Word-wrap s into at most maxN lines of font f (line pitch lh, capital height cap), where each
// line may only be as wide as the circle (radius ORA_TXT_R) is at that height. Tries 1, 2, ...
// lines until the text fits. Returns the line count, or 0 if it does not fit in this font.
static int oraWrap(const char *s, const GFXfont *f, int cap, int lh, int maxN, char lines[][64]) {
  gfx->setFont(f);
  gfx->setTextSize(1);
  for (int n = 1; n <= maxN; n++) {
    int top = -(n * lh) / 2 + (lh - cap) / 2;                    // cap top of line 0, relative to centre
    int li = 0;
    bool ok = true;
    char cur[64] = "";
    const char *p = s;
    while (*p && ok) {
      while (*p == ' ') p++;
      if (!*p) break;
      const char *e = p;
      while (*e && *e != ' ') e++;
      char word[48];
      int wl = (int)(e - p) < 47 ? (int)(e - p) : 47;
      memcpy(word, p, wl); word[wl] = 0;
      p = e;
      for (;;) {
        int y0 = top + li * lh, y1 = y0 + cap + 5;                // +5: descenders
        int dy = abs(y0) > abs(y1) ? abs(y0) : abs(y1);
        int maxW = dy < ORA_TXT_R ? (int)(2 * sqrtf((float)(ORA_TXT_R * ORA_TXT_R - dy * dy))) - 8 : 0;
        char test[64];
        snprintf(test, sizeof(test), cur[0] ? "%s %s" : "%s%s", cur, word);
        int16_t x1, yy; uint16_t w, h;
        gfx->getTextBounds(test, 0, 0, &x1, &yy, &w, &h);
        if ((int)w <= maxW) { snprintf(cur, sizeof(cur), "%s", test); break; }
        if (!cur[0]) { ok = false; break; }                       // one word wider than the line
        snprintf(lines[li++], 64, "%s", cur); cur[0] = 0;
        if (li >= n) { ok = false; break; }
      }
    }
    if (ok && cur[0]) snprintf(lines[li++], 64, "%s", cur);
    if (ok && li <= n) { gfx->setFont(); return li; }
  }
  gfx->setFont();
  return 0;
}

// The answer window: a dark disc with a thin rim, the text centred in it, faded by level (0..4).
// v3.19: the window fills most of the sphere; the biggest font that fits wins (title 26 px caps
// for the short decrees, 20 px for most quotes, 17 px as the last resort - no more tiny text).
static void oraWindow(const char *text, int level, bool ask) {
  gfx->fillCircle(ORA_CX, ORA_CY, ORA_WIN, RGB565(3, 6, 26));
  gfx->drawCircle(ORA_CX, ORA_CY, ORA_WIN, RGB565(40, 80, 150));
  gfx->drawCircle(ORA_CX, ORA_CY, ORA_WIN - 1, RGB565(20, 40, 90));
  int k = 40 + level * 53;                                       // fade: dim blue -> bright
  uint16_t c = RGB565(k * 7 / 10, k * 9 / 10, k);
  if (ask) {
    uiCentred(ORA_CY - 44, "ASK", &UiTitle, c);
    uiCentred(ORA_CY + 6, "tap or shake", &UiBig, RGB565(110, 140, 190));
    uiCentred(ORA_CY + 36, "the oracle", &UiBig, RGB565(110, 140, 190));
    return;
  }
  static const GFXfont *FONT[] = { &UiTitle, &UiOracle, &UiBig };
  static const int CAP[] = { UITITLE_CAP, UIORACLE_CAP, UIBIG_CAP };
  static const int LH[]  = { 38, 31, 27 };
  static const int MAXN[] = { 4, 6, 7 };
  char lines[7][64];
  int n = 0, fi = 0;
  for (fi = 0; fi < 3; fi++) if ((n = oraWrap(text, FONT[fi], CAP[fi], LH[fi], MAXN[fi], lines)) > 0) break;
  if (n == 0) { fi = 2; n = 1; snprintf(lines[0], 64, "%.20s", text); }   // never happens with ORA_SAYS
  int top = ORA_CY - (n * LH[fi]) / 2 + (LH[fi] - CAP[fi]) / 2 + (level < 4 ? (4 - level) * 4 : 0);   // floats up
  for (int i = 0; i < n; i++) uiCentred(top + i * LH[fi], lines[i], FONT[fi], c);
}

static void oraFrame(int spin, int twist, const char *text, int level, bool ask, bool window) {
  gfx->fillScreen(RGB565(2, 4, 14));
  uiCentred(20, "THE ORACLE", &UiSmall, RGB565(90, 120, 170));
  oraSphere(spin, twist);
  if (window) oraWindow(text, level, ask);
  oraHomePill();
}

void showOracle() {
  if (!oraBuild()) {
    gfx->fillScreen(RGB565_BLACK);
    centred(226, "no memory for the oracle", 2, RGB565(255, 120, 120));
    gfx->flush();
    return;
  }
  oraFrame(oraSpin, 0, oraNow >= 0 ? ORA_SAYS[oraNow] : "", 4, oraNow < 0, true);
  gfx->flush();
}

// Spin, swirl, pick an answer, let it float up. Blocks for ~2 s like the Ask Danny coin toss;
// BOOT still goes home.
static void oraAsk() {
  int pick;
  do { pick = esp_random() % ORA_N; } while (pick == oraLast && ORA_N > 1);
  oraLast = pick;
  sosSwish(150);                                                 // Sos.ino (respects mute)
  uint32_t t0 = millis();
  int frames = 0;
  while (millis() - t0 < ORA_SPIN_MS) {
    float t = (millis() - t0) / (float)ORA_SPIN_MS;              // 0..1
    float speed = 1.0f - t;                                      // fast, then slowing down
    oraSpin += (int)(4 + 14 * speed * speed * 2);
    int twist = (int)(256 * sinf(t * PI));                       // swirl in, then settle
    // the window shrinks away at the start
    oraFrame(oraSpin, twist, "", 0, false, false);
    if (t < 0.25f) {
      int r = (int)(ORA_WIN * (1.0f - t / 0.25f));
      if (r > 2) gfx->fillCircle(ORA_CX, ORA_CY, r, RGB565(3, 6, 26));
    }
    flushRect(0, ORA_CY - ORA_R, LCD_WIDTH, 2 * ORA_R + 2);
    frames++;
    if (bootButtonHit()) { showScreen(ST_HOME); return; }
  }
  oraNow = pick;
  // the window opens, the answer floats up and brightens
  for (int lvl = 0; lvl <= 4; lvl++) {
    oraFrame(oraSpin, 0, ORA_SAYS[pick], lvl, false, true);
    if (lvl == 0) {                                              // open from a dot
      for (int r = 20; r < ORA_WIN; r += 33) {
        oraFrame(oraSpin, 0, "", 0, false, false);
        gfx->fillCircle(ORA_CX, ORA_CY, r, RGB565(3, 6, 26));
        flushRect(0, ORA_CY - ORA_R, LCD_WIDTH, 2 * ORA_R + 2);
      }
      oraFrame(oraSpin, 0, ORA_SAYS[pick], lvl, false, true);
    }
    flushRect(0, ORA_CY - ORA_WIN - 2, LCD_WIDTH, 2 * ORA_WIN + 6);
    delay(60);
  }
  Serial.printf("oracle : \"%s\" (%d swirl frames, %lu ms)\n", ORA_SAYS[pick], frames, (unsigned long)(millis() - t0));
  oraReadyAt = millis() + 1200;
}

void oracleTap(int x, int y) {
  if (y >= ORA_HOME_Y - 10 && x > 160 && x < 306) { showScreen(ST_HOME); return; }
  oraAsk();
}

// From loop() while the oracle is up: a shake asks too.
void oracleTick() {
  if (millis() < oraReadyAt || millis() - oraLastShake < 20) return;
  oraLastShake = millis();
  float g = sosAccelG();                                         // Sos.ino; 0 = no new sample
  if (g > 0 && (g > 1.9f || g < 0.25f)) {
    Serial.printf("oracle : shaken (%.1f g)\n", g);
    oraAsk();
  }
}

// The carousel icon: an 8-ball - dark ball, white disc, an "8".
void drawOracleIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  gfx->fillCircle(cx, cy, r, RGB565(20, 40, 90));
  gfx->drawCircle(cx, cy, r, fg);
  gfx->fillCircle(cx - r / 5, cy - r / 5, r / 2, RGB565(235, 240, 250));
  int s = r / 7 + 1;
  gfx->drawCircle(cx - r / 5, cy - r / 5 - s, s, RGB565(10, 20, 50));
  gfx->drawCircle(cx - r / 5, cy - r / 5 + s, s + 1, RGB565(10, 20, 50));
  if (r > 30) {
    gfx->drawCircle(cx - r / 5, cy - r / 5 - s, s - 1, RGB565(10, 20, 50));
    gfx->drawCircle(cx - r / 5, cy - r / 5 + s, s, RGB565(10, 20, 50));
  }
}
