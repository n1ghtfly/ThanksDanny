/*
 * Gallery — the badge's picture app: fetch from GitHub, then scroll randomly through them.
 *
 * This is a second .ino in the Badge sketch folder. Arduino concatenates the .ino files of a
 * sketch (main file first), so this shares Badge.ino's globals - gfx, screen, showScreen(),
 * centred(), insidePanel(), LittleFS - without a header. Keep every function signature here to
 * PRIMITIVE types only: Arduino auto-generates prototypes, and a custom type in a signature
 * fails to compile at the point where the prototype is spliced in above the type's definition.
 *
 * How it works
 *   1. list.txt is fetched over HTTPS and parsed (no JSON library: "<file> <bytes> <w> <h>").
 *   2. Each picture whose size on disk differs is downloaded into /pics on LittleFS.
 *      The cache survives an app flash - only reflashing the filesystem image wipes it.
 *   3. Pictures are shown in a shuffled random order. Each one is shown STILL, centred in the
 *      round window, for HOLD_MS; then a short slide brings in the next. A tap skips to the
 *      next picture straight away; HOLD (0.7 s) or BOOT goes home. No button is drawn over
 *      the pictures (v2.7 removed the cog).
 *
 *      v2: the pictures used to PAN slowly across the window for 13 s (an 835-wide landscape
 *      is wider than the 466 panel). The user did not want that - "it shows the pictures but
 *      then they scroll" - so the window is now fixed: centred horizontally, and a third of
 *      the way down for tall pictures, where the subject usually is.
 *
 * Every frame is composed into the canvas framebuffer (Badge.ino's `gfx`) and sent with ONE
 * flush, so the cog and the caption land in the same frame as the picture - no flicker, and
 * no extra blits. A tap is taken on the PRESS EDGE only: the old code treated a finger that was
 * still down as a fresh tap on the next screen, so one tap could skip two or three pictures.
 *
 * TLS: NetworkClientSecure defaults to _use_insecure = false AND _use_ca_bundle = false, which
 * is a handshake failure, not a secure default. useBuiltinCACertBundle() turns on real
 * validation (the core's own example does exactly this); if that handshake fails we fall back
 * to setInsecure() so the feature still works, and say so in the log.
 */

#include <NetworkClientSecure.h>
#include <HTTPClient.h>

#define ST_GALLERY      4
#define ST_GALLERY_SYNC 5

#define PICS_DIR   "/pics"
#define PIC_HOST   "https://n1ghtfly.github.io/danny-pics-7f3c9a"

#define MAX_PICS       40
#define PANEL_PX       466
#define HOLD_MS        10000      // each picture stays up, still, for this long
#define CAPTION_MS      2500      // the "3/6 name" caption shows this long, then goes away
#define SLIDE_FRAMES   12         // short slide between pictures (~0.5 s)
#define MAX_FRAME_PX   ((long)1100 * PANEL_PX)   // the PSRAM decode buffers hold this many pixels
#define SYNC_TIMEOUT   12000

static String   picFile[MAX_PICS];
static uint32_t picBytes[MAX_PICS];
static int      picW[MAX_PICS];
static int      picH[MAX_PICS];
int             picCount = 0;            // read by Badge.ino's tap handler, so not static

static int      picOrder[MAX_PICS];      // shuffled deck
static int      deckPos = 0;             // where we are in it
static int      picShown = 0;            // 1-based index of the picture on screen, for the count

static uint16_t *frameA = nullptr;
static uint16_t *frameB = nullptr;
static uint16_t *slideWin = nullptr;     // = the canvas framebuffer; flush() sends it to the glass
static int      curW = 0, curH = 0;      // loaded size of the current picture

static int      syncState = 0;           // 0 idle, 1 listing, 2 downloading, 3 done, 4 failed
static String   syncNote;
static int      syncDone = 0, syncTotal = 0;

// A sync that runs quietly: no screen takeover, no progress on the glass. The boot fetch uses it
// when pictures are already cached, so the home screen is not interrupted by a check that
// usually finds nothing to do. It still logs everything.
bool            syncQuiet = false;

// The picture decoded ahead of time by the hold, so the transition has nothing to wait for.
// -1 means nothing is waiting.
static int      pendIdx = -1;
static int      pendW = 0, pendH = 0;

// Self-seeding xorshift. A fixed seed would replay the same picture order after every boot,
// which is the one thing a "random" slideshow must not do.
static uint32_t gRng = 0;
static uint32_t nextRand() {
  if (gRng == 0) {
    gRng = esp_random();
    if (gRng == 0) gRng = 0x9e3779b9;
  }
  gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5;
  return gRng;
}

// ---------------------------------------------------------------- decoding into PSRAM

// The decoder hands us blocks wherever it likes. Badge.ino's callback draws them straight to
// the panel; when decDest is set we instead pack them into a PSRAM frame, which is what makes
// scrolling possible (you cannot scroll what has already been thrown at the glass).
uint16_t *decDest = nullptr;
int       decDestW = 0;

bool jpegBlockTo(uint16_t *dest, int destW, int16_t x, int16_t y, uint16_t w, uint16_t h,
                 uint16_t *bitmap) {
  if (dest == nullptr) return false;
  for (int j = 0; j < h; j++) {
    memcpy(dest + (size_t)(y + j) * destW + x, bitmap + (size_t)j * w, (size_t)w * 2);
  }
  return true;
}

// Decode one picture into a PSRAM frame. Returns true and fills outW/outH on success.
//
// The size is read from the JPEG itself, not trusted from the caller. The caller's numbers come
// from list.txt - or, offline, from a guess of 466x466 - and the width is the decode STRIDE: a
// wrong one shears the picture into diagonal stripes, which is what an offline boot used to show.
bool decodeToFrame(const char *path, uint16_t *dest, int destW, int expectH, int *outW, int *outH) {
  // TJpg_Decoder's getFsJpgSize() and drawFsJpg() both CLOSE the File they are given (checked in
  // its source). So the size is read from one handle and the picture decoded from a second,
  // fresh one. v2 first shipped reusing the closed handle: every decode failed and the picture
  // app showed nothing.
  File probe = LittleFS.open(path, FILE_READ);
  if (!probe) { Serial.printf("gallery: cannot open %s\n", path); return false; }
  uint16_t jw = 0, jh = 0;
  if (TJpgDec.getFsJpgSize(&jw, &jh, probe) == JDR_OK && jw > 0 && jh > 0) {
    if (jw != destW || jh != expectH)
      Serial.printf("gallery: %s is really %ux%u (list said %dx%d)\n", path, jw, jh, destW, expectH);
    destW = jw;
    expectH = jh;
  } else {
    Serial.printf("gallery: %s - could not read the JPEG header, using %dx%d\n", path, destW, expectH);
  }
  if (probe) probe.close();
  if ((long)destW * (long)expectH > MAX_FRAME_PX || destW < PANEL_PX / 2 || expectH < PANEL_PX / 2) {
    Serial.printf("gallery: %s %dx%d does not fit the frame, skipped\n", path, destW, expectH);
    return false;
  }
  File f = LittleFS.open(path, FILE_READ);
  if (!f) { Serial.printf("gallery: cannot reopen %s\n", path); return false; }
  // A picture narrower or shorter than the panel would leave stale pixels round it.
  if (destW < PANEL_PX || expectH < PANEL_PX) memset(dest, 0, (size_t)MAX_FRAME_PX * 2);
  decDest = dest;
  decDestW = destW;
  uint32_t t0 = millis();
  JRESULT r = TJpgDec.drawFsJpg(0, 0, f);
  decDest = nullptr;
  f.close();
  *outW = destW;
  *outH = expectH;
  Serial.printf("gallery: decoded %s result %d (%s) in %lu ms\n",
                path, (int)r, r == JDR_OK ? "ok" : "FAILED", millis() - t0);
  return r == JDR_OK;
}

// ---------------------------------------------------------------- drawing a scrolled window

// Where the still window sits on a picture: centred across, and for a tall picture a third of
// the way down (portraits keep their subject high). Negative means the picture is smaller than
// the panel on that axis and gets centred with black round it.
int windowX(int w) { return (w - PANEL_PX) / 2; }
int windowY(int h) { return h > PANEL_PX ? (h - PANEL_PX) / 3 : (h - PANEL_PX) / 2; }

// Copy `count` pixels of source row `sy`, starting at source column `sx`, to `dst`. Anything
// outside the picture comes out black, so no offset can read past the buffer.
void copySpan(uint16_t *dst, int count, uint16_t *frame, int frameW, int frameH, int sx, int sy) {
  if (count <= 0) return;
  if (sy < 0 || sy >= frameH) { memset(dst, 0, (size_t)count * 2); return; }
  int lead = 0;
  if (sx < 0) { lead = -sx; if (lead > count) lead = count; memset(dst, 0, (size_t)lead * 2); }
  int from = sx + lead;
  int n = count - lead;
  int avail = frameW - from;
  if (avail < 0) avail = 0;
  int take = n < avail ? n : avail;
  if (take > 0) memcpy(dst + lead, frame + (size_t)sy * frameW + from, (size_t)take * 2);
  if (n - take > 0) memset(dst + lead + take, 0, (size_t)(n - take) * 2);
}

// Put the 466x466 window of `frame` whose top-left is (ox, oy) into the canvas. Does NOT flush:
// the caller adds the cog / caption on top and flushes once.
void composeWindow(uint16_t *frame, int frameW, int frameH, int ox, int oy) {
  for (int y = 0; y < PANEL_PX; y++)
    copySpan(slideWin + (size_t)y * PANEL_PX, PANEL_PX, frame, frameW, frameH, ox, oy + y);
}

// Compose and send in one go - used where nothing is drawn on top (the boot warm-up).
void blitWindow(uint16_t *frame, int frameW, int frameH, int ox, int oy) {
  composeWindow(frame, frameW, frameH, ox, oy);
  gfx->flush();
}

// One frame of the slide: `oldF` pushed out to the left by d, `newF` coming in from the right.
void slideFrame(uint16_t *oldF, int oldW, int oldH, int oldOX, int oldOY,
                uint16_t *newF, int newW, int newH, int newOX, int newOY, int d) {
  int keep = PANEL_PX - d;                       // how much of the old picture is still visible
  if (keep < 0) keep = 0;
  for (int y = 0; y < PANEL_PX; y++) {
    uint16_t *dst = slideWin + (size_t)y * PANEL_PX;
    copySpan(dst, keep, oldF, oldW, oldH, oldOX + d, oldOY + y);
    copySpan(dst + keep, PANEL_PX - keep, newF, newW, newH, newOX, newOY + y);
  }
  gfx->flush();
}

// Touch in the gallery (v2.7): TAP = next picture, HOLD (GAL_HOLD_MS) = back to the main screen.
// There is no on-screen button any more - the cog that used to sit over every picture is gone.
// Lift is time-based (RELEASE_MS without a report), like the home screen: the touch driver reads
// "no finger" between the controller's reports, and an edge detector counted those gaps as lifts,
// so one long press could skip several pictures.
// Returns 0 nothing, 1 tap (on the lift), 2 hold (once, while still held).
#define GAL_HOLD_MS 700
static bool     gDown = false, gUsed = false;
static uint32_t gDownAt = 0, gSeen = 0;
int galleryGesture() {
  if (!touchOK) return 0;
  int16_t xs[1], ys[1];
  uint32_t now = millis();
  if (touch.getPoint(xs, ys, 1) > 0) {
    gSeen = now;
    if (!gDown) { gDown = true; gDownAt = now; gUsed = false; }
    if (!gUsed && now - gDownAt >= GAL_HOLD_MS) { gUsed = true; return 2; }
    return 0;
  }
  if (gDown && now - gSeen >= RELEASE_MS) {
    gDown = false;
    if (!gUsed) return 1;
  }
  return 0;
}

// The finger that opened the gallery is probably still on the glass: treat it as used up.
void galleryIgnoreCurrentPress() {
  gDown = true; gUsed = true; gSeen = millis();
}

// ---------------------------------------------------------------- chrome


void drawGalleryPill(const char *text) {
  gfx->fillRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, 12, RGB565(10, 10, 14));
  gfx->drawRoundRect(PILL_X, PILL_Y, PILL_W, PILL_H, 12, RGB565(70, 70, 86));
  gfx->setTextSize(2);                      // size 1 was unreadable on the real glass
  gfx->setTextColor(RGB565(225, 232, 245));
  int16_t w = strlen(text) * 12;            // 12 px per character at size 2
  gfx->setCursor(PILL_X + (PILL_W - w) / 2, PILL_Y + (PILL_H - 16) / 2);
  gfx->print(text);
}

// ---------------------------------------------------------------- fetching

String galleryUrl(const char *leaf) {
  return String(PIC_HOST) + "/" + leaf;
}

// One GET onto the panel's filesystem, writing the body to `path`.
// Tries certificate validation first, then falls back to an unvalidated connection once, so a
// bundle problem cannot make the whole app dead. The body is written on BOTH paths - getting
// that wrong once meant a 113 kB picture was fetched, discarded, and reported as HTTP 200.
int httpGetToFile(const String &url, const char *path, uint32_t *got, bool *usedBundle) {
  if (got) *got = 0;
  if (usedBundle) *usedBundle = false;

  for (int attempt = 0; attempt < 2; attempt++) {
    bool validated = (attempt == 0);
    NetworkClientSecure client;
    if (validated) client.useBuiltinCACertBundle();
    else           client.setInsecure();

    HTTPClient http;
    http.setConnectTimeout(SYNC_TIMEOUT);
    http.setTimeout(SYNC_TIMEOUT);
    if (!http.begin(client, url)) {
      Serial.printf("gallery: %s begin() failed (%s)\n", url.c_str(),
                    validated ? "validated" : "unvalidated");
      http.end();
      continue;
    }

    int code = http.GET();
    if (code == 200) {
      File f = LittleFS.open(path, "w");
      if (!f) {
        Serial.printf("gallery: cannot open %s for writing\n", path);
      } else {
        int n = http.writeToStream(&f);
        f.close();
        if (got) *got = (uint32_t)(n > 0 ? n : 0);
        Serial.printf("gallery: wrote %d bytes to %s\n", n, path);
      }
      http.end();
      if (usedBundle) *usedBundle = validated;
      return code;
    }

    http.end();
    Serial.printf("gallery: %s -> %d (%s)%s\n", url.c_str(), code,
                  validated ? "validated" : "unvalidated",
                  validated ? ", retrying unvalidated" : ", giving up");
  }
  return -1;
}

String httpGetToString(const String &url, int *codeOut) {
  NetworkClientSecure client;
  client.useBuiltinCACertBundle();
  HTTPClient http;
  http.setConnectTimeout(SYNC_TIMEOUT);
  http.setTimeout(SYNC_TIMEOUT);
  String body;
  int code = -1;
  if (http.begin(client, url)) {
    code = http.GET();
    if (code == 200) body = http.getString();
    http.end();
  }
  if (code != 200) {
    NetworkClientSecure insecure;
    insecure.setInsecure();
    HTTPClient http2;
    http2.setConnectTimeout(SYNC_TIMEOUT);
    http2.setTimeout(SYNC_TIMEOUT);
    if (http2.begin(insecure, url)) {
      code = http2.GET();
      if (code == 200) body = http2.getString();
      http2.end();
    }
  }
  if (codeOut) *codeOut = code;
  return body;
}

// Parse "<file> <bytes> <w> <h>" per line. Deliberately not JSON: the badge has no JSON
// library and adding one costs flash for nothing.
int parsePicList(const String &body) {
  picCount = 0;
  int i = 0;
  while (i < (int)body.length() && picCount < MAX_PICS) {
    int eol = body.indexOf('\n', i);
    if (eol < 0) eol = body.length();
    String line = body.substring(i, eol);
    i = eol + 1;
    line.trim();
    if (!line.length() || line.startsWith("#")) continue;
    int s1 = line.indexOf(' ');
    if (s1 < 0) continue;
    int s2 = line.indexOf(' ', s1 + 1);
    int s3 = (s2 < 0) ? -1 : line.indexOf(' ', s2 + 1);
    if (s2 < 0 || s3 < 0) continue;
    String name = line.substring(0, s1);
    uint32_t bytes = (uint32_t)line.substring(s1 + 1, s2).toInt();
    int w = line.substring(s2 + 1, s3).toInt();
    int h = line.substring(s3 + 1).toInt();
    if (!name.length() || w < 64 || h < 64) continue;
    // The PSRAM frame is sized for 1100 x panel. An entry larger than that would decode past
    // the end of the buffer, so skip it rather than trust numbers published elsewhere.
    if ((long)w * (long)h > (long)1100 * (long)PANEL_PX) {
      Serial.printf("gallery: skipping %s (%dx%d exceeds the frame)\n", name.c_str(), w, h);
      continue;
    }
    picFile[picCount] = name;
    picBytes[picCount] = bytes;
    picW[picCount] = w;
    picH[picCount] = h;
    picCount++;
  }
  Serial.printf("gallery: list has %d picture(s)\n", picCount);
  return picCount;
}

// Download the list and any picture whose cached size does not match. The size check is what
// makes a re-fetch cheap and idempotent: unchanged files are not downloaded again.
void syncPictures() {
  syncState = 1;
  syncNote = "checking GitHub...";
  syncDone = 0; syncTotal = 0;
  // A quiet sync leaves the screen alone (drawGallerySync() also refuses to draw), so the home
  // screen stays put and the badge does not appear to do anything when there is nothing new.
  if (!syncQuiet) showScreen(ST_GALLERY_SYNC);

  if (WiFi.status() != WL_CONNECTED) {
    syncState = 4;
    syncNote = "no wifi - showing cached";
    Serial.println("gallery: no network, using what is cached");
    return;
  }

  // TLS validates the certificate's validity dates. This board has no RTC and starts at 1970,
  // so every certificate looks "not yet valid" until SNTP lands - which presents as a download
  // that silently never happens. Wait for a sane clock before the first handshake.
  Serial.printf("gallery: heap %u kB, clock %lu\n",
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned long)time(nullptr));
  if (time(nullptr) < 1600000000UL) {
    Serial.println("gallery: waiting for SNTP to set the clock");
    for (int i = 0; i < 24 && time(nullptr) < 1600000000UL; i++) delay(250);
    Serial.printf("gallery: clock is now %lu\n", (unsigned long)time(nullptr));
  }

  int code = 0;
  String list = httpGetToString(galleryUrl("list.txt"), &code);
  if (code != 200 || !list.length()) {
    syncState = 4;
    // Put the code on the glass: "download failed" with no number is not a diagnosis.
    syncNote = "list HTTP " + String(code);
    Serial.printf("gallery: list.txt -> code %d, %u bytes, clock %lu, heap %u kB\n",
                  code, (unsigned)list.length(), (unsigned long)time(nullptr),
                  (unsigned)(ESP.getFreeHeap() / 1024));
    return;
  }
  Serial.printf("gallery: list.txt fetched, %u bytes\n", (unsigned)list.length());
  if (!parsePicList(list)) { syncState = 4; syncNote = "list empty"; return; }

  // Directories: this LittleFS build does support them (the wallpaper folder proves it), but a
  // failed mkdir would send every download nowhere, so the result is logged rather than assumed.
  if (!LittleFS.exists(PICS_DIR)) {
    bool made = LittleFS.mkdir(PICS_DIR);
    Serial.printf("gallery: mkdir %s -> %s\n", PICS_DIR, made ? "ok" : "FAILED");
  }
  Serial.printf("gallery: %s exists: %s\n", PICS_DIR, LittleFS.exists(PICS_DIR) ? "yes" : "NO");

  syncState = 2;
  syncTotal = picCount;
  for (int n = 0; n < picCount; n++) {
    String path = String(PICS_DIR) + "/" + picFile[n];
    syncDone = n;
    syncNote = picFile[n];
    showScreen(ST_GALLERY_SYNC);

    File f = LittleFS.open(path, FILE_READ);
    uint32_t have = f ? f.size() : 0;
    if (f) f.close();
    if (have == picBytes[n]) continue;          // already cached, byte-for-byte

    uint32_t got = 0;
    bool usedBundle = true;
    uint32_t h0 = ESP.getFreeHeap();
    int c = httpGetToFile(galleryUrl(picFile[n].c_str()), path.c_str(), &got, &usedBundle);
    File g = LittleFS.open(path, FILE_READ);
    uint32_t now = g ? g.size() : 0;
    if (g) g.close();
    Serial.printf("gallery: %s -> HTTP %d, %lu of %lu bytes (tls %s, heap %u->%u kB)\n",
                  picFile[n].c_str(), c, (unsigned long)now, (unsigned long)picBytes[n],
                  usedBundle ? "validated" : "unvalidated",
                  (unsigned)(h0 / 1024), (unsigned)(ESP.getFreeHeap() / 1024));
    if (now != picBytes[n]) {
      syncState = 4;
      syncNote = String(picFile[n]) + " HTTP " + String(c);
      return;
    }
    delay(1);                       // let the idle task run between downloads
  }
  syncDone = picCount;
  syncState = 3;
  syncNote = "up to date";
  Serial.println("gallery: cache up to date");
}

// Count what is on the filesystem without the network, so the app works offline.
int loadCachedPictures() {
  picCount = 0;
  File dir = LittleFS.open(PICS_DIR);
  if (!dir || !dir.isDirectory()) return 0;
  File e = dir.openNextFile();
  while (e && picCount < MAX_PICS) {
    String name = e.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.endsWith(".jpg") || name.endsWith(".JPG")) {
      // Dimensions come from the list when we have it; otherwise this placeholder - the real
      // size is read from the JPEG header at decode time (decodeToFrame), so a guess is harmless.
      picFile[picCount] = name;
      picBytes[picCount] = e.size();
      picW[picCount] = PANEL_PX;
      picH[picCount] = PANEL_PX;
      picCount++;
    }
    e = dir.openNextFile();
  }
  dir.close();
  return picCount;
}

// ---------------------------------------------------------------- the show

// Which picture count the current order was shuffled for. -1 means "not shuffled yet".
static int deckFor = -1;

void shuffleDeck() {
  for (int i = 0; i < picCount; i++) picOrder[i] = i;
  for (int i = picCount - 1; i > 0; i--) {
    int j = (int)(nextRand() % (uint32_t)(i + 1));
    int t = picOrder[i]; picOrder[i] = picOrder[j]; picOrder[j] = t;
  }
  deckPos = 0;
  deckFor = picCount;
  Serial.printf("gallery: deck shuffled for %d picture(s)\n", picCount);
}

int picPathFor(int idx) { return idx; }

// True once every picture has been shown; then reshuffle so it never repeats a run.
int nextInDeck() {
  if (picCount == 0) return -1;
  // The order is only meaningful once it has been shuffled FOR THIS MANY pictures. Shuffling
  // once at boot was a no-op (picCount was still 0), which left picOrder full of zeros and
  // replayed the first picture over and over. Tying it to picCount also re-shuffles correctly
  // after a sync changes how many pictures exist.
  if (deckFor != picCount || deckPos >= picCount) shuffleDeck();
  return picOrder[deckPos++];
}

bool loadPictureInto(uint16_t *dest, int *outW, int *outH) {
  int idx = nextInDeck();
  if (idx < 0) return false;
  String path = String(PICS_DIR) + "/" + picFile[idx];
  int w = picW[idx], h = picH[idx];
  // Fall back to the list's dimensions if the cache scan gave us defaults.
  if (w == PANEL_PX && h == PANEL_PX && picBytes[idx] == 0) { w = PANEL_PX; h = PANEL_PX; }
  bool ok = decodeToFrame(path.c_str(), dest, w, h, outW, outH);
  picShown = idx + 1;
  return ok;
}

// Decode the next picture in the deck into `dest` now, ahead of the transition. Called from the
// hold so the ~350 ms JPEG decode overlaps the 13-second pan instead of landing between two
// pictures, where it read as the slideshow hesitating.
bool predecodeNext(uint16_t *dest) {
  if (pendIdx >= 0) return true;                 // one is already waiting
  int idx = nextInDeck();
  if (idx < 0) return false;
  String path = String(PICS_DIR) + "/" + picFile[idx];
  uint32_t t0 = millis();
  Serial.printf("gallery: pre-decoding %s during the hold\n", picFile[idx].c_str());
  if (!decodeToFrame(path.c_str(), dest, picW[idx], picH[idx], &pendW, &pendH)) {
    Serial.printf("gallery: pre-decode of %s FAILED - the transition will decode it\n",
                  picFile[idx].c_str());
    return false;
  }
  pendIdx = idx;
  Serial.printf("gallery: pre-decoded %s %dx%d in %lu ms\n", picFile[idx].c_str(), pendW, pendH,
                (unsigned long)(millis() - t0));
  return true;
}

// Compose the current picture's still window (and the caption, if wanted) and send it as one
// frame. Nothing else is drawn over the picture.
void drawStill(uint16_t *src, int w, int h, const char *caption) {
  composeWindow(src, w, h, windowX(w), windowY(h));
  if (caption) drawGalleryPill(caption);
  gfx->flush();
}

// Hold the picture STILL for HOLD_MS. Nothing is redrawn while it sits there (the old version
// re-sent a full frame every 40 ms to pan it), so the loop does nothing but watch the touch and
// the BOOT button - which is why a tap now answers at once. The caption is taken off after
// CAPTION_MS, and the next picture is decoded into `spare` early on so the change has no wait.
// Returns: 0 time up / tap (go to next picture), 1 left the gallery.
int holdStill(uint16_t *src, int w, int h, uint16_t *spare) {
  uint32_t start = millis();
  bool decodedAhead = false, captionUp = true;
  while (millis() - start < HOLD_MS) {
    if (!decodedAhead) { decodedAhead = true; predecodeNext(spare); }
    if (captionUp && millis() - start > CAPTION_MS) {
      captionUp = false;
      drawStill(src, w, h, nullptr);              // same picture, caption gone
    }
    if (bootButtonHit()) {
      Serial.println("btn    : BOOT pressed -> main screen (from the gallery)");
      showScreen(ST_HOME);
      return 1;
    }
    // A slap from another badge ends the slideshow; loop() then shows it.
    bool sosSlapWaiting();
    if (sosSlapWaiting()) { Serial.println("gallery: incoming slap - leaving the slideshow"); return 1; }
    int g = galleryGesture();
    if (g == 2) {
      Serial.println("gallery: hold -> main screen");
      showScreen(ST_HOME);
      return 1;
    }
    if (g == 1) {
      Serial.println("gallery: tap -> next picture");
      return 0;
    }
    delay(8);
  }
  return 0;
}

// Allocate the PSRAM decode buffers once. Whichever of the app or the boot self-test runs first
// pays for them; the other reuses them. The slide/compose window is the canvas's own framebuffer,
// so composing a frame and flushing it is the same memory - no extra 434 kB buffer.
bool galleryBuffers() {
  if (frameA) return true;
  frameA = (uint16_t *)ps_malloc((size_t)MAX_FRAME_PX * 2);
  frameB = (uint16_t *)ps_malloc((size_t)MAX_FRAME_PX * 2);
  slideWin = gfx->getFramebuffer();
  if (!frameA || !frameB || !slideWin) {
    Serial.printf("gallery: PSRAM allocation failed (free %u kB)\n",
                  (unsigned)(ESP.getFreePsram() / 1024));
    if (frameA) free(frameA);
    if (frameB) free(frameB);
    frameA = frameB = slideWin = nullptr;
    return false;
  }
  Serial.printf("gallery: buffers ready, %u kB PSRAM free\n",
                (unsigned)(ESP.getFreePsram() / 1024));
  return true;
}

void showGallery() {
  if (!galleryBuffers()) {
    centred(232, "no memory for pictures", 1, RGB565(255, 120, 120));
    gfx->flush();
    return;
  }

  if (picCount == 0) loadCachedPictures();

  if (picCount == 0) {
    gfx->fillScreen(RGB565(10, 10, 14));
    centred(200, "no pictures cached yet", 2, RGB565(255, 220, 120));
    centred(232, "tap to fetch from GitHub", 1, RGB565(180, 180, 195));
    centred(256, "BOOT button: main screen", 1, RGB565(140, 140, 155));
    gfx->flush();
    Serial.println("gallery: nothing cached - tap to fetch");
    return;
  }

  // The finger that opened the gallery is probably still on the glass: it must not count as the
  // first "next picture" tap.
  galleryIgnoreCurrentPress();

  // Two frame buffers, ping-ponged: `curBuf` is the picture on the glass, `spare` is where the
  // next one is decoded - ahead of time by the hold, or at the change as a fallback.
  uint16_t *curBuf = frameA;
  uint16_t *spare  = frameB;
  pendIdx = -1;

  Serial.printf("gallery: showing %d picture(s), still, %d s each\n", picCount, HOLD_MS / 1000);
  int fails = 0;
  while (!loadPictureInto(curBuf, &curW, &curH)) {
    if (++fails >= picCount) {
      gfx->fillScreen(RGB565(10, 10, 14));
      centred(232, "pictures could not be decoded", 1, RGB565(255, 120, 120));
      centred(256, "BOOT button: main screen", 1, RGB565(140, 140, 155));
      gfx->flush();
      return;
    }
  }
  // The first picture's caption is the how-to, since there is no button to see any more.
  char cap[40];
  snprintf(cap, sizeof(cap), "TAP NEXT  HOLD HOME");
  Serial.printf("gallery: showing %s (%dx%d)\n", picFile[picShown - 1].c_str(), curW, curH);
  drawStill(curBuf, curW, curH, cap);
  if (holdStill(curBuf, curW, curH, spare)) return;     // left the gallery

  // Runs until the cog or BOOT takes us out (the old version stopped after 200 pictures and
  // left a frozen screen behind).
  while (screen == ST_GALLERY) {
    int oldW = curW, oldH = curH;
    if (pendIdx >= 0) {
      curW = pendW; curH = pendH; picShown = pendIdx + 1; pendIdx = -1;
    } else if (!loadPictureInto(spare, &curW, &curH)) {
      // A picture that will not decode is skipped, not retried forever; the old one stays up.
      curW = oldW; curH = oldH;
      if (holdStill(curBuf, curW, curH, spare)) return;
      continue;
    }

    char cap2[40];
    snprintf(cap2, sizeof(cap2), "%d/%d  %s", picShown, picCount, picFile[picShown - 1].c_str());

    for (int f = 1; f <= SLIDE_FRAMES; f++) {
      int d = (PANEL_PX * f) / SLIDE_FRAMES;
      slideFrame(curBuf, oldW, oldH, windowX(oldW), windowY(oldH),
                 spare, curW, curH, windowX(curW), windowY(curH), d);
      int g = galleryGesture();
      if (g == 2) { Serial.println("gallery: hold during slide -> main screen"); showScreen(ST_HOME); return; }
      if (g == 1) break;                          // a tap jumps straight to the new picture
      if (bootButtonHit()) { showScreen(ST_HOME); return; }
    }
    // The new picture is on the glass, so the old buffer becomes the next spare.
    uint16_t *shown = spare;
    spare  = curBuf;
    curBuf = shown;
    drawStill(curBuf, curW, curH, cap2);
    Serial.printf("gallery: showing %s (%dx%d)\n", picFile[picShown - 1].c_str(), curW, curH);
    if (holdStill(curBuf, curW, curH, spare)) return;   // decodes the next one into the spare
  }
}

// The sync screen doubles as the gallery's progress display.
void drawGallerySync() {
  // A quiet sync must not touch the glass at all. Every sync redraw funnels through here, so
  // this one guard covers the takeover and the per-file progress lines alike.
  if (syncQuiet) return;
  gfx->fillScreen(RGB565(10, 12, 16));
  centred(196, "Pictures", 3, RGB565_WHITE);
  const char *msg = syncNote.length() ? syncNote.c_str() : "working...";
  centred(238, msg, 2, RGB565(150, 220, 255));
  if (syncState == 2 && syncTotal > 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d of %d", syncDone, syncTotal);
    centred(266, buf, 1, RGB565(180, 180, 195));
  }
  if (syncState == 4) centred(300, "tap to go back", 1, RGB565(255, 200, 120));
  if (syncState == 3) centred(300, "starting...", 1, RGB565(180, 180, 195));
  gfx->flush();
  Serial.printf("gallery: sync screen (%d) %s\n", syncState, msg);
}

// Boot self-test for the visual path: decode two cached pictures and run eight slide frames onto
// the panel. Decoding and the windowed blit are exactly what a download log says nothing about,
// so this exercises them at every boot rather than waiting for someone to tap. It also costs
// only a few hundred milliseconds, and it reports failure instead of leaving it to be discovered.
void galleryWarmup() {
  if (picCount < 1) { Serial.println("gallery: warm-up skipped, nothing cached"); return; }
  if (!galleryBuffers()) return;

  char p1[48], p2[48];
  snprintf(p1, sizeof(p1), "%s/%s", PICS_DIR, picFile[0].c_str());
  int w1 = picW[0], h1 = picH[0];
  uint32_t t0 = millis();
  // Use the picture's own width as the stride, exactly as the app does - a hard-coded 1100 here
  // meant the self-test checked geometry the gallery never uses.
  if (!decodeToFrame(p1, frameA, w1, h1, &w1, &h1)) {
    Serial.printf("gallery: warm-up decode FAILED on %s\n", p1);
    return;
  }
  Serial.printf("gallery: warm-up decoded %s %dx%d in %lu ms\n",
                picFile[0].c_str(), w1, h1, (unsigned long)(millis() - t0));
  blitWindow(frameA, w1, h1, 0, 0);
  Serial.println("gallery: warm-up blit ok");

  if (picCount < 2) return;
  snprintf(p2, sizeof(p2), "%s/%s", PICS_DIR, picFile[1].c_str());
  int w2 = picW[1], h2 = picH[1];
  if (!decodeToFrame(p2, frameB, w2, h2, &w2, &h2)) {
    Serial.printf("gallery: warm-up second decode FAILED on %s\n", p2);
    return;
  }
  uint32_t t1 = millis();
  for (int i = 1; i <= 8; i++)
    slideFrame(frameB, w2, h2, 0, 0, frameA, w1, h1, 0, 0, PANEL_PX * i / 8);
  uint32_t dt = millis() - t1;
  Serial.printf("gallery: warm-up ran 8 slide frames in %lu ms (%lu ms each)\n",
                (unsigned long)dt, (unsigned long)(dt / 8));
}

// ---------------------------------------------------------------- wallpaper cache

// The home wallpaper is the most expensive thing this badge draws. As 900 per-block JPEG draws
// it measured ~390 ms, which is most of the ~450 ms it took to return to the home screen from
// anywhere - and it is redrawn on every return. Decoding it ONCE into PSRAM and then blitting a
// single full frame measured 56 ms, so the cache pays for itself on the second draw.
//
// Same mechanism the gallery uses: point decDest at a buffer so jpegBlock() in Badge.ino packs
// the blocks into PSRAM instead of straight at the glass.
static uint16_t *wallBuf = nullptr;
static int       wallTries = 0;

// Draw the home wallpaper into the canvas. The first call decodes it once into PSRAM (~390 ms,
// measured); every call after that is a memcpy into the canvas framebuffer. If that decode fails it falls
// back to the 900 per-block JPEG draw, which is what the old code did on every draw.
//
// Deliberately returns void: Arduino's auto-prototype generator assumes void for any function it
// sees called before its definition, and a bool here produced "ambiguating new declaration".
void wallpaperBlit() {
  if (!wallBuf && wallTries < 2) {
    wallTries++;
    wallBuf = (uint16_t *)ps_malloc((size_t)PANEL_PX * PANEL_PX * 2);
    if (!wallBuf) {
      Serial.println("bg     : no PSRAM for the wallpaper cache");
    } else {
      File f = LittleFS.open(WALLPAPER_FILE, FILE_READ);
      if (!f) {
        Serial.printf("! %s missing - is the filesystem image flashed?\n", WALLPAPER_FILE);
        free(wallBuf); wallBuf = nullptr;
      } else {
        f.seek(0);
        uint32_t t0 = millis();
        decDest = wallBuf;
        decDestW = PANEL_PX;
        JRESULT r = TJpgDec.drawFsJpg(0, 0, f);
        decDest = nullptr;
        f.close();
        if (r != JDR_OK) {
          Serial.printf("bg     : wallpaper decode FAILED (%d)\n", (int)r);
          free(wallBuf); wallBuf = nullptr;
        } else {
          Serial.printf("bg     : wallpaper cached in PSRAM, decode %lu ms (%u kB PSRAM free)\n",
                        (unsigned long)(millis() - t0), (unsigned)(ESP.getFreePsram() / 1024));
        }
      }
    }
  }

  if (wallBuf && gfx->getFramebuffer()) {
    // Into the canvas, not at the glass: the cog and the pill go on top, then one flush.
    uint32_t t1 = millis();
    memcpy(gfx->getFramebuffer(), wallBuf, (size_t)PANEL_PX * PANEL_PX * 2);
    Serial.printf("bg     : wallpaper copied to canvas in %lu ms\n", (unsigned long)(millis() - t1));
    return;
  }

  // Fallback: the original path, straight to the panel in 900 blocks.
  File f = LittleFS.open(WALLPAPER_FILE, FILE_READ);
  if (f) {
    f.seek(0);
    blocks = 0;
    uint32_t t0 = millis();
    JRESULT r = TJpgDec.drawFsJpg(0, 0, f);
    f.close();
    Serial.printf("bg     : %s result %d (%s), %lu block(s), %lu ms\n",
                  WALLPAPER_FILE, (int)r, r == JDR_OK ? "ok" : "FAILED", blocks,
                  (unsigned long)(millis() - t0));
  } else {
    Serial.printf("! %s missing - is the filesystem image flashed?\n", WALLPAPER_FILE);
  }
}

// Restore rows y0..y1 of the wallpaper into the canvas - the home carousel's slide redraws only
// the band it covers, not the whole screen. Falls back to a full wallpaper draw if the PSRAM copy
// is not there.
void wallpaperRows(int y0, int y1) {
  if (y0 < 0) y0 = 0;
  if (y1 > PANEL_PX - 1) y1 = PANEL_PX - 1;
  if (!wallBuf || !gfx->getFramebuffer() || y1 < y0) { wallpaperBlit(); return; }
  memcpy(gfx->getFramebuffer() + (size_t)y0 * PANEL_PX, wallBuf + (size_t)y0 * PANEL_PX,
         (size_t)(y1 - y0 + 1) * PANEL_PX * 2);
}

// Called at boot once the badge is online. Fetches the list and any changed pictures, so the
// gallery has content before it is opened - and so the download path runs without anyone
// tapping, which is what makes it testable.
void galleryBootFetch() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("gallery: boot fetch skipped, not online");
    return;
  }
  // Quiet when pictures are already cached: the user is looking at the home screen and this check
  // usually finds nothing to do, so it must not take the display over. On a first boot (no /pics
  // directory yet) there is nothing to show, so the progress screen is worth having.
  bool quiet = LittleFS.exists(PICS_DIR);
  syncQuiet = quiet;
  Serial.printf("gallery: boot fetch starting%s\n", quiet ? " (quiet)" : "");
  syncPictures();
  syncQuiet = false;
  Serial.printf("gallery: boot fetch finished, %d picture(s) known\n", picCount);
  // Log the order the slideshow will follow. The deck used to be shuffled while picCount was
  // still 0, which is how a "random" slideshow ended up replaying one picture forever. Printing
  // the order makes that class of bug visible at boot instead of after six taps.
  if (picCount > 0) {
    if (deckFor != picCount) shuffleDeck();
    Serial.print("gallery: deck order:");
    for (int i = 0; i < picCount; i++) Serial.printf(" %s", picFile[picOrder[i]].c_str());
    Serial.println();
  }
  // The warm-up blits real frames to the panel - that is the point of it - but on a routine boot
  // it is a 1.4-second flash of pictures over a home screen that is meant to be sitting still.
  // Keep it for a first boot, where the panel is busy anyway, and skip it when quiet.
  if (quiet) Serial.println("gallery: warm-up skipped (quiet boot)");
  else galleryWarmup();
}

// Tap handling while the sync screen is up.
void gallerySyncTap() {
  if (syncState == 4 || syncState == 3) showScreen(ST_GALLERY);
}
