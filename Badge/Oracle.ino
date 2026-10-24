/*
 * Oracle (v3.18) - "Network Socrates", a magic 8-ball for network engineers.
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
#define ORA_WIN      118                  // answer window radius
#define ORA_SPIN_MS  1600                 // how long it swirls before answering
#define ORA_SHADES   10
#define ORA_HOME_Y   432

static const char *ORA_SAYS[] = {
  // the three that started it
  "Did you ping before you thought?",
  "It is DNS. It has always been DNS.",
  "The SFP is seated only in your mind.",
  // Socratic rebukes
  "What is a packet, if not a question nobody answered?",
  "You say it worked yesterday. Define yesterday.",
  "Know thyself. Then know thy default gateway.",
  "The unexamined config is not worth committing.",
  "I know that I know nothing. You know less: you did not check the logs.",
  "Is the link down, or is your understanding down?",
  "Who among us has truly read the release notes?",
  "If a switch reboots in an empty datacenter, does the NOC get paged?",
  "Have you considered that the cable is merely a metaphor?",
  "You ask why it fails. I ask why you expected otherwise.",
  "To err is human. To propagate it to 400 switches is automation.",
  "What you call a feature, the vendor calls undefined behaviour.",
  // the classics
  "It is always the firewall. Except when it is DNS.",
  "Have you tried turning off the spanning tree? (Do not.)",
  "Reply hazy. MTU mismatch likely.",
  "Ask again after the change window.",
  "Signs point to a duplex mismatch.",
  "Outlook not so good. Neither is Exchange.",
  "Without a doubt: layer 8.",
  "Concentrate and ask again. Preferably with a pcap.",
  "My sources say BGP. My sources are flapping.",
  "Cannot predict now. ARP cache is stale.",
  "Most likely. But only on the secondary.",
  "Yes, in production. No, in the lab.",
  "Better not tell you now. The NDA forbids it.",
  "Very doubtful. The ticket says 'no change'.",
  // excuses
  "Works on my VLAN.",
  "That port was never documented, therefore it does not exist.",
  "The vendor calls it a known issue. Known to whom?",
  "Someone plugged both ends into the same switch. Again.",
  "The fibre is fine. The cleaner is still in its bag.",
  "Latency is just the network thinking it over.",
  "The packet left. Where it went is a matter of faith.",
  "Have you checked the other end of the cable? There is always another end.",
  "Packet loss builds character.",
  "The certificate expired at 03:00 on a Sunday, as tradition demands.",
  "Someone ran 'debug all' on the core. Pray.",
  "That subnet overlaps with the VPN of your soul.",
  "The NTP server lies, and so does everything after it.",
  "QoS is just deciding whom to disappoint first.",
  "Yes, the wireless is slow. It is wireless.",
  "There is no cloud. Only someone else's switch.",
  "The change was minor. The outage was not.",
  "Somewhere, a loop. Somewhere, a broadcast storm. Somewhere, you.",
  "Rollback is also a strategy.",
  "Your ACL is correct. It is also in the wrong direction.",
  "Ping works, so the network is fine. Close the ticket.",
  "Have you tried a smaller MTU and a larger coffee?",
  "One does not simply reboot the core switch.",
  "The documentation is the running-config. Good luck.",
  "Ask the person who left last year. They knew.",
  "Trust, but traceroute.",
  "IPv6 will fix it. Next year. Every year.",
  "It is not a bug, it is an undocumented redundancy.",
  "All routes lead to 0.0.0.0/0 eventually.",
  "Danny approved it, therefore it is correct.",
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

// Word-wrap s into lines no wider than maxW in font f. Returns the line count (max 6);
// oraWrapTooWide is set when a single word is wider than maxW on its own.
static bool oraWrapTooWide = false;
static int oraWrap(const char *s, const GFXfont *f, int maxW, char lines[][64]) {
  int n = 0;
  char cur[64] = "";
  const char *p = s;
  oraWrapTooWide = false;
  gfx->setFont(f);
  gfx->setTextSize(1);
  while (*p && n < 6) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    char word[48];
    int wl = (int)(e - p) < 47 ? (int)(e - p) : 47;
    memcpy(word, p, wl); word[wl] = 0;
    char test[64];
    snprintf(test, sizeof(test), cur[0] ? "%s %s" : "%s%s", cur, word);
    int16_t x1, y1; uint16_t w, h;
    gfx->getTextBounds(test, 0, 0, &x1, &y1, &w, &h);
    if ((int)w > maxW && cur[0]) {
      snprintf(lines[n++], 64, "%s", cur); snprintf(cur, sizeof(cur), "%s", word);
      gfx->getTextBounds(cur, 0, 0, &x1, &y1, &w, &h);
      if ((int)w > maxW) oraWrapTooWide = true;
    } else if ((int)w > maxW) oraWrapTooWide = true;
    else snprintf(cur, sizeof(cur), "%s", test);
    p = e;
  }
  if (cur[0] && n < 6) snprintf(lines[n++], 64, "%s", cur);
  gfx->setFont();
  return n;
}

// The answer window: a dark disc with a thin rim, the text centred in it, faded by level (0..4).
static void oraWindow(const char *text, int level, bool ask) {
  gfx->fillCircle(ORA_CX, ORA_CY, ORA_WIN, RGB565(3, 6, 26));
  gfx->drawCircle(ORA_CX, ORA_CY, ORA_WIN, RGB565(40, 80, 150));
  gfx->drawCircle(ORA_CX, ORA_CY, ORA_WIN - 1, RGB565(20, 40, 90));
  int k = 40 + level * 53;                                       // fade: dim blue -> bright
  uint16_t c = RGB565(k * 7 / 10, k * 9 / 10, k);
  if (ask) {
    uiCentred(ORA_CY - 34, "ASK", &UiTitle, c);
    uiCentred(ORA_CY + 8, "tap or shake", &UiSmall, RGB565(110, 140, 190));
    uiCentred(ORA_CY + 26, "the oracle", &UiSmall, RGB565(110, 140, 190));
    return;
  }
  char lines[6][64];
  const GFXfont *f = &UiBig;
  int n = oraWrap(text, f, 2 * ORA_WIN - 50, lines), lh = 26;
  if (n > 4 || oraWrapTooWide) { f = &UiSmall; n = oraWrap(text, f, 2 * ORA_WIN - 40, lines); lh = 17; }
  int top = ORA_CY - (n * lh) / 2 + (level < 4 ? (4 - level) * 4 : 0);   // floats up as it fades in
  for (int i = 0; i < n; i++) uiCentred(top + i * lh, lines[i], f, c);
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
