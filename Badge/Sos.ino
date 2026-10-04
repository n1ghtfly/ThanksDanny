/*
 * Sos — the Sons of Sudo badge network: who is online, and remote slaps.
 *
 * Members: Claudio, Danny, Walter. Every badge runs the same firmware; Settings says which
 * person this badge belongs to. The broker (your own Mosquitto, see server/mosquitto/) and its
 * login are entered on the phone setup page and stored only on the badge (NVS "sos"), never in
 * the code - the repository is public.
 *
 * Protocol (keep in step with tools/sos_cli.py):
 *   sos/<to>/inbox/<from>   "slap|<strength 1-10>"   QoS 1. The broker ACL only lets <from> be
 *                                                      the sender's own login: no fake senders.
 *   sos/presence/<name>     "1" / "0"                 retained; "0" is also the last will, so a
 *                                                      badge that loses power shows offline.
 *   Persistent sessions: a slap sent while a badge is off waits on the broker and lands when it
 *   comes back (tested on Mosquitto 2.0.22, together with the ACL - see server/mosquitto).
 *
 * Sending: open SLAP, tap a victim, then SWING the badge like a slap. The QMI8658 gyro measures
 * the swing; its peak turn rate sets the strength (1-10). A SLAP button is there for anyone who
 * would rather not swing their badge across a meeting room.
 * Receiving: whatever screen is up, a hand flies in, the screen shakes in proportion to the
 * strength, a slap sound plays (unless muted in Settings), the counter goes up, and SLAP BACK
 * arms a return slap straight away.
 *
 * The MQTT link itself is src/sosnet.cpp (esp-mqtt, part of the ESP32 core). Sounds are
 * synthesized by tools/make_sos_sounds.py into sos_sounds.h - no third-party samples.
 */

#include "src/sosnet.h"
#include "sos_sounds.h"
#include <ImuDrv.hpp>            // SensorLib's current QMI8658 driver (SensorQMI8658.hpp is the deprecated one)

// The default broker comes from sos_local.h - a file kept on the PC only (in .gitignore), so the
// broker's address is not published in the public repository. Without it the default is empty
// and the host is simply typed on the phone setup page.
#if __has_include("sos_local.h")
#include "sos_local.h"
#endif
#ifndef SOS_DEFAULT_HOST
#define SOS_DEFAULT_HOST ""
#endif

#define SOS_N 3
static const char *SOS_ID[SOS_N]   = { "claudio", "danny", "walter" };
static const char *SOS_NAME[SOS_N] = { "CLAUDIO", "DANNY", "WALTER" };

#define SOS_SWING_DPS   480        // a turn faster than this starts a slap
#define SOS_FULL_DPS    2000       // ...this fast is a 10
#define SOS_ARM_MS      6000       // how long the badge waits for the swing
#define SOS_COOLDOWN_MS 2500       // between two slaps from this badge

// Settings (NVS namespace "sos")
static int    sosMe = -1;          // index into SOS_ID, -1 = not chosen yet
static String sosHost, sosUser, sosPass;
static int    sosPort = 8883;
static bool   sosTls = true;
static bool   sosMuted = false;
static uint32_t sosGot = 0, sosDealt = 0;
static bool   sosOnline[SOS_N] = { false, false, false };
static bool   sosStarted = false;
static uint32_t sosApDownAt = 0;   // tear the setup AP down after an MQTT-only save

// Incoming slaps waiting to be shown (filled by sosDrain, shown by sosPoll).
static int  sosInFrom = -1, sosInCount = 0, sosInStrength = 0;

// Slap app state
#define SLAP_PICK  0
#define SLAP_ARMED 1
#define SLAP_SENT  2
int slapState = SLAP_PICK;          // not static: Badge.ino resets it when the app opens
static int  slapTarget = -1, slapStrength = 0;
static uint32_t slapUntil = 0, slapLastSent = 0;
static float slapPeak = 0;
static uint32_t slapPeakAt = 0;

SensorQMI8658 imu;
static bool imuOK = false;

// ---------------------------------------------------------------- settings

int sosIndex(const char *id) {
  for (int i = 0; i < SOS_N; i++) if (!strcasecmp(id, SOS_ID[i])) return i;
  return -1;
}

void sosLoadConfig() {
  prefs.begin("sos", true);
  String me = prefs.getString("me", "");
  sosHost  = prefs.getString("host", SOS_DEFAULT_HOST);
  sosPort  = prefs.getInt("port", 8883);
  sosTls   = prefs.getBool("tls", true);
  sosUser  = prefs.getString("user", "");
  sosPass  = prefs.getString("pass", "");
  sosMuted = prefs.getBool("mute", false);
  sosGot   = prefs.getUInt("got", 0);
  sosDealt = prefs.getUInt("dealt", 0);
  prefs.end();
  sosMe = sosIndex(me.c_str());
  // The password is never logged - only whether one is set.
  Serial.printf("sos    : badge %s, broker %s:%d %s, user '%s', password %s, sound %s\n",
                sosMe >= 0 ? SOS_NAME[sosMe] : "(not chosen)", sosHost.length() ? sosHost.c_str() : "(none)",
                sosPort, sosTls ? "TLS" : "PLAIN", sosUser.c_str(), sosPass.length() ? "set" : "not set",
                sosMuted ? "muted" : "on");
}

void sosSaveConfig() {
  prefs.begin("sos", false);
  prefs.putString("me", sosMe >= 0 ? SOS_ID[sosMe] : "");
  prefs.putString("host", sosHost);
  prefs.putInt("port", sosPort);
  prefs.putBool("tls", sosTls);
  prefs.putString("user", sosUser);
  prefs.putString("pass", sosPass);
  prefs.putBool("mute", sosMuted);
  prefs.end();
}

void sosSaveCounters() {
  prefs.begin("sos", false);
  prefs.putUInt("got", sosGot);
  prefs.putUInt("dealt", sosDealt);
  prefs.end();
}

bool sosConfigured() { return sosMe >= 0 && sosHost.length() && sosUser.length(); }

// (Re)connect with the current settings. Called once Wi-Fi is up, and after a settings change.
void sosStart() {
  if (!sosConfigured() || WiFi.status() != WL_CONNECTED) {
    sosnet_stop();
    sosStarted = false;
    Serial.printf("sos    : not starting (%s)\n", !sosConfigured() ? "badge name / broker not set" : "no Wi-Fi");
    return;
  }
  char inbox[48], online[48], cid[32];
  snprintf(inbox, sizeof(inbox), "sos/%s/inbox/+", SOS_ID[sosMe]);
  snprintf(online, sizeof(online), "sos/presence/%s", SOS_ID[sosMe]);
  snprintf(cid, sizeof(cid), "sos-%s", SOS_ID[sosMe]);
  const char *subs[3] = { inbox, "sos/presence/+", "sos/all/#" };
  for (int i = 0; i < SOS_N; i++) sosOnline[i] = false;
  sosStarted = sosnet_start(sosHost.c_str(), sosPort, sosTls, cid, sosUser.c_str(), sosPass.c_str(), online, subs, 3);
  Serial.printf("sos    : connecting to %s:%d as %s (%s)\n", sosHost.c_str(), sosPort, SOS_ID[sosMe],
                sosTls ? "TLS" : "plain");
}

// For the home pill: e.g. "sos 2/3" (this badge counts as online when it is connected).
int sosOnlineCount() {
  int n = 0;
  for (int i = 0; i < SOS_N; i++) if (sosOnline[i] || (i == sosMe && sosnet_connected())) n++;
  return n;
}

// For the screensaver: is member i online (this badge counts while it is connected)?
bool sosIsOnline(int i) { return i >= 0 && i < SOS_N && (sosOnline[i] || (i == sosMe && sosnet_connected())); }
// SLAP is just showing the victims (not armed, not showing a result).
bool slapIdle() { return slapState == SLAP_PICK; }
// For the market screensaver: slaps received / dealt by this badge, all time.
uint32_t sosSlapsGot() { return sosGot; }
uint32_t sosSlapsDealt() { return sosDealt; }

// ---------------------------------------------------------------- motion

void sosImuBegin() {
  imuOK = imu.begin(Wire, 0x6B) || imu.begin(Wire, 0x6A);
  if (!imuOK) { Serial.println("sos    : ! QMI8658 not found - slaps use the SLAP button only"); return; }
  imu.configAccel(AccelFullScaleRange::FS_8G, 448.0f);
  imu.configGyro(GyroFullScaleRange::FS_2000_DPS, 448.0f);
  imu.enableAccel();
  imu.enableGyro();
  Serial.println("sos    : QMI8658 up, gyro +-2000 dps @ 448 Hz");
}

// Current turn rate in degrees per second (0 if no new sample).
float sosTurnRate() {
  if (!imuOK) return 0;
  GyroscopeData g;
  if (!imu.readGyro(g)) return 0;
  return sqrtf(g.dps.x * g.dps.x + g.dps.y * g.dps.y + g.dps.z * g.dps.z);
}

// ---------------------------------------------------------------- sound

static volatile bool sosSndBusy = false;
static const int16_t *sosSndPtr = nullptr;
static int sosSndLen = 0, sosSndGain = 256;     // gain: 256 = full

void sosSoundTask(void *arg) {
  static int16_t blk[256 * 2];
  for (int pos = 0; pos < sosSndLen; ) {
    int n = sosSndLen - pos; if (n > 256) n = 256;
    for (int i = 0; i < n; i++) blk[2 * i] = blk[2 * i + 1] = (int16_t)(((int32_t)sosSndPtr[pos + i] * sosSndGain) >> 8);
    decI2s.write((uint8_t *)blk, n * 4);
    pos += n;
  }
  memset(blk, 0, sizeof(blk));
  for (int i = 0; i < 6; i++) decI2s.write((uint8_t *)blk, sizeof(blk));
  digitalWrite(AUD_PA, LOW);
  sosSndBusy = false;
  vTaskDelete(nullptr);
}

void sosPlay(const int16_t *pcm, int len, int gain) {
  if (sosMuted || sosSndBusy || !decAudioInit()) return;
  decSoundStop();
  rumoursStop();
  sosSndPtr = pcm; sosSndLen = len; sosSndGain = gain;
  sosSndBusy = true;
  digitalWrite(AUD_PA, HIGH);
  if (xTaskCreatePinnedToCore(sosSoundTask, "sossnd", 4096, nullptr, 5, nullptr, 0) != pdPASS) {
    sosSndBusy = false; digitalWrite(AUD_PA, LOW);
  }
}

// ---------------------------------------------------------------- drawing

// An open hand, palm towards the viewer, scaled by s (100 = about 150 px tall).
void drawHand(int cx, int cy, int s, uint16_t skin, uint16_t line) {
  int pw = 76 * s / 100, ph = 70 * s / 100;
  int px = cx - pw / 2, py = cy - ph / 4;
  gfx->fillRoundRect(px, py, pw, ph, 18 * s / 100, skin);                       // palm
  int fw = 15 * s / 100, gap = (pw - 4 * fw) / 5;
  const int len[4] = { 58, 70, 66, 52 };
  for (int i = 0; i < 4; i++) {                                                   // fingers
    int fx = px + gap + i * (fw + gap);
    int fh = len[i] * s / 100;
    gfx->fillRoundRect(fx, py - fh + 10 * s / 100, fw, fh + 10 * s / 100, fw / 2, skin);
    gfx->drawFastVLine(fx + fw + gap / 2, py + 4, ph / 3, line);
  }
  // thumb, sticking out to the left
  gfx->fillTriangle(px + 6, py + ph / 2, px - 34 * s / 100, py + 4 * s / 100, px - 22 * s / 100, py - 8 * s / 100, skin);
  gfx->fillTriangle(px + 6, py + ph / 2, px + 4, py + 6, px - 22 * s / 100, py - 8 * s / 100, skin);
  gfx->fillCircle(px - 26 * s / 100, py - 1 * s / 100, 9 * s / 100, skin);
}

// The carousel icon: a small hand with motion lines.
void drawSlapIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  drawHand(cx + r / 8, cy + r / 6, r * 100 / 105, fg, bg);
  for (int i = 0; i < 3; i++)
    gfx->drawFastHLine(cx - r + 2, cy - r / 3 + i * r / 3, r / 3, fg);
}

void sosPill(const char *left, const char *right) {
  gfx->fillRoundRect(DEC_PILL_X, DEC_PILL_Y, DEC_PILL_W, DEC_PILL_H, 14, RGB565(10, 14, 30));
  gfx->drawRoundRect(DEC_PILL_X, DEC_PILL_Y, DEC_PILL_W, DEC_PILL_H, 14, RGB565(120, 200, 255));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  if (right) {
    gfx->drawFastVLine(DEC_PILL_X + DEC_PILL_W / 2, DEC_PILL_Y + 6, DEC_PILL_H - 12, RGB565(70, 90, 120));
    gfx->setCursor(DEC_PILL_X + (DEC_PILL_W / 2 - (int)strlen(left) * 12) / 2, DEC_PILL_Y + 11);
    gfx->print(left);
    gfx->setCursor(DEC_PILL_X + DEC_PILL_W / 2 + (DEC_PILL_W / 2 - (int)strlen(right) * 12) / 2, DEC_PILL_Y + 11);
    gfx->print(right);
  } else {
    gfx->setCursor(DEC_PILL_X + (DEC_PILL_W - (int)strlen(left) * 12) / 2, DEC_PILL_Y + 11);
    gfx->print(left);
  }
}

bool sosOnPill(int x, int y) {
  return x >= DEC_PILL_X - 10 && x <= DEC_PILL_X + DEC_PILL_W + 10 && y >= DEC_PILL_Y - 12 && y <= DEC_PILL_Y + DEC_PILL_H + 12;
}

// The two victims on the pick screen: index into SOS_ID, and where their buttons are.
int sosVictim(int k) {               // k = 0 or 1
  int n = 0;
  for (int i = 0; i < SOS_N; i++) if (i != sosMe) { if (n == k) return i; n++; }
  return -1;
}
#define VIC_Y 214
#define VIC_R 62
static const int VIC_X[2] = { 148, 318 };

void drawSlapPick() {
  gfx->fillScreen(RGB565(14, 8, 10));
  centred(44, "SLAP", 4, RGB565(255, 120, 110));
  if (!sosConfigured()) {
    centred(170, "who is this badge?", 2, RGB565(255, 220, 120));
    centred(204, "Settings: choose the badge name,", 1, RGB565(190, 190, 205));
    centred(220, "and enter the broker on the phone", 1, RGB565(190, 190, 205));
    centred(236, "setup page (Wi-Fi setup on phone).", 1, RGB565(190, 190, 205));
    sosPill("HOME", nullptr);
    return;
  }
  centred(96, "who deserves it?", 2, RGB565(220, 200, 205));
  for (int k = 0; k < 2; k++) {
    int v = sosVictim(k);
    bool on = sosOnline[v];
    gfx->fillCircle(VIC_X[k], VIC_Y, VIC_R, RGB565(40, 20, 26));
    gfx->drawCircle(VIC_X[k], VIC_Y, VIC_R, on ? RGB565(255, 120, 110) : RGB565(90, 70, 75));
    gfx->drawCircle(VIC_X[k], VIC_Y, VIC_R - 1, on ? RGB565(255, 120, 110) : RGB565(90, 70, 75));
    char ini[2] = { SOS_NAME[v][0], 0 };
    gfx->setTextSize(7);
    gfx->setTextColor(on ? RGB565_WHITE : RGB565(150, 140, 145));
    gfx->setCursor(VIC_X[k] - 17, VIC_Y - 24);
    gfx->print(ini);
    int w = strlen(SOS_NAME[v]) * 12;
    gfx->setTextSize(2);
    gfx->setCursor(VIC_X[k] - w / 2, VIC_Y + VIC_R + 10);
    gfx->print(SOS_NAME[v]);
    gfx->fillCircle(VIC_X[k] + 44, VIC_Y - 44, 9, on ? RGB565(80, 230, 110) : RGB565(80, 80, 90));
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "slapped %lu x  -  dealt %lu", (unsigned long)sosGot, (unsigned long)sosDealt);
  centred(326, buf, 1, RGB565(200, 180, 185));
  centred(344, sosnet_connected() ? "grey = offline, it waits for them" : sosnet_status(), 1,
          sosnet_connected() ? RGB565(140, 130, 135) : RGB565(255, 200, 120));
  if (sosMuted) centred(360, "(sound muted in Settings)", 1, RGB565(140, 130, 135));
  sosPill("HOME", nullptr);
}

void drawSlapArmed() {
  gfx->fillScreen(RGB565(40, 6, 8));
  centred(70, "SWING!", 5, RGB565_WHITE);
  char buf[32];
  snprintf(buf, sizeof(buf), "slap %s", SOS_NAME[slapTarget]);
  centred(128, buf, 3, RGB565(255, 170, 160));
  drawHand(233, 238, 85, RGB565(255, 200, 170), RGB565(150, 90, 70));
  centred(330, imuOK ? "swing the badge like a slap" : "no motion sensor - use the button", 1, RGB565(220, 190, 190));
  sosPill("SLAP", "CANCEL");
}

void drawSlapSent() {
  gfx->fillScreen(RGB565(14, 8, 10));
  centred(120, "SMACK!", 5, RGB565(255, 220, 120));
  char buf[40];
  snprintf(buf, sizeof(buf), "%s got a %d/10", SOS_NAME[slapTarget], slapStrength);
  centred(196, buf, 2, RGB565_WHITE);
  for (int i = 0; i < 10; i++)
    gfx->fillRoundRect(118 + i * 23, 236, 18, 26, 4, i < slapStrength ? RGB565(255, 90 + i * 14, 60) : RGB565(60, 40, 44));
  centred(290, sosnet_connected() && sosOnline[slapTarget] ? "delivered" :
                 sosnet_connected() ? "they're offline - it lands when they're back" : "queued - badge is offline",
          1, RGB565(200, 180, 185));
}

// ---------------------------------------------------------------- the Slap app

void showSlap() {
  if (slapState == SLAP_ARMED) drawSlapArmed();
  else if (slapState == SLAP_SENT) drawSlapSent();
  else drawSlapPick();
  gfx->flush();
}

void slapArm(int target) {
  slapTarget = target;
  slapState = SLAP_ARMED;
  slapUntil = millis() + SOS_ARM_MS;
  slapPeak = 0;
  slapPeakAt = 0;
  Serial.printf("sos    : armed - swing to slap %s\n", SOS_NAME[target]);
}

void slapSend(int strength) {
  if (millis() - slapLastSent < SOS_COOLDOWN_MS) { Serial.println("sos    : cooldown - slap not sent"); return; }
  slapLastSent = millis();
  slapStrength = strength < 1 ? 1 : (strength > 10 ? 10 : strength);
  char topic[64], payload[24];
  snprintf(topic, sizeof(topic), "sos/%s/inbox/%s", SOS_ID[slapTarget], SOS_ID[sosMe]);
  snprintf(payload, sizeof(payload), "slap|%d", slapStrength);
  bool ok = sosnet_publish(topic, payload, false);
  sosDealt++;
  sosSaveCounters();
  Serial.printf("sos    : slapped %s, strength %d (%s)\n", SOS_NAME[slapTarget], slapStrength, ok ? "sent" : "NOT queued");
  sosPlay(SND_SWISH, SND_SWISH_LEN, 200);
  slapState = SLAP_SENT;
  slapUntil = millis() + 1600;
  showScreen(ST_SLAP);
}

// From loop() while the Slap screen is up: watch the swing, time the screens.
void sosTick() {
  if (slapState == SLAP_ARMED) {
    float r = sosTurnRate();
    if (r > slapPeak) slapPeak = r;
    if (!slapPeakAt && r >= SOS_SWING_DPS) slapPeakAt = millis();
    // Keep measuring for 120 ms after the swing starts so the peak is the real peak.
    if (slapPeakAt && millis() - slapPeakAt > 120) {
      int s = 1 + (int)((slapPeak - SOS_SWING_DPS) * 9.0f / (SOS_FULL_DPS - SOS_SWING_DPS) + 0.5f);
      Serial.printf("sos    : swing peak %.0f dps -> strength %d\n", slapPeak, s);
      slapSend(s);
      return;
    }
    if (millis() > slapUntil) { slapState = SLAP_PICK; showScreen(ST_SLAP); }
  } else if (slapState == SLAP_SENT && millis() > slapUntil) {
    slapState = SLAP_PICK;
    showScreen(ST_SLAP);
  }
}

void slapTap(int x, int y) {
  if (slapState == SLAP_ARMED) {
    if (sosOnPill(x, y)) {
      if (x < DEC_PILL_X + DEC_PILL_W / 2) slapSend(5);           // SLAP button: a polite 5
      else { slapState = SLAP_PICK; showScreen(ST_SLAP); }        // CANCEL
    }
    return;
  }
  if (sosOnPill(x, y)) { slapState = SLAP_PICK; showScreen(ST_HOME); return; }
  if (slapState != SLAP_PICK || !sosConfigured()) return;
  for (int k = 0; k < 2; k++) {
    int dx = x - VIC_X[k], dy = y - VIC_Y;
    if (dx * dx + dy * dy <= (VIC_R + 14) * (VIC_R + 14)) { slapArm(sosVictim(k)); showScreen(ST_SLAP); return; }
  }
}

// ---------------------------------------------------------------- receiving

// Move everything the MQTT task received into our state. Presence updates are applied at once;
// slaps are collected (several from the same person while away become one "x3"). Returns true
// if a slap is waiting to be shown. Safe to call from the gallery's own loop.
bool sosDrain() {
  SosMsg m;
  bool presenceChanged = false;
  while (sosnet_next(&m)) {
    if (!strncmp(m.topic, "sos/presence/", 13)) {
      int who = sosIndex(m.topic + 13);
      if (who >= 0) {
        bool on = (m.payload[0] == '1');
        if (sosOnline[who] != on) { sosOnline[who] = on; presenceChanged = true;
          Serial.printf("sos    : %s is %s\n", SOS_NAME[who], on ? "online" : "offline"); }
      }
      continue;
    }
    const char *slash = strrchr(m.topic, '/');
    int from = slash ? sosIndex(slash + 1) : -1;
    if (from < 0 || strstr(m.topic, "/inbox/") == nullptr) continue;
    if (!strncmp(m.payload, "slap|", 5)) {
      int s = atoi(m.payload + 5); if (s < 1) s = 1; if (s > 10) s = 10;
      if (sosInCount && sosInFrom != from) {
        // a different slapper while one is still waiting: keep the strongest, credit both
        if (s > sosInStrength) { sosInFrom = from; sosInStrength = s; }
      } else {
        sosInFrom = from;
        if (s > sosInStrength) sosInStrength = s;
      }
      sosInCount++;
      Serial.printf("sos    : SLAP from %s, strength %d\n", SOS_NAME[from], s);
    }
  }
  if (presenceChanged && screen == ST_SLAP && slapState == SLAP_PICK) showScreen(ST_SLAP);
  if (presenceChanged && screen == ST_HOME) showScreen(ST_HOME);
  return sosInCount > 0;
}

// The takeover: hand flies in, the screen shakes with the strength, sound, counter.
void showSlapped() {
  int from = sosInFrom, s = sosInStrength, count = sosInCount;
  sosInCount = 0; sosInStrength = 0;
  if (from < 0) { showScreen(ST_HOME); return; }
  sosGot += count;
  sosSaveCounters();
  sosPlay(SND_SLAP, SND_SLAP_LEN, 110 + s * 14);                 // louder for a harder slap
  char who[40], power[32], tally[40];
  snprintf(who, sizeof(who), "%s SLAPPED YOU!", SOS_NAME[from]);
  if (count > 1) snprintf(power, sizeof(power), "x%d, hardest %d/10", count, s);
  else           snprintf(power, sizeof(power), "power %d/10", s);
  snprintf(tally, sizeof(tally), "slapped %lu times so far", (unsigned long)sosGot);
  int amp = 2 + s * 2;
  for (int f = 0; f <= 12; f++) {
    int dx = (f < 10) ? (int)((esp_random() % (2 * amp + 1)) - amp) : 0;
    int dy = (f < 10) ? (int)((esp_random() % (2 * amp + 1)) - amp) : 0;
    uint16_t bg = (f < 3) ? RGB565(200, 30, 30) : RGB565(60 - f * 3, 8, 10);
    gfx->fillScreen(bg);
    int hx = (f < 4) ? -120 + f * 95 : 233;                        // the hand swings in
    drawHand(hx + dx, 210 + dy, 120, RGB565(255, 205, 175), RGB565(160, 100, 80));
    centred(78 + dy, who, strlen(who) > 18 ? 2 : 3, RGB565_WHITE);
    centred(332 + dy, power, 2, RGB565(255, 220, 120));
    centred(362, tally, 1, RGB565(230, 200, 200));
    if (f == 12) sosPill("SLAP BACK", "OK");
    gfx->flush();
  }
  slapTarget = from;
  Serial.printf("sos    : showed slap from %s x%d (power %d), total %lu\n", SOS_NAME[from], count, s, (unsigned long)sosGot);
}

void slappedTap(int x, int y) {
  if (!sosOnPill(x, y)) return;
  if (x < DEC_PILL_X + DEC_PILL_W / 2) { slapArm(slapTarget); showScreen(ST_SLAP); }   // SLAP BACK
  else showScreen(ST_HOME);
}

// From loop(): take in MQTT traffic and show a slap if one arrived. Not during the setup pages.
void sosPoll() {
  if (sosApDownAt && millis() > sosApDownAt) {
    sosApDownAt = 0;
    if (WiFi.status() == WL_CONNECTED && apRunning) {
      WiFi.softAPdisconnect(true); apRunning = false;
      Serial.println("ap     : setup network closed after the badge settings were saved");
      if (screen == ST_APINFO) showScreen(ST_MENU);
    }
  }
  if (sosDrain() && screen != ST_APINFO && screen != ST_GALLERY_SYNC) showScreen(ST_SLAPPED);
}

// The gallery runs its own loop; it asks this between pictures so a slap is never missed.
bool sosSlapWaiting() { return sosDrain(); }

// ---------------------------------------------------------------- settings rows + setup page

// Settings rows (drawn by Badge.ino's drawMenu): the badge name and the sound switch.
const char *sosMeLabel() { return sosMe >= 0 ? SOS_NAME[sosMe] : "not chosen"; }
bool sosIsMuted() { return sosMuted; }

void sosCycleMe() {
  sosMe = (sosMe + 2) % (SOS_N + 1) - 1;          // -1 -> 0 -> 1 -> 2 -> -1 ...
  sosSaveConfig();
  Serial.printf("sos    : this badge is now %s\n", sosMeLabel());
  sosStart();
}

void sosToggleMute() {
  sosMuted = !sosMuted;
  sosSaveConfig();
  Serial.printf("sos    : slap sound %s\n", sosMuted ? "muted" : "on");
}

String sosHtmlEsc(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '\'') o += "&#39;";
    else if (c == '"') o += "&quot;"; else if (c == '&') o += "&amp;"; else o += c;
  }
  return o;
}

// The badge-network part of the phone setup page. The password is write-only: never sent back.
String sosSetupForm() {
  String f = F("<hr style='border:0;border-top:1px solid #334;margin:28px 0'>"
               "<h3 style='margin:0 0 6px'>Badge network</h3>"
               "<p style='color:#9ab'>Which person this badge belongs to, and your Mosquitto. "
               "Stored on the badge only.</p><form method='POST' action='/sos'>"
               "<p><select name='me' style='width:100%;padding:14px;font-size:16px;border-radius:8px'>");
  f += "<option value=''" + String(sosMe < 0 ? " selected" : "") + ">- this badge is... -</option>";
  for (int i = 0; i < SOS_N; i++)
    f += "<option value='" + String(SOS_ID[i]) + "'" + (i == sosMe ? " selected" : "") + ">" + SOS_NAME[i] + "</option>";
  const char *in = "style='width:100%;padding:14px;font-size:16px;border-radius:8px;box-sizing:border-box'";
  f += "</select></p><p><input name='host' placeholder='broker host, e.g. mqtt.example.com' autocomplete='off' " +
       String(in) + " value='" + sosHtmlEsc(sosHost) + "'></p>";
  f += "<p><input name='port' type='number' placeholder='port' " + String(in) + " value='" + String(sosPort) + "'></p>";
  f += "<p><label><input type='checkbox' name='tls' value='1'" + String(sosTls ? " checked" : "") +
       "> TLS (port 8883, public certificate e.g. Let's Encrypt)</label></p>";
  f += "<p><input name='user' placeholder='login (claudio / danny / walter)' autocomplete='off' " + String(in) +
       " value='" + sosHtmlEsc(sosUser) + "'></p>";
  f += "<p><input name='pw' type='password' autocomplete='off' placeholder='" +
       String(sosPass.length() ? "password (leave empty to keep it)" : "password") + "' " + String(in) + "></p>";
  f += "<p><button style='padding:14px 22px;font-size:16px;border-radius:8px'>Save badge settings</button></p></form>";
  f += "<p style='color:#667;font-size:13px'>Now: " + String(sosnet_status()) + "</p>";
  return f;
}

void sosHandleSave() {
  int me = sosIndex(server.arg("me").c_str());
  sosMe = me;
  sosHost = server.arg("host"); sosHost.trim();
  int port = server.arg("port").toInt();
  sosTls = server.arg("tls") == "1";
  sosPort = (port > 0 && port < 65536) ? port : (sosTls ? 8883 : 1883);
  sosUser = server.arg("user"); sosUser.trim();
  String pw = server.arg("pw");
  if (pw.length()) sosPass = pw;                    // empty = keep the saved one
  sosSaveConfig();
  Serial.printf("sos    : settings saved - badge %s, %s:%d %s, user '%s', password %u chars (not logged)\n",
                sosMeLabel(), sosHost.c_str(), sosPort, sosTls ? "TLS" : "plain", sosUser.c_str(), (unsigned)sosPass.length());
  server.send(200, "text/html",
    F("<body style='font-family:system-ui;background:#0e0e12;color:#eee;padding:26px'>"
      "<h2>Saved</h2><p>The badge is connecting to the broker - open SLAP on the badge to see who is online.</p>"
      "<p style='color:#9ab'>This setup network closes in a few seconds if the badge is already on Wi-Fi.</p></body>"));
  sosStart();
  if (WiFi.status() == WL_CONNECTED) sosApDownAt = millis() + 5000;
  if (screen == ST_SLAP) showScreen(ST_SLAP);
}
