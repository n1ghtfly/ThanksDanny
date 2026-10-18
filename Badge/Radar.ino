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
 * the rim at the right bearing. Tap = next range (5 / 10 / 15 / 25 km on ring 3), saved.
 * HOME at the bottom or BOOT = home screen.
 *
 * Location: entered once on the phone setup page (Settings -> Setup on phone), stored only on the
 * badge (NVS "radar") - never in the code, the repository is public. "52.37, 4.90" as copied from
 * a map app works. A default can also come from sos_local.h (RADAR_DEFAULT_LAT / _LON).
 *
 * Burn-in: the radar is meant to be left running, so the whole picture shifts by a few pixels
 * every minute (the rings would otherwise stand still on the AMOLED for hours).
 */

#include "src/adsbparse.h"

#define RADAR_FETCH_MS   5000
#define RADAR_FRAME_MS   300                // redraw cadence (aircraft move between fetches)
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

static AdsbPlane radarPlanes[RADAR_MAX];   // shared with the fetch task, under radarLock
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

static void radarTask(void *) {
  static AdsbPlane tmp[RADAR_MAX];
  uint32_t last = 0;
  while (radarRun) {
    if (WiFi.status() != WL_CONNECTED) { snprintf(radarStatus, sizeof(radarStatus), "no Wi-Fi"); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
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

void radarStop() { radarRun = false; }               // the task ends after its current request

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

static void radarPlane(const AdsbPlane &a, int x, int y, int cx, int cy) {
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
    gfx->drawLine(x, y, ex, ey, RADAR_TRACK);
    gfx->drawLine(x + 1, y, ex + 1, ey, RADAR_TRACK);
  }
  // triangle: nose 14 px ahead, tail 6 px behind, 8 px either side
  int nx = x + (int)(s * 14), ny = y - (int)(c * 14);
  int bx = x - (int)(s * 6),  by = y + (int)(c * 6);
  int wx = (int)(c * 8), wy = (int)(s * 8);
  gfx->fillTriangle(nx, ny, bx + wx, by + wy, bx - wx, by - wy, RADAR_PLANE);
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
  static AdsbPlane snap[RADAR_MAX];
  int n; uint32_t at; char status[40];
  xSemaphoreTake(radarLock, portMAX_DELAY);
  n = radarCount; at = radarFetchedAt;
  memcpy(snap, radarPlanes, sizeof(AdsbPlane) * n);
  snprintf(status, sizeof(status), "%s", radarStatus);
  xSemaphoreGive(radarLock);

  radarGrid(cx, cy);
  float pxPerKm = RADAR_R / radarOuterKm();
  int inside = 0;
  for (int i = 0; i < n; i++) {
    float dx, dy;
    radarPos(snap[i], at, &dx, &dy);
    float r = sqrtf(dx * dx + dy * dy) * pxPerKm;
    if (r <= RADAR_R - 6) {
      radarPlane(snap[i], cx + (int)(dx * pxPerKm), cy - (int)(dy * pxPerKm), cx, cy);
      inside++;
    } else if (r > 0) {                               // beyond the ring: a dot on the rim, right bearing
      int rx = cx + (int)(dx * pxPerKm / r * (RADAR_R + 14)), ry = cy - (int)(dy * pxPerKm / r * (RADAR_R + 14));
      gfx->fillCircle(rx, ry, 4, RADAR_PLANE);
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

// From loop() while the radar is up: redraw (planes glide), and shift the picture every minute.
void radarTick() {
  if (millis() - radarLastFrame < RADAR_FRAME_MS) return;
  radarLastFrame = millis();
  int m = (millis() / 60000) % 8;
  static const int8_t SX[8] = { 0, 2, 3, 2, 0, -2, -3, -2 }, SY[8] = { -3, -2, 0, 2, 3, 2, 0, -2 };
  radarOx = SX[m]; radarOy = SY[m];
  if (radarHaveLoc) showRadar();
}

void radarTap(int x, int y) {
  if (y >= 420 && x > 150 && x < 316) { showScreen(ST_HOME); return; }
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
