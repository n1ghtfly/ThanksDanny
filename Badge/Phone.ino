/*
 * Phone (v3.38) - the MUSIC app (the badge as a Bluetooth media remote for your phone) and iPhone
 * notifications popping up on the badge. The Bluetooth work is in src/phonelink.cpp.
 *
 * Bluetooth is OFF until MUSIC is opened the first time; from then on it starts at every boot
 * (NVS "phone"/"bt") until it is switched off on the MUSIC screen (fully off after the next restart).
 * Pair the badge in the phone's Bluetooth settings as "SoS <NAME>". iPhone: after pairing, allow
 * "Share System Notifications" for it when iOS asks (Settings > Bluetooth > (i) next to the badge).
 *
 * MUSIC screen: play/pause (big), previous / next, volume down / mute / volume up - they act on
 * whatever is playing on the phone. Taps fire on the press; the button flashes.
 * Notifications: a full-screen card - app, title, message (ASCII: accents dropped, emoji skipped),
 * a soft ping, 8 s (calls 15 s) or until tapped; then the badge goes back to where it was.
 */

#include "src/phonelink.h"

#define MUS_PLAY_X 233
#define MUS_ROW1_Y 222
#define MUS_PLAY_R 60
#define MUS_SIDE_R 42
static const int MUS_SIDE_X[2] = { 107, 359 };
#define MUS_ROW2_Y 340
static const int MUS_VOL_X[3] = { 153, 233, 313 };
#define MUS_VOL_R  32
#define MUS_BT_Y   394
#define MUS_HOME_Y 428

static bool phoneBtWanted = false;
static uint32_t musStatusAt = 0;
static int musLastState = -1;

// notification card
static char ntApp[24], ntTitle[48], ntMsg[140];
static int ntCat = 0;
static int ntPrev = ST_HOME;
static uint32_t ntUntil = 0;

static String phoneName() {
  const char *sosMeLabel(); int sosMeIndex();
  return sosMeIndex() >= 0 ? String("SoS ") + sosMeLabel() : String("SoS Badge");
}

// setup(): start Bluetooth if it was on before.
static void phoneMarkStarting(bool on) {
  Preferences p;
  p.begin("phone", false);
  p.putBool("starting", on);
  p.end();
}

static void phoneSaveWanted(bool on);

// Start Bluetooth with the crash marker around it (see phoneSetup).
static void phoneStart() {
  phoneMarkStarting(true);
  phoneBegin(phoneName().c_str());
  phoneMarkStarting(false);
}

void phoneSetup() {
  Preferences p;
  p.begin("phone", true);
  phoneBtWanted = p.getBool("bt", false);
  bool crashed = p.getBool("starting", false);
  p.end();
  // v3.40 safety net: "starting" is set just before Bluetooth starts and cleared once it is up. Still set at
  // boot = the last start crashed inside Bluetooth, so leave it off (no boot loop); MUSIC switches it on again.
  if (phoneBtWanted && crashed) {
    Serial.println("phone  : ! Bluetooth crashed at the last start - left off (open MUSIC to try again)");
    phoneSaveWanted(false);
    phoneMarkStarting(false);
    return;
  }
  if (phoneBtWanted) phoneStart();
  else Serial.println("phone  : Bluetooth off (open MUSIC to switch it on)");
}

static void phoneSaveWanted(bool on) {
  phoneBtWanted = on;
  Preferences p;
  p.begin("phone", false);
  p.putBool("bt", on);
  p.end();
}

// ---------------------------------------------------------------- MUSIC screen

static int musState() {                            // 0 off, 1 waiting, 2 pairing, 3 connected, 4 + notifications
  if (!phoneStarted() || !phoneBtWanted) return 0;
  if (!phoneConnected()) return 1;
  if (!phoneSecure()) return 2;
  return phoneNotifyReady() ? 4 : 3;
}

static void musStatus() {
  gfx->fillRect(40, 78, 386, 40, RGB565(8, 10, 22));
  int st = musState();
  char a[64];
  uint16_t c;
  switch (st) {
    case 0:  snprintf(a, sizeof(a), "Bluetooth is off"); c = RGB565(255, 170, 90); break;
    case 1:  snprintf(a, sizeof(a), "pair \"%s\" on your phone", phoneName().c_str()); c = RGB565(150, 210, 255); break;
    case 2:  snprintf(a, sizeof(a), "pairing..."); c = RGB565(255, 220, 120); break;
    case 3:  snprintf(a, sizeof(a), "phone connected"); c = RGB565(120, 240, 150); break;
    default: snprintf(a, sizeof(a), "phone connected  -  notifications on"); c = RGB565(120, 240, 150); break;
  }
  uiCentred(84, a, &UiSmall, c);
  musLastState = st;
}

static void musTriangle(int cx, int cy, int s, int dir, uint16_t c) {   // dir +1 right, -1 left
  gfx->fillTriangle(cx - dir * s / 2, cy - s, cx - dir * s / 2, cy + s, cx + dir * s, cy, c);
}

static void musButton(int i, bool lit) {
  uint16_t ring = lit ? RGB565_WHITE : RGB565(80, 110, 160), fill = lit ? RGB565(40, 60, 110) : RGB565(18, 22, 44);
  uint16_t ic = RGB565(220, 232, 250);
  if (i == 0) {                                    // play / pause
    gfx->fillCircle(MUS_PLAY_X, MUS_ROW1_Y, MUS_PLAY_R, lit ? RGB565(60, 200, 110) : RGB565(30, 150, 80));
    gfx->drawCircle(MUS_PLAY_X, MUS_ROW1_Y, MUS_PLAY_R, ring);
    musTriangle(MUS_PLAY_X - 14, MUS_ROW1_Y, 20, 1, RGB565_WHITE);
    gfx->fillRect(MUS_PLAY_X + 12, MUS_ROW1_Y - 20, 8, 40, RGB565_WHITE);
    gfx->fillRect(MUS_PLAY_X + 25, MUS_ROW1_Y - 20, 8, 40, RGB565_WHITE);
  } else if (i <= 2) {                             // previous / next
    int x = MUS_SIDE_X[i - 1], d = i == 1 ? -1 : 1;
    gfx->fillCircle(x, MUS_ROW1_Y, MUS_SIDE_R, fill);
    gfx->drawCircle(x, MUS_ROW1_Y, MUS_SIDE_R, ring);
    musTriangle(x - 6 * d, MUS_ROW1_Y, 11, d, ic);
    musTriangle(x + 8 * d, MUS_ROW1_Y, 11, d, ic);
    gfx->fillRect(d > 0 ? x + 19 : x - 23, MUS_ROW1_Y - 12, 4, 24, ic);
  } else {                                         // volume down / mute / up
    int k = i - 3, x = MUS_VOL_X[k];
    gfx->fillCircle(x, MUS_ROW2_Y, MUS_VOL_R, fill);
    gfx->drawCircle(x, MUS_ROW2_Y, MUS_VOL_R, ring);
    gfx->fillRect(x - 13, MUS_ROW2_Y - 6, 7, 12, ic);                       // speaker
    gfx->fillTriangle(x - 7, MUS_ROW2_Y, x + 2, MUS_ROW2_Y - 12, x + 2, MUS_ROW2_Y + 12, ic);
    if (k == 0) gfx->fillRect(x + 6, MUS_ROW2_Y - 2, 10, 4, ic);             // -
    if (k == 2) { gfx->fillRect(x + 6, MUS_ROW2_Y - 2, 12, 4, ic); gfx->fillRect(x + 10, MUS_ROW2_Y - 6, 4, 12, ic); }   // +
    if (k == 1) {                                                           // x
      for (int t = -1; t <= 1; t++) {
        gfx->drawLine(x + 6 + t, MUS_ROW2_Y - 6, x + 16 + t, MUS_ROW2_Y + 6, RGB565(255, 110, 110));
        gfx->drawLine(x + 6 + t, MUS_ROW2_Y + 6, x + 16 + t, MUS_ROW2_Y - 6, RGB565(255, 110, 110));
      }
    }
  }
}

static void musBtPill() {
  gfx->fillRect(130, MUS_BT_Y - 2, 206, 28, RGB565(8, 10, 22));
  bool on = phoneBtWanted;
  gfx->fillRoundRect(143, MUS_BT_Y, 180, 24, 11, RGB565(14, 18, 34));
  gfx->drawRoundRect(143, MUS_BT_Y, 180, 24, 11, on ? RGB565(70, 180, 110) : RGB565(150, 90, 70));
  uiCentred(MUS_BT_Y + 7, on ? "Bluetooth ON - tap for off" : "Bluetooth OFF - tap for on", &UiSmall,
            on ? RGB565(150, 240, 170) : RGB565(255, 180, 150));
}

void showMusic() {
  if (!phoneBtWanted) { phoneSaveWanted(true); }
  if (!phoneStarted()) phoneStart();
  else phoneAdvertise(true);
  gfx->fillScreen(RGB565(8, 10, 22));
  uiCentred(46, "MUSIC", &UiBig, RGB565(120, 240, 150));
  musStatus();
  for (int i = 0; i < 6; i++) musButton(i, false);
  musBtPill();
  gfx->fillRoundRect(188, MUS_HOME_Y, 90, 24, 10, RGB565(6, 10, 26));
  gfx->drawRoundRect(188, MUS_HOME_Y, 90, 24, 10, RGB565(70, 110, 150));
  uiCentred(MUS_HOME_Y + 7, "HOME", &UiSmall, RGB565(220, 220, 230));
  gfx->flush();
  musStatusAt = millis();
}

void musicTick() {
  if (millis() - musStatusAt < 500) return;
  musStatusAt = millis();
  if (musState() != musLastState) { musStatus(); flushRect(40, 78, 386, 40); }
}

static void musFlash(int i, int key) {
  musButton(i, true);
  int x0 = 0, y0 = (i < 3 ? MUS_ROW1_Y - MUS_PLAY_R : MUS_ROW2_Y - MUS_VOL_R) - 2;
  int h = (i < 3 ? 2 * MUS_PLAY_R : 2 * MUS_VOL_R) + 4;
  flushRect(x0, y0, LCD_WIDTH, h);
  if (musState() >= 3) phoneMediaKey(key);
  else Serial.println("phone  : no phone connected - key not sent");
  delay(90);
  musButton(i, false);
  flushRect(x0, y0, LCD_WIDTH, h);
}

void musicTap(int x, int y) {
  if (y >= MUS_HOME_Y - 8 && x > 160 && x < 306) { showScreen(ST_HOME); return; }
  if (y >= MUS_BT_Y - 6 && y < MUS_BT_Y + 30 && x > 130 && x < 336) {
    if (phoneBtWanted) {
      phoneSaveWanted(false);
      phoneAdvertise(false);
      Serial.println("phone  : Bluetooth switched off (fully off after the next restart)");
    } else {
      phoneSaveWanted(true);
      if (!phoneStarted()) phoneStart(); else phoneAdvertise(true);
    }
    musBtPill(); musStatus();
    flushRect(0, 78, LCD_WIDTH, 40); flushRect(0, MUS_BT_Y - 2, LCD_WIDTH, 28);
    return;
  }
  auto hit = [&](int cx, int cy, int r) { int dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= (r + 10) * (r + 10); };
  if (hit(MUS_PLAY_X, MUS_ROW1_Y, MUS_PLAY_R))         musFlash(0, PHONE_KEY_PLAY);
  else if (hit(MUS_SIDE_X[0], MUS_ROW1_Y, MUS_SIDE_R))  musFlash(1, PHONE_KEY_PREV);
  else if (hit(MUS_SIDE_X[1], MUS_ROW1_Y, MUS_SIDE_R))  musFlash(2, PHONE_KEY_NEXT);
  else if (hit(MUS_VOL_X[0], MUS_ROW2_Y, MUS_VOL_R))    musFlash(3, PHONE_KEY_VOLDN);
  else if (hit(MUS_VOL_X[1], MUS_ROW2_Y, MUS_VOL_R))    musFlash(4, PHONE_KEY_MUTE);
  else if (hit(MUS_VOL_X[2], MUS_ROW2_Y, MUS_VOL_R))    musFlash(5, PHONE_KEY_VOLUP);
}

// Home grid icon: two beamed eighth notes.
void drawMusicIcon(int cx, int cy, int r, uint16_t col) {
  int s = r * 10 / 38;
  gfx->fillEllipse(cx - 9 * s / 10 - 5, cy + 10 * s / 10, 7 * s / 10 + 2, 5 * s / 10 + 2, col);
  gfx->fillEllipse(cx + 11 * s / 10 - 5, cy + 7 * s / 10, 7 * s / 10 + 2, 5 * s / 10 + 2, col);
  gfx->fillRect(cx - 9 * s / 10 + 1, cy - 12 * s / 10, 3, 22 * s / 10, col);
  gfx->fillRect(cx + 11 * s / 10 + 1, cy - 15 * s / 10, 3, 22 * s / 10, col);
  for (int t = 0; t < 5; t++)
    gfx->drawLine(cx - 9 * s / 10 + 1, cy - 12 * s / 10 + t, cx + 11 * s / 10 + 3, cy - 15 * s / 10 + t, col);
}

// ---------------------------------------------------------------- notification card

// Word-wrap s in font f, each line as wide as the round panel allows at its height; draw centred.
static int ntWrapDraw(const char *s, const GFXfont *f, int yTop, int lineH, int maxLines, uint16_t col) {
  char line[64];
  int lines = 0;
  const char *p = s;
  gfx->setFont(f);
  while (*p && lines < maxLines) {
    while (*p == ' ') p++;
    int y = yTop + lines * lineH, dy = abs(y + lineH / 2 - 233);
    int maxW = dy < 225 ? (int)(2 * sqrtf((float)(225 * 225 - dy * dy))) - 40 : 0;
    int n = 0, lastSpace = -1;
    const char *q = p;
    while (*q && n < 62) {
      line[n] = *q; line[n + 1] = 0;
      int16_t x1, y1; uint16_t w, h;
      gfx->getTextBounds(line, 0, 0, &x1, &y1, &w, &h);
      if ((int)w > maxW) break;
      if (*q == ' ') lastSpace = n;
      n++; q++;
    }
    if (*q && lastSpace > 0) { n = lastSpace; q = p + lastSpace; }
    line[n] = 0;
    bool more = *q && lines == maxLines - 1;
    if (more && n > 3) { line[n - 3] = '.'; line[n - 2] = '.'; line[n - 1] = '.'; }
    gfx->setFont();
    uiCentred(y, line, f, col);
    gfx->setFont(f);
    lines++;
    p = q;
  }
  gfx->setFont();
  return lines;
}

static void ntDraw() {
  bool call = ntCat == PHONE_CAT_CALL;
  uint16_t accent = call ? RGB565(80, 220, 120) : ntCat == PHONE_CAT_EMAIL ? RGB565(110, 170, 255)
                  : ntCat == PHONE_CAT_SOCIAL ? RGB565(255, 150, 200) : ntCat == PHONE_CAT_SCHED ? RGB565(255, 200, 90)
                  : RGB565(150, 200, 255);
  gfx->fillScreen(RGB565(6, 8, 18));
  gfx->drawCircle(233, 233, 228, accent);
  gfx->drawCircle(233, 233, 227, accent);
  // app pill
  char app[32];
  snprintf(app, sizeof(app), "%s", call ? "INCOMING CALL" : ntApp);
  for (char *c = app; *c; c++) *c = toupper((unsigned char)*c);
  gfx->setFont(&UiSmall);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(app, 0, 0, &x1, &y1, &w, &h);
  gfx->setFont();
  int pw = w + 28;
  gfx->fillRoundRect(233 - pw / 2, 62, pw, 24, 11, accent);
  uiCentred(69, app, &UiSmall, RGB565(6, 8, 18));
  int y = 110;
  int tl = ntWrapDraw(ntTitle[0] ? ntTitle : ntApp, &UiBig, y, 30, 2, RGB565_WHITE);
  y += tl * 30 + 12;
  if (ntMsg[0]) ntWrapDraw(ntMsg, &UiLabel, y, 22, 7, RGB565(200, 212, 232));
  uiCentred(418, "tap to close", &UiSmall, RGB565(110, 125, 150));
  gfx->flush();
}

void showNotify() { ntDraw(); }                    // a redraw via showScreen(ST_NOTIFY)

// loop(): show a notification that came in.
void phoneTick() {
  if (!phoneStarted()) return;
  if (screen == ST_MUSIC && !saverActive()) musicTick();
  if (screen == ST_NOTIFY && millis() > ntUntil) { int p = ntPrev; showScreen(p == ST_NOTIFY ? ST_HOME : p); return; }
  char app[24], title[48], msg[140];
  int cat;
  if (!phoneNextNotification(app, sizeof(app), title, sizeof(title), msg, sizeof(msg), &cat)) return;
  memcpy(ntApp, app, sizeof(ntApp)); memcpy(ntTitle, title, sizeof(ntTitle)); memcpy(ntMsg, msg, sizeof(ntMsg));
  ntCat = cat;
  if (saverActive()) saverWake(false, "phone");
  if (screen != ST_NOTIFY) ntPrev = screen;
  screen = ST_NOTIFY;
  ntUntil = millis() + (cat == PHONE_CAT_CALL ? 15000 : 8000);
  ntDraw();
  sosPlay(SND_ORA_REVEAL, 5600, 70);               // a soft ping (the start of the Oracle chime)
  Serial.printf("phone  : showing %s: %s\n", ntApp, ntTitle);
}

void notifyTap(int x, int y) {
  int p = ntPrev;
  showScreen(p == ST_NOTIFY ? ST_HOME : p);
}
