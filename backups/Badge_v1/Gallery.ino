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
 *   3. Pictures are shown in a shuffled random order. Each one SCROLLS slowly across the
 *      round window (the panel is 466x466; a landscape picture is 835 wide, so scrolling is
 *      the only way to see all of it), then SLIDES out while the next slides in.
 *
 * Measured budget (BlitTest, this board): a full 466x466 frame blit from PSRAM is 56 ms
 * (~17.6 fps); a slide frame including the row copies is 76 ms (~13 fps), so a 20-frame
 * transition is ~1.5 s. Anything that draws a frame must poll the touch between frames or the
 * panel feels dead for the duration.
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
#define HOLD_MS        13000      // scroll across the picture for this long
#define SLIDE_FRAMES   20         // 20 x 76 ms ~= 1.5 s
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
static uint16_t *slideWin = nullptr;
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
bool decodeToFrame(const char *path, uint16_t *dest, int destW, int expectH, int *outW, int *outH) {
  File f = LittleFS.open(path, FILE_READ);
  if (!f) { Serial.printf("gallery: cannot open %s\n", path); return false; }
  f.seek(0);
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

// Blit the 466x466 window of `frame` (frameW x frameH) whose top-left is (ox, oy) to the panel.
// For a portrait picture the rows are contiguous, so it is one call; a landscape picture needs
// a per-row copy because draw16bitRGBBitmap has no stride parameter.
void blitWindow(uint16_t *frame, int frameW, int frameH, int ox, int oy) {
  if (ox < 0) ox = 0;
  if (oy < 0) oy = 0;
  if (ox > frameW - PANEL_PX) ox = frameW - PANEL_PX;
  if (oy > frameH - PANEL_PX) oy = frameH - PANEL_PX;
  if (frameW == PANEL_PX && oy == 0) {
    gfx->draw16bitRGBBitmap(0, 0, frame, PANEL_PX, PANEL_PX);   // already exactly a window
    return;
  }
  if (frameW == PANEL_PX) {
    gfx->draw16bitRGBBitmap(0, 0, frame + (size_t)oy * PANEL_PX, PANEL_PX, PANEL_PX);
    return;
  }
  for (int y = 0; y < PANEL_PX; y++) {
    memcpy(slideWin + (size_t)y * PANEL_PX,
           frame + (size_t)(oy + y) * frameW + ox,
           PANEL_PX * 2);
  }
  gfx->draw16bitRGBBitmap(0, 0, slideWin, PANEL_PX, PANEL_PX);
}

// One frame of the slide: `oldF` pushed out to the left by d, `newF` coming in from the right.
// Polls the touch between frames (see the note at the top) so a tap still lands.
void slideFrame(uint16_t *oldF, int oldW, int oldH, int oldOX, int oldOY,
                uint16_t *newF, int newW, int newH, int newOX, int newOY, int d) {
  int keep = PANEL_PX - d;                       // how much of the old picture is still visible
  if (keep < 0) keep = 0;
  for (int y = 0; y < PANEL_PX; y++) {
    uint16_t *dst = slideWin + (size_t)y * PANEL_PX;
    int oy = oldOY + y; if (oy > oldH - 1) oy = oldH - 1;
    int ny = newOY + y; if (ny > newH - 1) ny = newH - 1;
    int oLeft = oldW - 1 - (oldOX + d);          // pixels available from the old source
    int takeOld = keep; if (takeOld > oLeft) takeOld = oLeft; if (takeOld < 0) takeOld = 0;
    if (takeOld > 0) {
      memcpy(dst, oldF + (size_t)oy * oldW + oldOX + d, (size_t)takeOld * 2);
    }
    int takeNew = PANEL_PX - takeOld;
    if (takeNew > 0) {
      int avail = newW - 1 - newOX;
      if (takeNew > avail) takeNew = avail;
      if (takeNew > 0) {
        memcpy(dst + takeOld, newF + (size_t)ny * newW + newOX, (size_t)takeNew * 2);
      }
    }
  }
  gfx->draw16bitRGBBitmap(0, 0, slideWin, PANEL_PX, PANEL_PX);
}

// ---------------------------------------------------------------- chrome

// A cog in the same place as the home screen's, so "that button top right" means the same
// thing everywhere. Counts as inside the circle by the same arithmetic the boot check uses.
void drawGalleryCog() {
  gfx->fillCircle(COG_CX, COG_CY, COG_BTN_R, RGB565(12, 12, 18));
  gfx->drawCircle(COG_CX, COG_CY, COG_BTN_R, RGB565(120, 200, 255));
  drawCog(COG_CX, COG_CY, COG_R, RGB565(150, 230, 255), RGB565(12, 12, 18));
}

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
      // Dimensions come from the list when we have it; otherwise assume the panel's height and
      // let the decoder decide. A wrong guess only costs a re-clamped scroll.
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

// Scroll the picture being held, panning it if it is bigger than the panel. Each step is one
// blit, so the touch and the BOOT button are polled between steps. The next picture is decoded
// into `spare` early in the hold, while this one has barely started moving.
void holdAndScroll(uint16_t *src, int w, int h, uint16_t *spare) {
  if (w <= PANEL_PX && h <= PANEL_PX) { delay(200); return; }
  int spanX = w - PANEL_PX;
  int spanY = h - PANEL_PX;
  uint32_t start = millis();
  bool decodedAhead = false;
  while (millis() - start < HOLD_MS) {
    uint32_t el = millis() - start;
    float t = (float)el / (float)HOLD_MS;
    int ox = spanX > 0 ? (int)(spanX * t) : 0;
    int oy = spanY > 0 ? (int)(spanY * t) : 0;
    blitWindow(src, w, h, ox, oy);
    // Decode the next one once, early: the pan has barely started, so a pause here hides the
    // decode rather than stalling the change between pictures.
    if (!decodedAhead) { decodedAhead = true; predecodeNext(spare); }
    // A hardware press has to work during the 13-second scroll as well, not only between
    // pictures. The gallery is the one place that blocks for a long time, so it checks here.
    if (bootButtonHit()) {
      Serial.println("btn    : BOOT pressed -> main screen (from the gallery)");
      showScreen(ST_HOME);
      return;
    }
    // Poll here, not only between pictures: a 13-second scroll must not swallow taps.
    int16_t xs[1], ys[1];
    if (touchOK && touch.getPoint(xs, ys, 1) > 0) {
      int dx = xs[0] - COG_CX, dy = ys[0] - COG_CY;
      if (dx * dx + dy * dy <= (COG_BTN_R + 32) * (COG_BTN_R + 32)) {
        Serial.println("gallery: cog pressed, back to settings");
        showScreen(ST_MENU);
        return;
      }
      Serial.printf("gallery: tap at %d,%d -> next picture\n", xs[0], ys[0]);
      return;                                  // a tap ends the hold early
    }
    delay(40);                                  // ~13 fps of drawing, with room for taps
  }
}

// Allocate the PSRAM decode buffers once. Whichever of the app or the boot self-test runs first
// pays for them; the other reuses them.
bool galleryBuffers() {
  if (frameA) return true;
  frameA = (uint16_t *)ps_malloc((size_t)1100 * PANEL_PX * 2);
  frameB = (uint16_t *)ps_malloc((size_t)1100 * PANEL_PX * 2);
  slideWin = (uint16_t *)ps_malloc((size_t)PANEL_PX * PANEL_PX * 2);
  if (!frameA || !frameB || !slideWin) {
    Serial.printf("gallery: PSRAM allocation failed (free %u kB)\n",
                  (unsigned)(ESP.getFreePsram() / 1024));
    frameA = frameB = slideWin = nullptr;
    return false;
  }
  Serial.printf("gallery: buffers ready, %u kB PSRAM free\n",
                (unsigned)(ESP.getFreePsram() / 1024));
  return true;
}

void showGallery() {
  if (!galleryBuffers()) { centred(232, "no memory for pictures", 1, RGB565(255, 120, 120)); return; }

  if (picCount == 0) loadCachedPictures();

  if (picCount == 0) {
    gfx->fillScreen(RGB565(10, 10, 14));
    centred(200, "no pictures cached yet", 2, RGB565(255, 220, 120));
    centred(232, "tap to fetch from GitHub", 1, RGB565(180, 180, 195));
    drawGalleryCog();
    Serial.println("gallery: nothing cached - tap to fetch");
    return;
  }

  // Two frame buffers, ping-ponged: `curBuf` is the picture on the glass, `spare` is where the
  // next one is decoded - either ahead of time by the hold, or at the transition as a fallback.
  uint16_t *curBuf = frameA;
  uint16_t *spare  = frameB;

  Serial.printf("gallery: scrolling %d picture(s)\n", picCount);
  // First picture.
  if (!loadPictureInto(curBuf, &curW, &curH)) {
    centred(232, "decode failed", 1, RGB565(255, 120, 120));
    return;
  }
  char cap[40];
  snprintf(cap, sizeof(cap), "%d/%d  %s", picShown, picCount, picFile[picShown - 1].c_str());
  Serial.printf("gallery: showing %s (%dx%d)\n", picFile[picShown - 1].c_str(), curW, curH);

  blitWindow(curBuf, curW, curH, 0, 0);
  drawGalleryCog();
  drawGalleryPill(cap);
  // The hold decodes the next picture into the spare buffer while this one is being panned.
  holdAndScroll(curBuf, curW, curH, spare);
  if (screen != ST_GALLERY) return;             // the cog was pressed during the scroll

  // Then keep sliding to the next one.
  for (int guard = 0; guard < 200; guard++) {
    if (screen != ST_GALLERY) return;
    int oldW = curW, oldH = curH;
    // The hold usually decoded it already, so the transition starts with no pause at all.
    if (pendIdx >= 0) {
      curW = pendW; curH = pendH; picShown = pendIdx + 1; pendIdx = -1;
    } else if (!loadPictureInto(spare, &curW, &curH)) {
      delay(1500); continue;                    // decode it here if the hold could not
    }

    // Where the incoming picture starts and where the outgoing one ended (its scroll end).
    int oldOX = (oldW > PANEL_PX) ? (oldW - PANEL_PX) : 0;
    int oldOY = (oldH > PANEL_PX) ? (oldH - PANEL_PX) : 0;
    char cap2[40];
    snprintf(cap2, sizeof(cap2), "%d/%d  %s", picShown, picCount, picFile[picShown - 1].c_str());

    for (int f = 0; f <= SLIDE_FRAMES; f++) {
      int d = (PANEL_PX * f) / SLIDE_FRAMES;
      slideFrame(curBuf, oldW, oldH, oldOX, oldOY, spare, curW, curH, 0, 0, d);
      // A tap during the transition jumps straight to the new picture.
      int16_t xs[1], ys[1];
      if (touchOK && touch.getPoint(xs, ys, 1) > 0) {
        int dx = xs[0] - COG_CX, dy = ys[0] - COG_CY;
        if (dx * dx + dy * dy <= (COG_BTN_R + 32) * (COG_BTN_R + 32)) {
          Serial.println("gallery: cog pressed during slide, back to settings");
          showScreen(ST_MENU);
          return;
        }
        break;
      }
    }
    // The new picture is on the glass, so the old buffer becomes the next spare.
    uint16_t *shown = spare;
    spare  = curBuf;
    curBuf = shown;
    blitWindow(curBuf, curW, curH, 0, 0);
    drawGalleryCog();
    drawGalleryPill(cap2);
    Serial.printf("gallery: showing %s (%dx%d)\n", picFile[picShown - 1].c_str(), curW, curH);
    holdAndScroll(curBuf, curW, curH, spare);   // decodes the next one into the spare
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

// Draw the home wallpaper. The first call decodes it once into PSRAM (~390 ms, measured); every
// call after that is a single full-frame blit (~56 ms measured). If that decode fails it falls
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

  if (wallBuf) {
    uint32_t t1 = millis();
    gfx->draw16bitRGBBitmap(0, 0, wallBuf, PANEL_PX, PANEL_PX);
    Serial.printf("bg     : wallpaper blit %lu ms\n", (unsigned long)(millis() - t1));
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
