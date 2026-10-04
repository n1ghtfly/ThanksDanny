/*
 * Rumours — plays a random rumour (a short MP3 about Danny) from the GitHub Pages site.
 *
 * The clips are AI-generated voice parodies of well-known people; the screen says so under the
 * speaker's name, and so does the list on the site. They are not real statements.
 *
 * Fourth .ino in the Badge sketch (concatenated after Badge, Decider, Gallery), so it reuses:
 *   - Gallery.ino : PIC_HOST, httpGetToFile(), httpGetToString(), syncQuiet - same site, same
 *                   download-into-LittleFS approach as the pictures, so rumours work offline
 *                   once fetched;
 *   - Decider.ino : decAudioInit(), decI2s, AUD_PA, decSoundStop(), decDrawPill() and the
 *                   AGAIN | HOME pill geometry - the same 16 kHz I2S + ES8311 audio path.
 *
 * Site layout (github-pics/rumours/, made by tools/prep_rumours.py):
 *   rumours.txt   one line per clip: "<file> <bytes> <speaker name with spaces>"
 *   rNN.mp3       16 kHz mono MP3, 48 kbit/s
 *
 * Decoding: the Helix fixed-point MP3 decoder in src/helix, driven by src/mp3stream.c. That loop
 * was checked on a PC against ffmpeg on these clips: correlation 1.0000 at zero lag, durations
 * matching, no memory errors under AddressSanitizer. The ESP8266Audio library is deliberately
 * NOT used: its I2S output uses the legacy ESP-IDF I2S driver, and linking that next to the new
 * driver (ESP_I2S, used here) aborts at boot with "CONFLICT! The new i2s driver can't work along
 * with the legacy i2s driver".
 *
 * Playback runs in a FreeRTOS task on core 0 (like the drum roll), so the screen and touch stay
 * live: the sound bars follow the real loudness of each decoded frame.
 */

#include "src/mp3stream.h"

#define RUM_DIR      "/rumours"
#define RUM_LIST     "/rumours/rumours.txt"
#define RUM_MAX      24
#define RUM_BARS     13
#define RUM_BAR_X0   112                   // the bar field, inside the circle
#define RUM_BAR_Y0   268
#define RUM_BAR_W    242
#define RUM_BAR_H    84
#define RUM_MAXSAMP  1152                  // samples per channel in one MP3 frame, at most

static String   rumFile[RUM_MAX];
static uint32_t rumBytes[RUM_MAX];
static String   rumWho[RUM_MAX];
static int      rumCount = 0;
static int      rumOrder[RUM_MAX];
static int      rumDeckPos = 0, rumDeckFor = -1, rumLast = -1;
static int      rumNow = -1;              // index playing (or last played)

static volatile bool rumBusy = false;     // the player task is running
static volatile bool rumStop = false;     // ask it to stop
static volatile int  rumLevel = 0;        // loudness of the last frame, 0..32767
static volatile uint32_t rumPlayedMs = 0; // how far into the clip
static uint32_t rumLastTick = 0;
static float    rumBar[RUM_BARS];

// ---------------------------------------------------------------- the list

// "<file> <bytes> <speaker...>" per line; '#' lines are comments.
int parseRumourList(const String &body) {
  rumCount = 0;
  int i = 0;
  while (i < (int)body.length() && rumCount < RUM_MAX) {
    int eol = body.indexOf('\n', i);
    if (eol < 0) eol = body.length();
    String line = body.substring(i, eol);
    i = eol + 1;
    line.trim();
    if (!line.length() || line.startsWith("#")) continue;
    int s1 = line.indexOf(' ');
    int s2 = (s1 < 0) ? -1 : line.indexOf(' ', s1 + 1);
    if (s1 < 0 || s2 < 0) continue;
    String name = line.substring(0, s1);
    if (!name.endsWith(".mp3")) continue;
    rumFile[rumCount]  = name;
    rumBytes[rumCount] = (uint32_t)line.substring(s1 + 1, s2).toInt();
    rumWho[rumCount]   = line.substring(s2 + 1);
    rumWho[rumCount].trim();
    rumCount++;
  }
  Serial.printf("rumours: list has %d clip(s)\n", rumCount);
  return rumCount;
}

// Offline: read the copy of the list saved by the last sync.
int loadCachedRumours() {
  File f = LittleFS.open(RUM_LIST, FILE_READ);
  if (!f) return 0;
  String body = f.readString();
  f.close();
  return parseRumourList(body);
}

// Fetch the list, keep a copy, and download any clip whose size changed. `quiet` = no screen.
// Returns the number of clips ready on the badge.
int rumoursSync(bool quiet) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("rumours: offline, using the cached set");
    return loadCachedRumours();
  }
  if (time(nullptr) < 1600000000UL) {             // TLS needs the clock (see Gallery.ino)
    for (int i = 0; i < 24 && time(nullptr) < 1600000000UL; i++) delay(250);
  }
  int code = 0;
  String list = httpGetToString(String(PIC_HOST) + "/rumours/rumours.txt", &code);
  if (code != 200 || !list.length()) {
    Serial.printf("rumours: list.txt -> HTTP %d, using the cached set\n", code);
    return loadCachedRumours();
  }
  if (!parseRumourList(list)) return 0;
  if (!LittleFS.exists(RUM_DIR)) LittleFS.mkdir(RUM_DIR);
  File lf = LittleFS.open(RUM_LIST, "w");
  if (lf) { lf.print(list); lf.close(); }

  int ready = 0;
  for (int n = 0; n < rumCount; n++) {
    String path = String(RUM_DIR) + "/" + rumFile[n];
    File f = LittleFS.open(path, FILE_READ);
    uint32_t have = f ? f.size() : 0;
    if (f) f.close();
    if (have == rumBytes[n]) { ready++; continue; }
    if (!quiet) {
      gfx->fillScreen(RGB565(10, 10, 14));
      centred(196, "Rumours", 3, RGB565_WHITE);
      char buf[40];
      snprintf(buf, sizeof(buf), "fetching %d of %d", n + 1, rumCount);
      centred(240, buf, 2, RGB565(150, 220, 255));
      gfx->flush();
    }
    uint32_t got = 0;
    bool usedBundle = true;
    int c = httpGetToFile(String(PIC_HOST) + "/rumours/" + rumFile[n], path.c_str(), &got, &usedBundle);
    File g = LittleFS.open(path, FILE_READ);
    uint32_t now = g ? g.size() : 0;
    if (g) g.close();
    Serial.printf("rumours: %s -> HTTP %d, %lu of %lu bytes\n", rumFile[n].c_str(), c,
                  (unsigned long)now, (unsigned long)rumBytes[n]);
    if (now == rumBytes[n]) ready++;
  }
  Serial.printf("rumours: %d of %d clip(s) on the badge\n", ready, rumCount);
  int gone = pruneDir(RUM_DIR, rumFile, rumCount, "rumours.txt");   // Gallery.ino
  if (gone) Serial.printf("rumours: removed %d clip(s) no longer on the site\n", gone);
  return ready;
}

// Called at boot after the picture fetch: quiet when a set is already cached.
void rumoursBootFetch() {
  if (WiFi.status() != WL_CONNECTED) return;
  rumoursSync(true);
}

// ---------------------------------------------------------------- picking one

int nextRumour() {
  if (rumCount <= 0) return -1;
  if (rumDeckFor != rumCount || rumDeckPos >= rumCount) {
    for (int i = 0; i < rumCount; i++) rumOrder[i] = i;
    for (int i = rumCount - 1; i > 0; i--) {
      int j = (int)(esp_random() % (uint32_t)(i + 1));
      int t = rumOrder[i]; rumOrder[i] = rumOrder[j]; rumOrder[j] = t;
    }
    // A new deck must not start with the clip that just finished the old one.
    if (rumCount > 1 && rumOrder[0] == rumLast) { int t = rumOrder[0]; rumOrder[0] = rumOrder[1]; rumOrder[1] = t; }
    rumDeckPos = 0;
    rumDeckFor = rumCount;
  }
  rumLast = rumOrder[rumDeckPos++];
  return rumLast;
}

// ---------------------------------------------------------------- the player (core 0)

static File rumFileHandle;

int rumRead(void *ctx, unsigned char *buf, int n) {
  if (rumStop) return 0;
  return rumFileHandle.read(buf, n);
}

// One decoded frame: downmix to mono if needed, measure it for the bars, write it as stereo.
int rumPcm(void *ctx, const short *pcm, int samples, int channels, int rate) {
  static int16_t blk[RUM_MAXSAMP * 2];
  if (rumStop) return 0;
  long long sq = 0;
  int n = samples > RUM_MAXSAMP ? RUM_MAXSAMP : samples;
  for (int i = 0; i < n; i++) {
    int v = (channels == 2) ? ((int)pcm[2 * i] + pcm[2 * i + 1]) / 2 : pcm[i];
    blk[2 * i] = blk[2 * i + 1] = (int16_t)v;
    sq += (long long)v * v;
  }
  rumLevel = (int)sqrtf((float)(sq / (n > 0 ? n : 1)));
  rumPlayedMs += (uint32_t)n * 1000 / (rate > 0 ? rate : 16000);
  decI2s.write((uint8_t *)blk, n * 4);
  return 1;
}

void rumPlayerTask(void *arg) {
  int idx = (int)(intptr_t)arg;
  String path = String(RUM_DIR) + "/" + rumFile[idx];
  uint32_t t0 = millis();
  rumFileHandle = LittleFS.open(path, FILE_READ);
  if (!rumFileHandle) {
    Serial.printf("rumours: cannot open %s\n", path.c_str());
  } else {
    mp3_result r = mp3_stream(rumRead, rumPcm, nullptr);
    rumFileHandle.close();
    Serial.printf("rumours: %s (%s) %s - %d frames, %d Hz, %d ch, %lu ms%s\n", rumFile[idx].c_str(),
                  rumWho[idx].c_str(), r.stopped || rumStop ? "stopped" : "finished", r.frames, r.rate,
                  r.channels, (unsigned long)(millis() - t0), r.error ? " - DECODER OUT OF MEMORY" : "");
    if (r.rate && r.rate != DEC_SND_RATE)
      Serial.printf("rumours: ! %s is %d Hz, the badge plays at %d Hz - run tools/prep_rumours.py\n",
                    rumFile[idx].c_str(), r.rate, DEC_SND_RATE);
  }
  static int16_t zeros[512];
  for (int i = 0; i < 6; i++) decI2s.write((uint8_t *)zeros, sizeof(zeros));   // flush with silence
  digitalWrite(AUD_PA, LOW);
  rumLevel = 0;
  rumBusy = false;
  vTaskDelete(nullptr);
}

void rumoursStop() {
  if (!rumBusy) return;
  rumStop = true;
  for (int i = 0; i < 100 && rumBusy; i++) delay(5);
}

bool rumoursPlay(int idx) {
  if (idx < 0 || !decAudioInit()) return false;
  decSoundStop();                       // never on top of the drum roll
  rumoursStop();
  rumStop = false;
  rumBusy = true;
  rumPlayedMs = 0;
  rumNow = idx;
  digitalWrite(AUD_PA, HIGH);
  // 8 kB stack: Helix keeps its big buffers on the heap / in statics, the frame loop is shallow.
  if (xTaskCreatePinnedToCore(rumPlayerTask, "rumour", 8192, (void *)(intptr_t)idx, 5, nullptr, 0) != pdPASS) {
    rumBusy = false;
    digitalWrite(AUD_PA, LOW);
    Serial.println("rumours: could not start the player task");
    return false;
  }
  Serial.printf("rumours: playing %s - %s\n", rumFile[idx].c_str(), rumWho[idx].c_str());
  return true;
}

// ---------------------------------------------------------------- the screen

// A megaphone, in units of r, in the same line style as the other carousel icons.
void drawRumoursIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  cx -= (r * 14) / 100;
  int bx0 = cx - (r * 72) / 100, bx1 = cx - (r * 42) / 100;       // back of the horn
  int mx  = cx + (r * 34) / 100;                                    // mouth
  gfx->fillRect(bx0, cy - (r * 22) / 100, bx1 - bx0, (r * 44) / 100, fg);
  gfx->fillTriangle(bx1, cy - (r * 22) / 100, mx, cy - (r * 60) / 100, mx, cy + (r * 60) / 100, fg);
  gfx->fillTriangle(bx1, cy - (r * 22) / 100, bx1, cy + (r * 22) / 100, mx, cy + (r * 60) / 100, fg);
  gfx->fillRect(mx, cy - (r * 64) / 100, (r * 10) / 100 + 1, (r * 128) / 100, fg);       // rim
  gfx->fillRect(cx - (r * 36) / 100, cy + (r * 20) / 100, (r * 14) / 100 + 1, (r * 36) / 100, fg); // handle
  // Two sound waves out of the mouth, drawn as dots along arcs.
  int dot = r / 14; if (dot < 1) dot = 1;
  for (int w = 0; w < 2; w++) {
    float rad = r * (0.30f + 0.26f * w);
    for (int a = -38; a <= 38; a += 7) {
      float t = a * PI / 180.0f;
      gfx->fillCircle(mx + (r * 12) / 100 + (int)(cosf(t) * rad), cy + (int)(sinf(t) * rad), dot, fg);
    }
  }
}

// Sound bars: their height follows the loudness of the clip, with a little per-bar variation so
// it reads as a voice rather than a single block. Drawn into the canvas; the caller flushes.
void rumDrawBars(bool playing) {
  gfx->fillRect(RUM_BAR_X0, RUM_BAR_Y0, RUM_BAR_W, RUM_BAR_H, RGB565(8, 10, 18));
  float lvl = playing ? fminf(1.0f, rumLevel / 9000.0f) : 0.0f;
  int bw = RUM_BAR_W / RUM_BARS;
  for (int i = 0; i < RUM_BARS; i++) {
    float shape = 0.55f + 0.45f * sinf(i * 0.9f + millis() * 0.011f + i * i * 0.3f);
    float want = lvl * shape;
    rumBar[i] += (want - rumBar[i]) * (want > rumBar[i] ? 0.7f : 0.25f);   // fast up, slow down
    int h = 4 + (int)(rumBar[i] * (RUM_BAR_H - 6));
    int x = RUM_BAR_X0 + i * bw + 3;
    int y = RUM_BAR_Y0 + (RUM_BAR_H - h) / 2;
    uint16_t c = playing ? RGB565(120 + (int)(rumBar[i] * 120), 200, 255) : RGB565(50, 70, 95);
    gfx->fillRoundRect(x, y, bw - 6, h, 3, c);
  }
}

void rumDrawScreen() {
  gfx->fillScreen(RGB565(8, 10, 18));
  drawRumoursIcon(233, 92, 44, RGB565(150, 230, 255), RGB565(8, 10, 18));
  centred(152, "RUMOURS", 3, RGB565(150, 230, 255));
  if (rumNow >= 0) {
    const char *who = rumWho[rumNow].c_str();
    int sz = strlen(who) <= 14 ? 3 : 2;
    centred(198, who, sz, RGB565_WHITE);
    centred(234, "AI voice parody", 1, RGB565(140, 140, 160));
  } else {
    centred(204, "no rumours yet", 2, RGB565(255, 220, 120));
  }
  rumDrawBars(rumBusy);
  decDrawPill();                                  // AGAIN | HOME, same as the Decider
}

void showRumours() {
  void contentSyncWait();                         // Gallery.ino: a background update may be running
  contentSyncWait();
  if (rumCount == 0) loadCachedRumours();
  if (rumCount == 0) rumoursSync(false);          // first time: fetch, with progress on screen
  int idx = nextRumour();
  if (idx < 0) {
    rumNow = -1;
    rumDrawScreen();
    centred(330, WiFi.status() == WL_CONNECTED ? "none on the site yet" : "connect Wi-Fi first", 1,
            RGB565(180, 180, 195));
    gfx->flush();
    return;
  }
  rumNow = idx;
  for (int i = 0; i < RUM_BARS; i++) rumBar[i] = 0;
  rumoursPlay(idx);
  rumDrawScreen();
  gfx->flush();
  rumLastTick = millis();
}

// From loop(): animate the bars ~20 times a second while the Rumours screen is up.
void rumoursTick() {
  if (millis() - rumLastTick < 50) return;
  rumLastTick = millis();
  rumDrawBars(rumBusy);
  flushRect(RUM_BAR_X0, RUM_BAR_Y0, RUM_BAR_W, RUM_BAR_H);
}

// A tap on the Rumours screen: HOME (right half of the pill) leaves, anything else = another one.
void rumoursTap(int x, int y) {
  bool onPill = x >= DEC_PILL_X - 10 && x <= DEC_PILL_X + DEC_PILL_W + 10 &&
                y >= DEC_PILL_Y - 12 && y <= DEC_PILL_Y + DEC_PILL_H + 12;
  if (onPill && x >= DEC_PILL_X + DEC_PILL_W / 2) {
    Serial.println("         -> rumours: home");
    showScreen(ST_HOME);
  } else {
    Serial.println("         -> rumours: another one");
    showScreen(ST_RUMOURS);
  }
}
