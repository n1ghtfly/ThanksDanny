/*
 * Radar (v3.12) - live aircraft around you, sonar style.
 *
 * Inspired by and modelled on MatixYo/ESP32-Plane-Radar (MIT licence, (c) 2026 MatixYo):
 * the same idea, data source and look, rebuilt for this badge - 466x466 instead of 240x240,
 * Arduino_GFX instead of LovyanGFX, and no JSON library (src/adsbparse.c).
 *
 * Data: adsb.fi open data, GET https://opendata.adsb.fi/api/v3/lat/<lat>/lon/<lon>/dist/<nm>
 * every RADAR_FETCH_MS while the app is open (their public limit is 1 request per second per IP).
 * Fetching runs in a task on core 0, so the screen keeps moving; between fetches every aircraft is
 * dead-reckoned along its track at its ground speed, so it glides instead of jumping.
 *
 * Screen: dark blue, four green range rings, N/E/S/W, range label on the east spoke. Aircraft
 * inside the outer ring: red triangle pointing along the track, magenta speed vector (one minute
 * of travel x1.5), callsign + altitude/type. Aircraft further out (still fetched): a red dot on
 * the rim at the right bearing. Tap a plane = details (route, airline, aircraft - v3.15); tap anywhere
 * else = next range (5 / 10 / 15 / 25 km on ring 3), saved.
 * HOME at the bottom or BOOT = home screen.
 *
 * Location: entered once on the phone setup page (Settings -> Setup on phone), stored only on the
 * badge (NVS "radar") - never in the code, the repository is public. "52.37, 4.90" as copied from
 * a map app works. A default can also come from sos_local.h (RADAR_DEFAULT_LAT / _LON).
 *
 * Sweep (v3.21): a green beam turns clockwise once every RADAR_SWEEP_MS with a fading trail behind
 * it, like a real radar scope. Each aircraft lights up bright when the beam passes over it and
 * dims until the next turn (labels stay readable). The frame rate went from ~3 to ~25 per second;
 * the beam angle comes from millis(), so it turns at the same speed even if a frame is slow.
 *
 * Burn-in: the radar is meant to be left running, so the whole picture shifts by a few pixels
 * every minute (the rings would otherwise stand still on the AMOLED for hours).
 */

#include "src/adsbparse.h"

#define RADAR_FETCH_MS   5000
#define RADAR_FRAME_MS   40                 // redraw cadence: ~25 fps for the sweep (v3.21: was 300)
#define RADAR_SWEEP_MS   4000               // one beam revolution (airport radars turn in 4-5 s)
#define RADAR_TRAIL_DEG  70                 // length of the fading trail behind the beam
#define RADAR_TRAIL_SEG  28                 // trail drawn as this many wedges (2.5 deg each)
#define RADAR_MAX        96                 // aircraft kept per fetch (busy airspace near Schiphol)
#define RADAR_R          200                // outer ring radius, px
#define RADAR_KM_DEG     111.32f

static const float RADAR_RING3_KM[4] = { 5, 10, 15, 25 };   // label on ring 3 (of 4)

#define RADAR_BG     RGB565(4, 10, 28)
#define RADAR_GRID   RGB565(16, 100, 32)
#define RADAR_PLANE  RGB565(255, 40, 40)
#define RADAR_TRACK  RGB565(255, 0, 255)
#define RADAR_TAG    RGB565(235, 235, 235)
#define RADAR_TAG2   RGB565(255, 200, 0)

static Preferences radarPrefs;
static double   radarLat = 0, radarLon = 0;
static bool     radarHaveLoc = false;
static int      radarRange = 1;            // index into RADAR_RING3_KM

static AdsbPlane *radarPlanes = nullptr;   // RADAR_MAX, in PSRAM; shared with the fetch task, under radarLock
static AdsbPlane *radarTmp = nullptr;      // the fetch task's parse buffer
static AdsbPlane *radarSnap = nullptr;     // the drawing copy

// v3.15: tap a plane for details. Where each plane was drawn last frame, to find the tapped one.
static int16_t  radarHitX[RADAR_MAX], radarHitY[RADAR_MAX], radarHitTx[RADAR_MAX], radarHitTw[RADAR_MAX];   // plane + its label
static char     radarHitHex[RADAR_MAX][8];
static int      radarHitN = 0;
static bool     radarDetail = false;       // showing the detail screen
static char     radarSelHex[8] = "";
static AdsbPlane radarSel;                 // last known state of the selected plane
static uint32_t radarSelSeen = 0, radarSelAt = 0;

// Route / airline / aircraft from adsbdb.com, looked up once per selected plane (in the fetch task).
struct RadarInfo {
  char hex[8], call[9];
  char airline[32], org[5], orgCity[24], dst[5], dstCity[24], maker[24], model[24], owner[28], reg[10];
};
static RadarInfo radarInfo;
static volatile uint8_t radarInfoState = 0;   // 0 idle, 1 wanted, 2 looking up, 3 done
static int      radarCount = 0;
static uint32_t radarFetchedAt = 0;        // millis() of the last good fetch
static char     radarStatus[40] = "";
static SemaphoreHandle_t radarLock = nullptr;
static volatile bool radarRun = false;     // the fetch task runs while this is true
static volatile bool radarTaskAlive = false;
static uint32_t radarLastFrame = 0;

static float radarOuterKm() { return RADAR_RING3_KM[radarRange] * 4.0f / 3.0f; }

// ---------------------------------------------------------------- settings

static bool radarParseLoc(String s, double *lat, double *lon) {
  // Accepts "52.3676, 4.9041", "52.3676 4.9041", and with decimal commas "52,3676; 4,9041",
  // "52,3676 4,9041" or "52,3676,4,9041" (as some phones copy it).
  s.trim();
  if (s.indexOf('.') >= 0) {                       // decimal points: commas/semicolons separate
    s.replace(',', ' '); s.replace(';', ' ');
  } else if (s.indexOf(';') >= 0 || s.indexOf(' ') >= 0) {
    s.replace(';', ' '); s.replace(',', '.');        // decimal commas, separated by ; or space
  } else {                                           // "52,3676,4,9041": the 2nd comma separates
    int c1 = s.indexOf(','), c2 = c1 >= 0 ? s.indexOf(',', c1 + 1) : -1;
    if (c2 < 0) return false;
    s.setCharAt(c2, ' ');
    s.replace(',', '.');
  }
  double la, lo;
  if (sscanf(s.c_str(), "%lf %lf", &la, &lo) != 2) return false;
  if (la < -90 || la > 90 || lo < -180 || lo > 180 || (la == 0 && lo == 0)) return false;
  *lat = la; *lon = lo;
  return true;
}

void radarLoadConfig() {
  if (!radarLock) radarLock = xSemaphoreCreateMutex();
  if (!radarPlanes) {
    radarPlanes = (AdsbPlane *)ps_malloc(sizeof(AdsbPlane) * RADAR_MAX);
    radarTmp    = (AdsbPlane *)ps_malloc(sizeof(AdsbPlane) * RADAR_MAX);
    radarSnap   = (AdsbPlane *)ps_malloc(sizeof(AdsbPlane) * RADAR_MAX);
  }
  radarPrefs.begin("radar", true);
  radarHaveLoc = radarPrefs.getBool("have", false);
  radarLat = radarPrefs.getDouble("lat", 0);
  radarLon = radarPrefs.getDouble("lon", 0);
  radarRange = radarPrefs.getUChar("range", 1);
  radarPrefs.end();
  if (radarRange < 0 || radarRange > 3) radarRange = 1;
#if defined(RADAR_DEFAULT_LAT) && defined(RADAR_DEFAULT_LON)
  if (!radarHaveLoc) { radarLat = RADAR_DEFAULT_LAT; radarLon = RADAR_DEFAULT_LON; radarHaveLoc = true; }
#endif
  Serial.printf("radar  : location %s, range %.0f km\n", radarHaveLoc ? "set" : "NOT set",
                RADAR_RING3_KM[radarRange]);            // the coordinates themselves are not logged
}

static void radarSaveConfig() {
  radarPrefs.begin("radar", false);
  radarPrefs.putBool("have", radarHaveLoc);
  radarPrefs.putDouble("lat", radarLat);
  radarPrefs.putDouble("lon", radarLon);
  radarPrefs.putUChar("range", radarRange);
  radarPrefs.end();
}

// Phone setup page section (Badge.ino buildPage) and its POST /radar handler.
String radarSetupForm() {
  const char *in = "style='width:100%;padding:14px;font-size:16px;border-radius:8px;box-sizing:border-box'";
  String f = F("<hr style='border:0;border-top:1px solid #334;margin:28px 0'>"
               "<h3 style='margin:0 0 6px'>Plane radar</h3>"
               "<p style='color:#9ab'>The centre of the radar: latitude and longitude, e.g. copied from a map "
               "app (long-press a spot). Stored on the badge only.</p><form method='POST' action='/radar'>");
  f += "<p><input name='loc' placeholder='52.3676, 4.9041' autocomplete='off' inputmode='decimal' " + String(in);
  if (radarHaveLoc) f += " value='" + String(radarLat, 4) + ", " + String(radarLon, 4) + "'";
  f += "></p><p><button style='padding:14px 22px;font-size:16px;border-radius:8px'>Save radar location</button></p></form>";
  return f;
}

void radarHandleSave() {
  double la, lo;
  bool ok = radarParseLoc(server.arg("loc"), &la, &lo);
  if (ok) { radarLat = la; radarLon = lo; radarHaveLoc = true; radarSaveConfig(); }
  Serial.printf("radar  : location %s\n", ok ? "saved" : "NOT understood");
  server.send(200, "text/html", String(F("<body style='font-family:system-ui;background:#0e0e12;color:#eee;padding:26px'>")) +
              (ok ? F("<h2>Saved</h2><p>Open RADAR on the badge.</p>")
                  : F("<h2>Not understood</h2><p>Type it like <b>52.3676, 4.9041</b> and try again.</p>")) +
              F("<p><a style='color:#9cf' href='/'>Back</a></p></body>"));
}

// ---------------------------------------------------------------- fetching (core 0)

struct RadarReader { NetworkClient *s; HTTPClient *h; uint32_t deadline; uint8_t buf[512]; int pos, len; };

static int radarRead(void *ctx) {
  RadarReader *r = (RadarReader *)ctx;
  if (r->pos < r->len) return r->buf[r->pos++];
  while (millis() < r->deadline) {
    int av = r->s->available();
    if (av > 0) {
      r->len = r->s->read(r->buf, av > (int)sizeof(r->buf) ? (int)sizeof(r->buf) : av);
      r->pos = 0;
      if (r->len > 0) return r->buf[r->pos++];
    } else if (!r->s->connected()) {
      return -1;                                     // body finished (HTTP/1.0: the server closes)
    }
    vTaskDelay(1);
  }
  return -1;
}

static bool radarFetchOnce(AdsbPlane *tmp, int *n) {
  char url[128];
  float nm = radarOuterKm() * 1.3f / 1.852f;          // a bit past the ring, for the rim dots
  snprintf(url, sizeof(url), "https://opendata.adsb.fi/api/v3/lat/%.4f/lon/%.4f/dist/%.0f",
           radarLat, radarLon, nm < 1 ? 1 : nm);
  for (int attempt = 0; attempt < 2; attempt++) {
    NetworkClientSecure client;
    if (attempt == 0) client.useBuiltinCACertBundle();
    else              client.setInsecure();            // a CA bundle problem must not kill the app
    HTTPClient http;
    http.useHTTP10(true);                             // no chunked encoding: the body is plain JSON
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    if (!http.begin(client, url)) continue;
    int code = http.GET();
    if (code != 200) {
      http.end();
      snprintf(radarStatus, sizeof(radarStatus), "adsb.fi HTTP %d", code);
      if (code > 0) return false;                     // a real answer: no point retrying insecure
      continue;
    }
    RadarReader rd = { http.getStreamPtr(), &http, millis() + 8000, {0}, 0, 0 };
    int got = adsb_parse(radarRead, &rd, tmp, RADAR_MAX);
    http.end();
    if (got < 0) { snprintf(radarStatus, sizeof(radarStatus), "bad answer"); return false; }
    *n = got;
    return true;
  }
  if (!radarStatus[0]) snprintf(radarStatus, sizeof(radarStatus), "no connection");
  return false;
}

// One small HTTPS GET into a String (adsbdb answers are ~1 kB).
static int radarGet(const char *url, String &out) {
  int code = -1;
  for (int attempt = 0; attempt < 2; attempt++) {
    NetworkClientSecure client;
    if (attempt == 0) client.useBuiltinCACertBundle();
    else              client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setConnectTimeout(6000);
    http.setTimeout(6000);
    if (!http.begin(client, url)) continue;
    code = http.GET();
    if (code == 200) out = http.getString();
    http.end();
    if (code > 0) break;                              // a real answer: no insecure retry
  }
  return code;
}

// Route + aircraft for the selected plane, from adsbdb.com (free, no key).
static void radarLookup() {
  radarInfoState = 2;
  RadarInfo r;
  memset(&r, 0, sizeof(r));
  memcpy(r.hex, radarInfo.hex, sizeof(r.hex));
  memcpy(r.call, radarInfo.call, sizeof(r.call));
  char url[96];
  String body;
  bool isCall = r.call[0] && strcmp(r.call, r.hex);    // no callsign -> the hex stands in for it
  if (isCall) {
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", r.call);
    int code = radarGet(url, body);
    if (code == 200) {
      const char *b = body.c_str();
      json_flat_field(b, "airline", "name", r.airline, sizeof(r.airline));
      json_flat_field(b, "origin", "iata_code", r.org, sizeof(r.org));
      json_flat_field(b, "origin", "municipality", r.orgCity, sizeof(r.orgCity));
      json_flat_field(b, "destination", "iata_code", r.dst, sizeof(r.dst));
      json_flat_field(b, "destination", "municipality", r.dstCity, sizeof(r.dstCity));
    }
    Serial.printf("radar  : route %s -> HTTP %d %s-%s\n", r.call, code, r.org, r.dst);
  }
  if (r.hex[0]) {
    snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", r.hex);
    body = "";
    int code = radarGet(url, body);
    if (code == 200) {
      const char *b = body.c_str();
      json_flat_field(b, "aircraft", "manufacturer", r.maker, sizeof(r.maker));
      json_flat_field(b, "aircraft", "type", r.model, sizeof(r.model));
      json_flat_field(b, "aircraft", "registered_owner", r.owner, sizeof(r.owner));
      json_flat_field(b, "aircraft", "registration", r.reg, sizeof(r.reg));
    }
    Serial.printf("radar  : aircraft %s -> HTTP %d %s %s\n", r.hex, code, r.maker, r.model);
  }
  xSemaphoreTake(radarLock, portMAX_DELAY);
  if (!strcmp(radarInfo.hex, r.hex)) { radarInfo = r; radarInfoState = 3; }   // still the same plane?
  xSemaphoreGive(radarLock);
}

static void radarTask(void *) {
  AdsbPlane *tmp = radarTmp;
  uint32_t last = 0;
  while (radarRun) {
    if (!tmp) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if (WiFi.status() != WL_CONNECTED) { snprintf(radarStatus, sizeof(radarStatus), "no Wi-Fi"); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if (radarInfoState == 1) radarLookup();          // a plane was tapped: details first
    if (last && millis() - last < RADAR_FETCH_MS) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    last = millis();
    int n = 0;
    uint32_t t0 = millis();
    if (radarFetchOnce(tmp, &n)) {
      int keep = 0;
      for (int i = 0; i < n; i++) if (!tmp[i].ground) tmp[keep++] = tmp[i];   // airborne only
      xSemaphoreTake(radarLock, portMAX_DELAY);
      memcpy(radarPlanes, tmp, sizeof(AdsbPlane) * keep);
      radarCount = keep;
      radarFetchedAt = millis();
      radarStatus[0] = 0;
      xSemaphoreGive(radarLock);
      Serial.printf("radar  : %d aircraft (%d on the ground left out), %lu ms\n", keep, n - keep,
                    (unsigned long)(millis() - t0));
    } else {
      Serial.printf("radar  : fetch failed - %s\n", radarStatus);
    }
  }
  radarTaskAlive = false;
  vTaskDelete(nullptr);
}

// Called on every frame: (re)starts the fetch task when needed. Reopening the app while the old
// task is still finishing a request simply keeps that task going - nothing waits.
static void radarStart() {
  if (!radarHaveLoc) return;
  radarRun = true;
  if (radarTaskAlive) return;
  radarTaskAlive = true;                             // set before the task exists: no double start
  if (xTaskCreatePinnedToCore(radarTask, "radar", 12288, nullptr, 1, nullptr, 0) != pdPASS)
    radarTaskAlive = false;
}

void radarStop() { radarRun = false; radarDetail = false; }   // the task ends after its current request

// ---------------------------------------------------------------- drawing

static int radarOx = 0, radarOy = 0;                 // burn-in shift, changes every minute

static void radarText(int x, int y, const char *s, uint8_t size, uint16_t c) {
  gfx->setTextSize(size); gfx->setTextColor(c); gfx->setCursor(x, y); gfx->print(s);
}

// Where an aircraft is now: last fix + (time since the fix) x ground speed along the track.
static void radarPos(const AdsbPlane &a, uint32_t fetchedAt, float *dxKm, float *dyKm) {
  float coslat = cosf(radarLat * PI / 180.0f);
  float age = (millis() - fetchedAt) / 1000.0f + a.pos_age;
  float lat = a.lat, lon = a.lon;
  if (a.has_track && a.gs > 0 && age < 90) {
    float km = a.gs * 1.852f / 3600.0f * age, t = a.track * PI / 180.0f;
    lat += km * cosf(t) / RADAR_KM_DEG;
    lon += km * sinf(t) / (RADAR_KM_DEG * coslat);
  }
  *dxKm = (lon - radarLon) * RADAR_KM_DEG * coslat;
  *dyKm = (lat - radarLat) * RADAR_KM_DEG;
}

// Beam angle now, radians clockwise from north.
static float radarBeam() { return (millis() % RADAR_SWEEP_MS) * (2.0f * PI / RADAR_SWEEP_MS); }

// How long since the beam passed bearing b (radians): 0 = just now, 1 = a full turn ago.
static float radarSince(float b) {
  float d = radarBeam() - b;
  while (d < 0) d += 2.0f * PI;
  while (d >= 2.0f * PI) d -= 2.0f * PI;
  return d / (2.0f * PI);
}

// Mix the plane colour from bright (just swept) to dim (a turn ago).
static uint16_t radarEcho(float since, int r, int g, int b) {
  float f = 1.0f - 0.65f * since;                    // 100% -> 35%
  return RGB565((int)(r * f), (int)(g * f), (int)(b * f));
}

// The beam: a fan of thin wedges fading from green into the background, then the bright edge.
static void radarSweep(int cx, int cy) {
  float a = radarBeam(), seg = RADAR_TRAIL_DEG * PI / 180.0f / RADAR_TRAIL_SEG;
  int R = RADAR_R - 2;
  for (int i = RADAR_TRAIL_SEG - 1; i >= 0; i--) {   // oldest first, so the brightest wedge is on top
    float a0 = a - (i + 1) * seg, a1 = a - i * seg + 0.01f;
    float k = 1.0f - (float)i / RADAR_TRAIL_SEG;     // 1 at the beam, ->0 at the tail
    k = k * k;
    int R8 = 4 + (int)(26 * k), G8 = 10 + (int)(150 * k), B8 = 28 + (int)(22 * k);   // bg (4,10,28) -> green
    gfx->fillTriangle(cx, cy, cx + (int)(sinf(a0) * R), cy - (int)(cosf(a0) * R),
                      cx + (int)(sinf(a1) * R), cy - (int)(cosf(a1) * R), RGB565(R8, G8, B8));
  }
  int ex = cx + (int)(sinf(a) * R), ey = cy - (int)(cosf(a) * R);
  gfx->drawLine(cx, cy, ex, ey, RGB565(150, 255, 170));
  gfx->drawLine(cx + 1, cy, ex + 1, ey, RGB565(150, 255, 170));
  gfx->drawLine(cx, cy + 1, ex, ey + 1, RGB565(90, 230, 120));
}

static void radarGrid(int cx, int cy) {
  for (int k = 1; k <= 4; k++) {
    gfx->drawCircle(cx, cy, RADAR_R * k / 4, RADAR_GRID);
    gfx->drawCircle(cx, cy, RADAR_R * k / 4 - 1, RADAR_GRID);
  }
  gfx->drawFastHLine(cx - RADAR_R, cy, 2 * RADAR_R, RADAR_GRID);
  gfx->drawFastVLine(cx, cy - RADAR_R, 2 * RADAR_R, RADAR_GRID);
  gfx->fillCircle(cx, cy, 3, RADAR_TAG);
  radarText(cx - 5, cy - RADAR_R - 22, "N", 2, RADAR_TAG);
  radarText(cx + 6, cy + RADAR_R - 24, "S", 2, RADAR_TAG);   // inside the ring: HOME sits below it
  radarText(cx + RADAR_R + 8, cy - 7, "E", 2, RADAR_TAG);
  radarText(cx - RADAR_R - 19, cy - 7, "W", 2, RADAR_TAG);
  char lab[12];
  snprintf(lab, sizeof(lab), "%.0fkm", RADAR_RING3_KM[radarRange]);
  radarText(cx + RADAR_R * 3 / 4 - (int)strlen(lab) * 12 - 4, cy - 20, lab, 2, RADAR_GRID);
}

static void radarPlane(const AdsbPlane &a, int x, int y, int cx, int cy, uint16_t body, uint16_t vec) {
  float t = a.track * PI / 180.0f, s = sinf(t), c = cosf(t);
  // speed vector: one minute of travel x1.5, at this range's scale (clipped at the ring)
  if (a.gs > 0) {
    float km = a.gs * 1.852f / 60.0f * 1.5f;
    int len = (int)(km / radarOuterKm() * RADAR_R);
    if (len < 4) len = 4;
    if (len > 90) len = 90;
    int ex = x + (int)(s * len), ey = y - (int)(c * len);
    float d = sqrtf((float)(ex - cx) * (ex - cx) + (float)(ey - cy) * (ey - cy));
    if (d > RADAR_R) { float f = (RADAR_R - sqrtf((float)(x - cx) * (x - cx) + (float)(y - cy) * (y - cy))) / (d - sqrtf((float)(x - cx) * (x - cx) + (float)(y - cy) * (y - cy)) + 0.01f);
                       if (f < 0) f = 0; if (f > 1) f = 1; ex = x + (int)((ex - x) * f); ey = y + (int)((ey - y) * f); }
    gfx->drawLine(x, y, ex, ey, vec);
    gfx->drawLine(x + 1, y, ex + 1, ey, vec);
  }
  // triangle: nose 14 px ahead, tail 6 px behind, 8 px either side
  int nx = x + (int)(s * 14), ny = y - (int)(c * 14);
  int bx = x - (int)(s * 6),  by = y + (int)(c * 6);
  int wx = (int)(c * 8), wy = (int)(s * 8);
  gfx->fillTriangle(nx, ny, bx + wx, by + wy, bx - wx, by - wy, body);
  // tag: callsign, then altitude / type
  int tx = x + 14, ty = y - 14;
  if (tx > cx + RADAR_R - 70) tx = x - 14 - (int)strlen(a.flight) * 12;   // flip to the left near the east edge
  radarText(tx, ty, a.flight, 2, RADAR_TAG);
  char l2[20];
  if (a.has_alt) snprintf(l2, sizeof(l2), "%d ft %s", a.alt, a.type);
  else           snprintf(l2, sizeof(l2), "%s", a.type);
  radarText(tx, ty + 18, l2, 1, RADAR_TAG2);
}

void showRadar() {
  gfx->fillScreen(RADAR_BG);
  int cx = 233 + radarOx, cy = 233 + radarOy;
  if (!radarHaveLoc) {
    radarGrid(cx, cy);
    gfx->fillRoundRect(78, 170, 310, 126, 16, RGB565(10, 14, 30));
    centred(184, "Where are you?", 2, RADAR_TAG2);
    centred(214, "Settings > Setup on phone,", 1, RADAR_TAG);
    centred(230, "then type your location", 1, RADAR_TAG);
    centred(246, "under Plane radar.", 1, RADAR_TAG);
    centred(270, "BOOT = home", 1, RGB565(140, 140, 160));
    gfx->flush();
    return;
  }
  radarStart();
  if (radarDetail) { radarDrawDetail(); return; }
  AdsbPlane *snap = radarSnap;
  if (!snap || !radarPlanes) return;
  int n; uint32_t at; char status[40];
  xSemaphoreTake(radarLock, portMAX_DELAY);
  n = radarCount; at = radarFetchedAt;
  memcpy(snap, radarPlanes, sizeof(AdsbPlane) * n);
  snprintf(status, sizeof(status), "%s", radarStatus);
  xSemaphoreGive(radarLock);

  radarSweep(cx, cy);                                // v3.21: under the rings and the planes
  radarGrid(cx, cy);
  float pxPerKm = RADAR_R / radarOuterKm();
  int inside = 0;
  radarHitN = 0;
  for (int i = 0; i < n; i++) {
    float dx, dy;
    radarPos(snap[i], at, &dx, &dy);
    float r = sqrtf(dx * dx + dy * dy) * pxPerKm;
    if (r <= RADAR_R - 6) {
      int px = cx + (int)(dx * pxPerKm), py = cy - (int)(dy * pxPerKm);
      float since = radarSince(atan2f(dx, dy));         // bearing, clockwise from north
      radarPlane(snap[i], px, py, cx, cy, radarEcho(since, 255, 40, 40), radarEcho(since, 255, 0, 255));
      if (radarHitN < RADAR_MAX) {
        radarHitX[radarHitN] = px; radarHitY[radarHitN] = py;
        int tw = (int)strlen(snap[i].flight) * 12, tx = px + 14;
        if (tx > cx + RADAR_R - 70) tx = px - 14 - tw;   // same flip as radarPlane()
        radarHitTx[radarHitN] = tx; radarHitTw[radarHitN] = tw;
        memcpy(radarHitHex[radarHitN], snap[i].hex, 8);
        radarHitN++;
      }
      inside++;
    } else if (r > 0) {                               // beyond the ring: a dot on the rim, right bearing
      int rx = cx + (int)(dx * pxPerKm / r * (RADAR_R + 14)), ry = cy - (int)(dy * pxPerKm / r * (RADAR_R + 14));
      gfx->fillCircle(rx, ry, 4, radarEcho(radarSince(atan2f(dx, dy)), 255, 40, 40));
    }
  }
  // status, small, under N; HOME at the bottom
  char line[48];
  if (status[0] && (!at || millis() - at > 20000)) snprintf(line, sizeof(line), "%s", status);
  else if (!at) snprintf(line, sizeof(line), "looking for aircraft...");
  else          snprintf(line, sizeof(line), "%d aircraft", inside);
  centred(cy - RADAR_R + 14, line, 1, RGB565(150, 170, 190));
  gfx->fillRoundRect(188, 432, 90, 24, 10, RGB565(10, 14, 30));
  gfx->drawRoundRect(188, 432, 90, 24, 10, RGB565(70, 110, 150));
  centred(437, "HOME", 2, RADAR_TAG);
  gfx->flush();
}

// ---------------------------------------------------------------- detail screen (v3.15)

static const char *radarCompass(float deg) {
  static const char *P[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  int i = (int)floorf(fmodf(deg + 360.0f + 22.5f, 360.0f) / 45.0f);
  return P[i & 7];
}

static void radarOpenDetail(const char *hex) {
  xSemaphoreTake(radarLock, portMAX_DELAY);
  bool found = false;
  for (int i = 0; i < radarCount; i++)
    if (!strcmp(radarPlanes[i].hex, hex)) { radarSel = radarPlanes[i]; radarSelAt = radarFetchedAt; found = true; break; }
  if (found) {
    snprintf(radarSelHex, sizeof(radarSelHex), "%s", hex);
    radarSelSeen = millis();
    memset(&radarInfo, 0, sizeof(radarInfo));
    snprintf(radarInfo.hex, sizeof(radarInfo.hex), "%s", radarSel.hex);
    snprintf(radarInfo.call, sizeof(radarInfo.call), "%s", radarSel.flight);
    radarInfoState = 1;                               // the fetch task looks it up next
  }
  xSemaphoreGive(radarLock);
  if (!found) return;
  radarDetail = true;
  Serial.printf("radar  : details for %s (%s)\n", radarSel.flight, radarSel.hex);
  radarDrawDetail();
}

// Centred text in the larger UI font if it fits the width, else the small one.
static void radarFit(int yTop, const char *s, uint16_t c, int maxW) {
  int16_t x1, y1; uint16_t w, h;
  gfx->setFont(&UiBig); gfx->setTextSize(1);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setFont();
  if (w <= maxW) uiCentred(yTop, s, &UiBig, c);
  else           uiCentred(yTop + 4, s, &UiSmall, c);
}

static void radarRow(int y, const char *label, const char *value, uint16_t c) {
  uiText(96, y + 7, label, &UiSmall, RGB565(130, 150, 175));
  uiText(196, y, value, &UiBig, c);
}

void radarDrawDetail() {
  // Fresh data for the selected plane, if it is still in the feed.
  xSemaphoreTake(radarLock, portMAX_DELAY);
  for (int i = 0; i < radarCount; i++)
    if (!strcmp(radarPlanes[i].hex, radarSelHex)) {
      if (radarFetchedAt != radarSelAt || radarSelSeen == 0) { radarSel = radarPlanes[i]; radarSelAt = radarFetchedAt; }
      radarSelSeen = millis();
      break;
    }
  RadarInfo info = radarInfo;
  uint8_t ist = radarInfoState;
  xSemaphoreGive(radarLock);
  const AdsbPlane &a = radarSel;
  bool lost = millis() - radarSelSeen > 15000;

  gfx->fillScreen(RADAR_BG);
  const uint16_t white = RGB565(240, 240, 240), grey = RGB565(140, 155, 180), amber = RADAR_TAG2;
  char t[64];

  uiCentred(40, a.flight, &UiTitle, white);
  const char *who = info.airline[0] ? info.airline : (a.op[0] ? a.op : info.owner);
  if (who[0]) uiCentred(76, who, &UiSmall, amber);

  // Route
  if (info.org[0] && info.dst[0]) {
    snprintf(t, sizeof(t), "%s  ->  %s", info.org, info.dst);
    uiCentred(100, t, &UiBig, white);
    if (info.orgCity[0] || info.dstCity[0]) {
      snprintf(t, sizeof(t), "%s -> %s", info.orgCity, info.dstCity);
      uiCentred(124, t, &UiSmall, grey);
    }
  } else {
    uiCentred(108, ist == 3 ? "route unknown" : "looking up the route...", &UiSmall, grey);
  }

  // Aircraft: the feed's description, else adsbdb's maker + model; plus the registration
  const char *reg = a.reg[0] ? a.reg : info.reg;
  if (a.desc[0])                     snprintf(t, sizeof(t), "%s  %s", a.desc, reg);
  else if (info.maker[0])            snprintf(t, sizeof(t), "%s %s  %s", info.maker, info.model, reg);
  else                               snprintf(t, sizeof(t), "%s  %s", a.type, reg);
  radarFit(150, t, white, 360);

  // Live numbers
  if (a.has_alt) {
    if (a.has_vrate && abs(a.vrate) >= 200) snprintf(t, sizeof(t), "%d ft  %s%d", a.alt, a.vrate > 0 ? "^" : "v", abs(a.vrate));
    else                                     snprintf(t, sizeof(t), "%d ft  level", a.alt);
  } else snprintf(t, sizeof(t), "-");
  radarRow(190, "ALTITUDE", t, white);
  snprintf(t, sizeof(t), "%.0f kt  %.0f km/h", a.gs, a.gs * 1.852f);
  radarRow(222, "SPEED", t, white);
  snprintf(t, sizeof(t), "%.0f  %s", a.track, radarCompass(a.track));
  radarRow(254, "HEADING", t, white);
  float dx, dy;
  radarPos(a, radarSelAt, &dx, &dy);
  float km = sqrtf(dx * dx + dy * dy), brg = atan2f(dx, dy) * 180.0f / PI;
  snprintf(t, sizeof(t), "%.1f km  %s", km, radarCompass(brg));
  radarRow(286, "DISTANCE", t, white);
  bool emergency = !strcmp(a.squawk, "7500") || !strcmp(a.squawk, "7600") || !strcmp(a.squawk, "7700");
  snprintf(t, sizeof(t), "%s%s", a.squawk[0] ? a.squawk : "-", emergency ? "  EMERGENCY" : "");
  radarRow(318, "SQUAWK", t, emergency ? RGB565(255, 70, 70) : white);

  if (info.owner[0] && strcmp(info.owner, who)) {
    snprintf(t, sizeof(t), "owner: %s", info.owner);
    uiCentred(352, t, &UiSmall, grey);
  }
  if (lost) {
    snprintf(t, sizeof(t), "out of range - last seen %lus ago", (unsigned long)((millis() - radarSelSeen) / 1000));
    uiCentred(374, t, &UiSmall, RGB565(255, 150, 90));
  }
  gfx->fillRoundRect(168, 396, 130, 40, 14, RGB565(10, 14, 30));
  gfx->drawRoundRect(168, 396, 130, 40, 14, RGB565(120, 200, 255));
  uiCentred(408, "BACK", &UiBig, white);
  gfx->flush();
}

// From loop() while the radar is up: redraw (planes glide), and shift the picture every minute.
void radarTick() {
  if (millis() - radarLastFrame < RADAR_FRAME_MS) return;
  radarLastFrame = millis();
  int m = (millis() / 60000) % 8;
  static const int8_t SX[8] = { 0, 2, 3, 2, 0, -2, -3, -2 }, SY[8] = { -3, -2, 0, 2, 3, 2, 0, -2 };
  radarOx = SX[m]; radarOy = SY[m];
  if (!radarHaveLoc) return;
  if (radarDetail) {                                 // the detail screen: once a second is plenty
    static uint32_t lastDetail = 0;
    if (millis() - lastDetail < 1000) return;
    lastDetail = millis();
  }
  uint32_t f0 = millis();
  showRadar();
  // v3.21: how long a frame takes, once a minute (it should stay well under RADAR_FRAME_MS)
  static uint32_t lastLog = 0;
  if (!radarDetail && millis() - lastLog > 60000) {
    lastLog = millis();
    Serial.printf("radar  : sweep frame %lu ms\n", (unsigned long)(millis() - f0));
  }
}

void radarTap(int x, int y) {
  if (radarDetail) {                                 // detail screen: any tap goes back to the radar
    radarDetail = false;
    showRadar();
    return;
  }
  if (y >= 420 && x > 150 && x < 316) { showScreen(ST_HOME); return; }
  // A plane under the finger? (nearest drawn plane within 36 px)
  int best = -1; long bestD = 36L * 36L;
  for (int i = 0; i < radarHitN; i++) {
    long dx = x - radarHitX[i], dy = y - radarHitY[i], d = dx * dx + dy * dy;
    if (d <= bestD) { bestD = d; best = i; }
  }
  if (best < 0)                                      // or its callsign label
    for (int i = 0; i < radarHitN; i++)
      if (x >= radarHitTx[i] - 6 && x <= radarHitTx[i] + radarHitTw[i] + 6 &&
          y >= radarHitY[i] - 20 && y <= radarHitY[i] + 20) { best = i; break; }
  if (best >= 0) { radarOpenDetail(radarHitHex[best]); return; }
  radarRange = (radarRange + 1) % 4;
  radarSaveConfig();
  Serial.printf("radar  : range %.0f km\n", RADAR_RING3_KM[radarRange]);
  showRadar();
}

// The carousel icon: rings, a sweep and a blip.
void drawRadarIcon(int cx, int cy, int r, uint16_t fg, uint16_t bg) {
  gfx->drawCircle(cx, cy, r, fg);
  gfx->drawCircle(cx, cy, r - 1, fg);
  gfx->drawCircle(cx, cy, r * 6 / 10, fg);
  gfx->drawCircle(cx, cy, r * 2 / 10, fg);
  gfx->drawFastHLine(cx - r, cy, 2 * r, fg);
  gfx->drawFastVLine(cx, cy - r, 2 * r, fg);
  for (int k = 0; k < 3; k++) gfx->drawLine(cx, cy, cx + r * 7 / 10 + k, cy - r * 7 / 10 + k, fg);   // sweep
  gfx->fillCircle(cx - r * 4 / 10, cy - r * 3 / 10, r / 9 + 1, RGB565(255, 80, 80));               // blip
}
