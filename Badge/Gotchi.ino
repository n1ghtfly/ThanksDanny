/*
 * Danny-gotchi (v3.33) - a pixel-art Danny who lives on the badge and needs looking after.
 *
 * Three needs, 0..100, running down in REAL time (internet time), also while the app is closed or the
 * badge is off - so on opening the app the time since the last visit is applied first:
 *   COFFEE  -8/h awake, -3/h asleep (empty in ~12 h)
 *   MOOD    -5/h awake, -2/h asleep
 *   ENERGY  -4/h awake, +25/h asleep (he wakes up by himself when full)
 * All three at zero = KERNEL PANIC: Danny crashes, his uptime is shown, the middle button reboots a
 * fresh one. Uptime decides his job title: INTERN, JUNIOR, MEDIOR, SENIOR, PRINCIPAL, ROOT.
 *
 * Buttons: COFFEE (+30; when already full he gets WIRED and grumpy), THANKS (+25 mood, hearts, a reply),
 * SLEEP / WAKE. Waking him with coffee or thanks makes him grumpy. Tap Danny himself: he waves.
 * Elsewhere on the badge: every slap your badge has received since the last visit costs mood (and wakes
 * him up); shaking the badge makes him dizzy.
 *
 * State lives in NVS ("gotchi"), saved on every action and every 5 minutes. The home grid icon gets a
 * red dot when he needs care. Pixel art: Badge/gotchi_assets.h from tools/make_gotchi_assets.py.
 */

#include "gotchi_assets.h"

#define GOT_SCALE    6                       // sprite pixel = 6 x 6 screen pixels (144 x 156)
#define GOT_SX       161                     // sprite top-left
#define GOT_SY       142
#define GOT_SCENE_X  70                      // the animated box (redrawn ~10 times a second)
#define GOT_SCENE_Y  120
#define GOT_SCENE_W  326
#define GOT_SCENE_H  234
#define GOT_MSG_Y    326                     // message line (top of capitals)
#define GOT_BTN_Y    380
static const int GOT_BTN_X[3] = { 151, 233, 315 };
#define GOT_BTN_R    28
#define GOT_HOME_Y   428
#define GOT_FRAME_MS 100

static Preferences gotPrefs;
static bool gotLoaded = false;
static float gotCoffee = 80, gotMood = 80, gotEnergy = 80;
static uint32_t gotBorn = 0, gotLast = 0, gotSlapsSeen = 0, gotDeadUp = 0;
static bool gotAsleep = false, gotDead = false;
static uint32_t gotLastFrame = 0, gotLastSave = 0, gotLastShake = 0;
static uint32_t gotDizzyUntil = 0, gotWiredUntil = 0, gotActUntil = 0, gotHeartsUntil = 0, gotMsgUntil = 0;
static int gotActFrame = GOT_HAPPY;
static char gotMsg[64] = "";
static int gotShown[3] = { -1, -1, -1 };     // bar lengths on screen (redraw the top when they change)
static int16_t *gotBeep[3] = { nullptr, nullptr, nullptr };   // low / high / sad, made once
#define GOT_BEEP_N 1600                      // 0.1 s at 16 kHz

static uint32_t gotNow() {                   // epoch seconds, or 0 until internet time is there
  time_t t = time(nullptr);
  return t > 1600000000 ? (uint32_t)t : 0;
}

static float gotClamp(float v) { return v < 0 ? 0 : v > 100 ? 100 : v; }

static void gotSave() {
  gotPrefs.begin("gotchi", false);
  gotPrefs.putFloat("c", gotCoffee); gotPrefs.putFloat("m", gotMood); gotPrefs.putFloat("e", gotEnergy);
  gotPrefs.putUInt("born", gotBorn); gotPrefs.putUInt("last", gotLast); gotPrefs.putUInt("sl", gotSlapsSeen);
  gotPrefs.putBool("slp", gotAsleep); gotPrefs.putBool("dead", gotDead); gotPrefs.putUInt("du", gotDeadUp);
  gotPrefs.end();
  gotLastSave = millis();
}

static void gotLoad() {
  if (gotLoaded) return;
  gotPrefs.begin("gotchi", true);
  gotCoffee = gotPrefs.getFloat("c", 80); gotMood = gotPrefs.getFloat("m", 80); gotEnergy = gotPrefs.getFloat("e", 80);
  gotBorn = gotPrefs.getUInt("born", 0); gotLast = gotPrefs.getUInt("last", 0);
  gotSlapsSeen = gotPrefs.getUInt("sl", sosSlapsGot());
  gotAsleep = gotPrefs.getBool("slp", false); gotDead = gotPrefs.getBool("dead", false); gotDeadUp = gotPrefs.getUInt("du", 0);
  gotPrefs.end();
  gotLoaded = true;
}

static void gotSay(const char *s, uint32_t ms) {
  snprintf(gotMsg, sizeof(gotMsg), "%s", s);
  gotMsgUntil = millis() + ms;
}

// Apply the time since the last update, and slaps received since the last look.
static void gotUpdate() {
  gotLoad();
  uint32_t now = gotNow();
  if (now) {
    if (!gotBorn) gotBorn = now;
    if (!gotLast) gotLast = now;
    uint32_t dt = now > gotLast ? now - gotLast : 0;
    if (dt > 72 * 3600) dt = 72 * 3600;
    float h = dt / 3600.0f;
    if (!gotDead && h > 0) {
      if (gotAsleep) {
        gotEnergy += 25 * h; gotCoffee -= 3 * h; gotMood -= 2 * h;
        if (gotEnergy >= 100) { gotAsleep = false; gotSay("Good morning! Coffee?", 4000); }
      } else {
        gotCoffee -= 8 * h; gotMood -= 5 * h; gotEnergy -= 4 * h;
      }
      gotCoffee = gotClamp(gotCoffee); gotMood = gotClamp(gotMood); gotEnergy = gotClamp(gotEnergy);
      if (gotCoffee <= 0 && gotMood <= 0 && gotEnergy <= 0) {
        gotDead = true; gotAsleep = false;
        gotDeadUp = now - gotBorn;
        Serial.printf("gotchi : KERNEL PANIC after %lu h uptime\n", (unsigned long)(gotDeadUp / 3600));
      }
    }
    gotLast = now;
  }
  uint32_t slaps = sosSlapsGot();
  if (slaps > gotSlapsSeen) {
    uint32_t d = slaps - gotSlapsSeen;
    gotSlapsSeen = slaps;
    if (!gotDead) {
      gotMood = gotClamp(gotMood - 6.0f * d);
      gotAsleep = false;
      char b[48];
      snprintf(b, sizeof(b), d == 1 ? "Ouch! Somebody slapped me." : "Ouch! Slapped %lu times!", (unsigned long)d);
      gotSay(b, 4000);
    }
  } else if (slaps < gotSlapsSeen) gotSlapsSeen = slaps;
}

bool gotNeedsCare() {                        // for the red dot on the home grid
  gotUpdate();
  return gotDead || gotCoffee < 25 || gotMood < 25 || gotEnergy < 20;
}

static const char *gotTitle(uint32_t up) {
  uint32_t d = up / 86400;
  return d < 1 ? "INTERN" : d < 3 ? "JUNIOR ENG" : d < 7 ? "MEDIOR ENG" : d < 14 ? "SENIOR ENG" : d < 30 ? "PRINCIPAL" : "ROOT";
}

// ---------------------------------------------------------------- sound

static void gotMakeBeeps() {
  if (gotBeep[0]) return;
  static const float F[3] = { 660, 1320, 330 };
  for (int k = 0; k < 3; k++) {
    gotBeep[k] = (int16_t *)heap_caps_malloc(GOT_BEEP_N * 2, MALLOC_CAP_SPIRAM);
    if (!gotBeep[k]) return;
    for (int i = 0; i < GOT_BEEP_N; i++) {
      float env = i < 160 ? i / 160.0f : (GOT_BEEP_N - i) / (float)(GOT_BEEP_N - 160);
      float sq = sinf(2 * PI * F[k] * i / 16000) > 0 ? 1 : -1;            // square wave: a retro beep
      gotBeep[k][i] = (int16_t)(sq * env * 9000);
    }
  }
}

static void gotBeepPlay(int k) {
  gotMakeBeeps();
  if (gotBeep[k]) sosPlay(gotBeep[k], GOT_BEEP_N, 200);    // Sos.ino; silent when the slap sound is muted
}

// ---------------------------------------------------------------- drawing

static void gotPixels(const uint8_t *px, int w, int h, int x, int y, int s, const uint16_t *pal) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) {
      uint8_t v = px[j * w + i];
      if (v) gfx->fillRect(x + i * s, y + j * s, s, s, pal[v]);
    }
}

static void gotTextC(int yTop, const char *s, const GFXfont *f, uint16_t col) {
  uiCentred(yTop, s, f, col);
}

static const uint16_t GOT_BG = RGB565(8, 10, 24);

static int gotFrame() {
  uint32_t m = millis();
  if (gotDead) return GOT_DEAD;
  if (gotAsleep) return GOT_SLEEP;
  if (m < gotDizzyUntil) return GOT_DIZZY;
  if (m < gotWiredUntil) return GOT_WIRED;
  if (m < gotActUntil) return gotActFrame;
  if (gotCoffee < 20) return GOT_HUNGRY;
  if (gotMood < 25 || gotEnergy < 15) return GOT_SAD;
  if (m % 4200 < 160) return GOT_BLINK;
  return GOT_NORMAL;
}

static const char *gotStatusMsg() {
  static const char *QUIPS[] = {
    "It's always DNS.", "Did you ping before you thought?", "Working as designed.", "No changes on Fridays.",
    "sudo make me a coffee", "Who touched the firewall?", "Uptime is a lifestyle.", "Have you tried rebooting?",
    "Layer 8 problem, I think.", "Ship it.",
  };
  if (millis() < gotMsgUntil) return gotMsg;
  if (gotDead) return "KERNEL PANIC";
  if (gotAsleep) return "zzz... maintenance window";
  if (gotCoffee < 20) return "Need... coffee...";
  if (gotMood < 25) return "Feeling unappreciated.";
  if (gotEnergy < 15) return "So tired. 3 a.m. change?";
  return QUIPS[(millis() / 7000) % (sizeof(QUIPS) / sizeof(QUIPS[0]))];
}

// The animated middle: rack, Danny, effects, message.
static void gotScene() {
  uint32_t m = millis();
  gfx->fillRect(GOT_SCENE_X, GOT_SCENE_Y, GOT_SCENE_W, GOT_SCENE_H, GOT_BG);
  const uint16_t *pal = gotDead ? GOT_PAL_DEAD : GOT_PAL;
  // floor + shadow
  gfx->drawFastHLine(GOT_SCENE_X + 10, GOT_SY + 156 + 4, GOT_SCENE_W - 20, RGB565(30, 34, 60));
  gfx->fillEllipse(GOT_SX + 72, GOT_SY + 156 + 2, 52, 6, RGB565(20, 22, 40));
  // server rack with blinking lights
  gotPixels(GOT_RACK, GOT_RACK_W, GOT_RACK_H, 84, GOT_SY + 156 + 4 - GOT_RACK_H * 6, 6, pal);
  for (int u = 0; u < 5; u++) {
    int ly = GOT_SY + 156 + 4 - GOT_RACK_H * 6 + (2 + u * 3) * 6 + 2;
    for (int l = 0; l < 3; l++) {
      uint32_t seed = (m / (180 + 70 * l + 40 * u)) * 2654435761u + u * 97 + l * 13;
      bool on = (seed >> 13) & 1;
      uint16_t c = gotDead ? ((m / 300) % 2 ? RGB565(255, 40, 40) : RGB565(60, 10, 10))
                 : on ? (l == 2 ? RGB565(255, 180, 40) : RGB565(60, 255, 110)) : RGB565(20, 40, 24);
      gfx->fillRect(84 + 18 + l * 10, ly, 6, 6, c);
    }
  }
  // Danny: bobbing, jittery when wired, still when asleep or dead
  int f = gotFrame();
  int dx = 0, dy = 0;
  if (!gotDead && !gotAsleep) dy = (m / 500) % 2 ? 0 : -3;
  if (f == GOT_WIRED) { dx = (int)(esp_random() % 7) - 3; dy = (int)(esp_random() % 5) - 2; }
  if (f == GOT_DIZZY) dx = (int)(6 * sinf(m / 90.0f));
  gotPixels(GOT_FRAMES[f], GOT_W, GOT_H, GOT_SX + dx, GOT_SY + dy, GOT_SCALE, pal);
  // effects
  if (m < gotHeartsUntil) {
    float t = 1.0f - (gotHeartsUntil - m) / 2500.0f;
    for (int k = 0; k < 3; k++) {
      int hx = GOT_SX + 20 + k * 46 + (int)(8 * sinf(t * 8 + k));
      int hy = GOT_SY + 40 - (int)(t * 90) - k * 12;
      if (hy > GOT_SCENE_Y + 2) gotPixels(GOT_HEART, GOT_HEART_W, GOT_HEART_H, hx, hy, 4, GOT_PAL);
    }
  }
  if (gotAsleep) {
    for (int k = 0; k < 3; k++) {
      float t = fmodf(m / 2200.0f + k / 3.0f, 1.0f);
      int zx = GOT_SX + 140 + (int)(t * 30), zy = GOT_SY + 30 - (int)(t * 70);
      if (zy > GOT_SCENE_Y + 4) uiText(zx, zy, "Z", k == 1 ? &UiBig : &UiSmall, RGB565(160, 170, 255));
    }
  }
  if (gotDead && (m / 600) % 2) uiText(GOT_SX + 120, GOT_SY + 10, "!", &UiTitle, RGB565(255, 60, 60));
  // message
  const char *msg = gotStatusMsg();
  const GFXfont *mf = &UiBig;
  gfx->setFont(mf);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
  gfx->setFont();
  if (w > 380) mf = &UiSmall;
  uint16_t mc = gotDead ? RGB565(255, 80, 80) : millis() < gotMsgUntil ? RGB565(255, 230, 140) : RGB565(200, 215, 235);
  gotTextC(GOT_MSG_Y, msg, mf, mc);
}

static void gotMeter(int cx, int y, const uint8_t *icon, float v, uint16_t col) {
  gotPixels(icon, 10, 10, cx - 44, y - 2, 2, GOT_PAL);
  gfx->drawRect(cx - 20, y, 62, 14, RGB565(90, 100, 130));
  int w = (int)(v * 58 / 100);
  bool low = v < 25;
  uint16_t c = low && (millis() / 400) % 2 ? RGB565(255, 60, 60) : col;
  gfx->fillRect(cx - 18, y + 2, 58, 10, RGB565(14, 16, 30));
  if (w > 0) gfx->fillRect(cx - 18, y + 2, w, 10, c);
}

static void gotTop() {
  gfx->fillRect(60, 30, 346, 88, GOT_BG);
  gotTextC(40, "DANNY-GOTCHI", &UiSmall, RGB565(90, 255, 130));
  gotMeter(128, 62, GOT_I_MUG, gotCoffee, RGB565(200, 130, 60));
  gotMeter(233, 62, GOT_I_HEART, gotMood, RGB565(240, 90, 130));
  gotMeter(338, 62, GOT_I_MOON, gotEnergy, RGB565(250, 220, 110));
  char b[48];
  uint32_t now = gotNow();
  uint32_t up = gotDead ? gotDeadUp : (now && gotBorn ? now - gotBorn : 0);
  snprintf(b, sizeof(b), "%s  -  %s %lud %luh", gotDead ? "CRASHED" : gotTitle(up), gotDead ? "WAS UP" : "UP",
           (unsigned long)(up / 86400), (unsigned long)(up % 86400 / 3600));
  gotTextC(94, now ? b : "waiting for internet time", &UiSmall, RGB565(170, 185, 210));
  gotShown[0] = (int)gotCoffee; gotShown[1] = (int)gotMood; gotShown[2] = (int)gotEnergy;
}

static void gotButtons() {
  gfx->fillRect(100, GOT_BTN_Y - GOT_BTN_R - 4, 266, 2 * GOT_BTN_R + 50, GOT_BG);
  static const char *L0[3] = { "COFFEE", "THANKS", "SLEEP" };
  for (int i = 0; i < 3; i++) {
    int x = GOT_BTN_X[i];
    bool reboot = gotDead && i == 1;
    gfx->fillCircle(x, GOT_BTN_Y, GOT_BTN_R, RGB565(18, 22, 44));
    gfx->drawCircle(x, GOT_BTN_Y, GOT_BTN_R, reboot ? RGB565(255, 80, 80) : RGB565(90, 120, 170));
    if (reboot) uiText(x - 10, GOT_BTN_Y - 11, "R", &UiBig, RGB565(255, 120, 120));
    else gotPixels(i == 0 ? GOT_I_MUG : i == 1 ? GOT_I_HEART : GOT_I_MOON, 10, 10, x - 15, GOT_BTN_Y - 15, 3, GOT_PAL);
  }
  for (int i = 0; i < 3; i++) {                     // the names, centred under each button
    bool reboot = gotDead && i == 1;
    const char *lb = reboot ? "REBOOT" : (i == 2 && gotAsleep) ? "WAKE" : L0[i];
    gfx->setFont(&UiSmall);
    int16_t x1, y1; uint16_t w, h;
    gfx->getTextBounds(lb, 0, 0, &x1, &y1, &w, &h);
    gfx->setFont();
    uiText(GOT_BTN_X[i] - (int)w / 2 - x1, GOT_BTN_Y + GOT_BTN_R + 4, lb, &UiSmall, RGB565(200, 215, 235));
  }
  gfx->fillRoundRect(188, GOT_HOME_Y, 90, 24, 10, RGB565(6, 10, 26));
  gfx->drawRoundRect(188, GOT_HOME_Y, 90, 24, 10, RGB565(70, 110, 150));
  uiCentred(GOT_HOME_Y + 7, "HOME", &UiSmall, RGB565(220, 220, 230));
}

void showGotchi() {
  gotUpdate();
  gfx->fillScreen(GOT_BG);
  gotTop();
  gotScene();
  gotButtons();
  gfx->flush();
  gotLastFrame = millis();
  Serial.printf("gotchi : open - coffee %.0f mood %.0f energy %.0f%s%s\n", gotCoffee, gotMood, gotEnergy,
                gotAsleep ? " (asleep)" : "", gotDead ? " (DEAD)" : "");
}

void gotchiTick() {
  uint32_t m = millis();
  if (m - gotLastFrame < GOT_FRAME_MS) return;
  gotLastFrame = m;
  // shake -> dizzy
  if (!gotDead && m - gotLastShake > 1500) {
    float g = sosAccelG();
    if (g > 1.9f || (g > 0 && g < 0.25f)) {
      gotLastShake = m;
      gotDizzyUntil = m + 3000;
      gotMood = gotClamp(gotMood - 3);
      if (gotAsleep) { gotAsleep = false; gotButtons(); flushRect(100, GOT_BTN_Y - GOT_BTN_R - 4, 266, GOT_HOME_Y + 26 - (GOT_BTN_Y - GOT_BTN_R - 4)); }
      gotSay("Whoa... packet loss...", 3000);
      gotBeepPlay(2);
    }
  }
  static uint32_t lastUpd = 0;
  if (m - lastUpd > 30000) {                 // the needs run down: apply now and then
    lastUpd = m;
    bool wasDead = gotDead, wasAsleep = gotAsleep;
    gotUpdate();
    if (gotDead != wasDead || gotAsleep != wasAsleep) { showGotchi(); return; }
  }
  if (m - gotLastSave > 300000) gotSave();
  // the bars blink when low, and change slowly: redraw the top when needed
  bool lowBlink = (gotCoffee < 25 || gotMood < 25 || gotEnergy < 25);
  if ((int)gotCoffee != gotShown[0] || (int)gotMood != gotShown[1] || (int)gotEnergy != gotShown[2] || lowBlink) {
    gotTop();
    flushRect(60, 30, 346, 88);
  }
  gotScene();
  flushRect(GOT_SCENE_X, GOT_SCENE_Y, GOT_SCENE_W, GOT_SCENE_H);
}

static void gotWakeGrumpy() {
  gotAsleep = false;
  gotMood = gotClamp(gotMood - 10);
  gotSay("Hey! I was sleeping...", 3000);
  gotBeepPlay(2);
}

// The three buttons (0 COFFEE, 1 THANKS / REBOOT, 2 SLEEP / WAKE) - from the screen or the remote page.
void gotchiAction(int b) {
  gotUpdate();
  uint32_t m = millis();
  if (gotDead) {
    if (b == 1) {                              // REBOOT
      gotCoffee = gotMood = gotEnergy = 80;
      gotDead = false; gotAsleep = false;
      gotBorn = gotNow(); gotLast = gotBorn;
      gotSay("Booting... Danny v2 online.", 4000);
      gotBeepPlay(1);
      gotSave();
      if (screen == ST_GOTCHI) showGotchi();
    }
    return;
  }
  if (b == 0) {                                // COFFEE
    if (gotAsleep) gotWakeGrumpy();
    else if (gotCoffee > 85) {
      gotWiredUntil = m + 4000;
      gotMood = gotClamp(gotMood - 8); gotEnergy = gotClamp(gotEnergy + 10);
      gotSay("TOO MUCH COFFEE!!!", 3500);
      gotBeepPlay(2);
    } else {
      gotCoffee = gotClamp(gotCoffee + 30); gotMood = gotClamp(gotMood + 4); gotEnergy = gotClamp(gotEnergy + 4);
      gotActFrame = GOT_COFFEE; gotActUntil = m + 2200;
      gotSay("Ahh, java.", 2500);
      gotBeepPlay(0);
    }
  } else if (b == 1) {                         // THANKS
    if (gotAsleep) gotWakeGrumpy();
    else {
      static const char *R[] = { "You're welcome!", "It was DNS, by the way.", "Anytime. Root helps.",
                                 "No problem - layer 1 issue.", "Thank YOU. Now commit it." };
      gotMood = gotClamp(gotMood + 25);
      gotActFrame = GOT_HAPPY; gotActUntil = m + 2500; gotHeartsUntil = m + 2500;
      gotSay(R[esp_random() % 5], 3000);
      gotBeepPlay(1);
    }
  } else {                                     // SLEEP / WAKE
    if (gotAsleep) { gotAsleep = false; gotSay("Morning! Coffee?", 3000); }
    else { gotAsleep = true; gotSay("Good night. Pager off.", 2500); }
    gotBeepPlay(0);
  }
  gotSave();
  if (screen == ST_GOTCHI) {
    gotTop();
    gotButtons();
    gotScene();
    gfx->flush();
  }
}

// For the remote page: the needs as whole numbers, and the state.
void gotchiState(int *c, int *mo, int *e, bool *asleep, bool *dead) {
  gotUpdate();
  *c = (int)gotCoffee; *mo = (int)gotMood; *e = (int)gotEnergy; *asleep = gotAsleep; *dead = gotDead;
}

void gotchiTap(int x, int y) {
  if (y >= GOT_HOME_Y - 8 && x > 160 && x < 306) { gotSave(); showScreen(ST_HOME); return; }
  uint32_t m = millis();
  int b = -1;
  for (int i = 0; i < 3; i++) {
    int dx = x - GOT_BTN_X[i], dy = y - GOT_BTN_Y;
    if (dx * dx + dy * dy <= 40 * 40) b = i;
  }
  if (b < 0) {
    if (y > GOT_SCENE_Y && y < GOT_SY + 160 && x > GOT_SX - 10 && x < GOT_SX + 154 && !gotDead && !gotAsleep) {
      gotActFrame = GOT_HAPPY; gotActUntil = m + 1200;
      gotSay("Hi! Need something?", 2000);
      gotBeepPlay(1);
    }
    return;
  }
  gotchiAction(b);
}

// Home grid icon: Danny's head, with a red dot when he needs you.
void drawGotchiIcon(int cx, int cy, int r) {
  int s = r >= 30 ? 2 : 1;
  gotPixels(GOT_F_NORMAL, GOT_W, 15, cx - GOT_W * s / 2, cy - 15 * s / 2 - 1, s, GOT_PAL);
  if (gotNeedsCare()) {
    gfx->fillCircle(cx + r - 6, cy - r + 6, 7, RGB565(255, 50, 60));
    gfx->drawCircle(cx + r - 6, cy - r + 6, 7, RGB565_WHITE);
  }
}
