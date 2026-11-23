// Phone link (v3.38) - see phonelink.h.
//
// MEDIA REMOTE: a BLE HID device with one consumer-control report (next, previous, stop, play/pause,
// mute, volume up/down). Phones treat it like a Bluetooth keyboard's media keys: whatever app is
// playing reacts. Pairing is "just works" with bonding (Secure Connections); the pairing is kept in NVS.
//
// IPHONE NOTIFICATIONS (Apple Notification Center Service, ANCS): once a bonded iPhone is connected
// and the link is encrypted, the badge - although it is the peripheral - acts as a GATT client on that
// same connection: it finds the ANCS service on the phone, subscribes to Notification Source and Data
// Source, and for every new notification asks (Control Point) for its app, title and message. The
// Arduino BLE wrapper's BLEClient cannot work on an existing connection, so this part talks to the
// NimBLE host directly, and listens to GAP events with ble_gap_event_listener_register().
// The advertisement solicits ANCS (AD type 0x15) so iOS offers it to the badge.
// Android has no ANCS: there only the media remote works.

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/ble_store.h"
#include "os/os_mbuf.h"
#include "phonelink.h"

static bool sStarted = false, sConnected = false, sSecure = false, sAncs = false;
static uint16_t sConn = BLE_HS_CONN_HANDLE_NONE;
static BLEServer *sServer = nullptr;
static BLEHIDDevice *sHid = nullptr;
static BLECharacteristic *sInput = nullptr;
static bool sAdvOn = true;

// ---------------------------------------------------------------- HID report map: consumer control
static uint8_t REPORT_MAP[] = {
  0x05, 0x0C,        // Usage Page (Consumer)
  0x09, 0x01,        // Usage (Consumer Control)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x15, 0x00,        //   Logical Minimum (0)
  0x25, 0x01,        //   Logical Maximum (1)
  0x75, 0x01,        //   Report Size (1)
  0x95, 0x08,        //   Report Count (8)
  0x09, 0xB5,        //   Usage (Scan Next Track)       bit 0
  0x09, 0xB6,        //   Usage (Scan Previous Track)   bit 1
  0x09, 0xB7,        //   Usage (Stop)                  bit 2
  0x09, 0xCD,        //   Usage (Play/Pause)            bit 3
  0x09, 0xE2,        //   Usage (Mute)                  bit 4
  0x09, 0xE9,        //   Usage (Volume Increment)      bit 5
  0x09, 0xEA,        //   Usage (Volume Decrement)      bit 6
  0x0A, 0x23, 0x02,  //   Usage (AC Home)               bit 7
  0x81, 0x02,        //   Input (Data, Variable, Absolute)
  0xC0               // End Collection
};

// ---------------------------------------------------------------- ANCS
// UUIDs (128-bit, little-endian byte order as NimBLE stores them)
static const ble_uuid128_t UUID_ANCS = BLE_UUID128_INIT(0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, 0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
static const ble_uuid128_t UUID_NS   = BLE_UUID128_INIT(0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, 0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
static const ble_uuid128_t UUID_CP   = BLE_UUID128_INIT(0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, 0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
static const ble_uuid128_t UUID_DS   = BLE_UUID128_INIT(0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, 0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);

static uint16_t aSvcStart = 0, aSvcEnd = 0, aNs = 0, aCp = 0, aDs = 0, aNsCccd = 0, aDsCccd = 0;
static bool aBusy = false;                         // a Get Notification Attributes request is in flight
static uint8_t aPendUid[8][4];                     // new notifications waiting for their request
static int aPendN = 0;
static uint8_t aCurUid[4];
static int aCurCat = 0;
static uint8_t aBuf[600];                          // Data Source reply, assembled
static int aLen = 0;
static uint8_t aCatOf[8][5];                       // uid -> category for the pending ones
static int aCatN = 0;
static uint32_t aReqAt = 0;

struct Notif { char app[24]; char title[48]; char msg[140]; int cat; };
static Notif nQueue[4];
static volatile int nHead = 0, nTail = 0;
static portMUX_TYPE nLock = portMUX_INITIALIZER_UNLOCKED;

// UTF-8 to plain ASCII for the badge fonts: accents lose their accent, curly quotes and dashes
// become straight ones, emoji and other symbols are dropped.
static void toAscii(const uint8_t *s, int n, char *out, int outLen) {
  int o = 0;
  for (int i = 0; i < n && o < outLen - 1; ) {
    uint32_t c = s[i];
    int k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (k > 1 && i + k <= n) {
      c &= (0xFF >> (k + 1));
      for (int j = 1; j < k; j++) c = (c << 6) | (s[i + j] & 0x3F);
    }
    i += k;
    char r = 0;
    const char *rs = nullptr;
    if (c >= 0x20 && c < 0x7F) r = (char)c;
    else if (c == '\n' || c == '\r' || c == '\t') r = ' ';
    else if (c >= 0xC0 && c <= 0xFF) {
      static const char *L = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
      r = L[c - 0xC0];
      if (c == 0xDF) rs = "ss";
    }
    else if (c == 0x2018 || c == 0x2019 || c == 0x201A || c == 0x2032) r = '\'';
    else if (c == 0x201C || c == 0x201D || c == 0x201E || c == 0x2033) r = '"';
    else if (c == 0x2013 || c == 0x2014 || c == 0x2212) r = '-';
    else if (c == 0x2026) rs = "...";
    else if (c == 0x20AC) rs = "EUR";
    else if (c == 0xA0 || c == 0x2009 || c == 0x202F) r = ' ';
    if (rs) { while (*rs && o < outLen - 1) out[o++] = *rs++; }
    else if (r) out[o++] = r;
  }
  out[o] = 0;
}

static void appName(const char *bundle, char *out, int outLen) {
  static const char *MAP[][2] = {
    { "com.apple.MobileSMS", "Messages" }, { "com.apple.mobilephone", "Phone" }, { "com.apple.mobilemail", "Mail" },
    { "com.apple.mobilecal", "Calendar" }, { "com.apple.reminders", "Reminders" }, { "com.apple.facetime", "FaceTime" },
    { "net.whatsapp.WhatsApp", "WhatsApp" }, { "ph.telegra.Telegraph", "Telegram" }, { "com.tinyspeck.chatlyio", "Slack" },
    { "com.microsoft.skype.teams", "Teams" }, { "com.microsoft.Office.Outlook", "Outlook" }, { "com.google.Gmail", "Gmail" },
    { "com.facebook.Messenger", "Messenger" }, { "org.whispersystems.signal", "Signal" }, { "com.burbn.instagram", "Instagram" },
    { "com.atebits.Tweetie2", "X" }, { "com.hammerandchisel.discord", "Discord" }, { "com.linkedin.LinkedIn", "LinkedIn" },
  };
  for (auto &m : MAP) if (!strcmp(bundle, m[0])) { snprintf(out, outLen, "%s", m[1]); return; }
  const char *dot = strrchr(bundle, '.');
  snprintf(out, outLen, "%s", dot ? dot + 1 : bundle);
}

static int ancsReqWritten(uint16_t, const struct ble_gatt_error *err, struct ble_gatt_attr *, void *) {
  if (err->status) { Serial.printf("phone  : notification request failed (%d)\n", err->status); aBusy = false; }
  return 0;
}

static void ancsRequestNext() {
  if (aBusy || !aPendN || !sAncs) return;
  memcpy(aCurUid, aPendUid[0], 4);
  aCurCat = 0;
  for (int i = 0; i < aCatN; i++) if (!memcmp(aCatOf[i], aCurUid, 4)) aCurCat = aCatOf[i][4];
  for (int i = 1; i < aPendN; i++) memcpy(aPendUid[i - 1], aPendUid[i], 4);
  aPendN--;
  uint8_t cmd[] = { 0x00, aCurUid[0], aCurUid[1], aCurUid[2], aCurUid[3],
                    0x00,                    // AppIdentifier
                    0x01, 40, 0x00,          // Title, max 40 bytes
                    0x03, 120, 0x00 };       // Message, max 120 bytes
  aLen = 0;
  aBusy = true;
  aReqAt = millis();
  int rc = ble_gattc_write_flat(sConn, aCp, cmd, sizeof(cmd), ancsReqWritten, nullptr);
  if (rc) { Serial.printf("phone  : notification request not sent (%d)\n", rc); aBusy = false; }
}

// Try to read a complete reply from aBuf: cmd, uid, then 3 attributes (id, len16, data).
static void ancsTryParse() {
  if (aLen < 5) return;
  int p = 5;
  const uint8_t *att[4] = { 0 };
  int alen[4] = { 0 };
  for (int k = 0; k < 3; k++) {
    if (p + 3 > aLen) return;
    int id = aBuf[p], l = aBuf[p + 1] | (aBuf[p + 2] << 8);
    if (p + 3 + l > aLen) return;                 // not all here yet
    if (id <= 3) { att[id] = aBuf + p + 3; alen[id] = l; }
    p += 3 + l;
  }
  Notif n;
  char bundle[64];
  toAscii(att[0] ? att[0] : (const uint8_t *)"", alen[0], bundle, sizeof(bundle));
  appName(bundle, n.app, sizeof(n.app));
  toAscii(att[1] ? att[1] : (const uint8_t *)"", alen[1], n.title, sizeof(n.title));
  toAscii(att[3] ? att[3] : (const uint8_t *)"", alen[3], n.msg, sizeof(n.msg));
  n.cat = aCurCat;
  portENTER_CRITICAL(&nLock);
  int next = (nHead + 1) % 4;
  if (next != nTail) { nQueue[nHead] = n; nHead = next; }    // full: drop the newest
  portEXIT_CRITICAL(&nLock);
  Serial.printf("phone  : notification from %s: \"%s\"\n", n.app, n.title);
  aBusy = false;
  aLen = 0;
  ancsRequestNext();
}

static int ancsCccdWritten(uint16_t, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg) {
  int which = (int)(intptr_t)arg;
  if (err->status) { Serial.printf("phone  : ANCS subscribe %d failed (%d)\n", which, err->status); return 0; }
  if (which == 1) {                                // Data Source done; now Notification Source
    static const uint8_t on[2] = { 1, 0 };
    ble_gattc_write_flat(sConn, aNsCccd, on, 2, ancsCccdWritten, (void *)2);
  } else {
    sAncs = true;
    Serial.println("phone  : iPhone notifications on");
  }
  return 0;
}

static int ancsDscFound(uint16_t, const struct ble_gatt_error *err, uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *) {
  if (err->status == 0 && dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902) {
    if (chr_val_handle == aNs) aNsCccd = dsc->handle;
    if (chr_val_handle == aDs) aDsCccd = dsc->handle;
  } else if (err->status == BLE_HS_EDONE) {
    if (!aNsCccd) aNsCccd = aNs + 1;               // usual layout if discovery found nothing
    if (!aDsCccd) aDsCccd = aDs + 1;
    static const uint8_t on[2] = { 1, 0 };
    ble_gattc_write_flat(sConn, aDsCccd, on, 2, ancsCccdWritten, (void *)1);
  }
  return 0;
}

static int ancsChrFound(uint16_t, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *) {
  if (err->status == 0 && chr) {
    if (!ble_uuid_cmp(&chr->uuid.u, &UUID_NS.u)) aNs = chr->val_handle;
    if (!ble_uuid_cmp(&chr->uuid.u, &UUID_CP.u)) aCp = chr->val_handle;
    if (!ble_uuid_cmp(&chr->uuid.u, &UUID_DS.u)) aDs = chr->val_handle;
  } else if (err->status == BLE_HS_EDONE) {
    if (aNs && aCp && aDs) ble_gattc_disc_all_dscs(sConn, aNs, aSvcEnd, ancsDscFound, nullptr);
    else Serial.println("phone  : ANCS characteristics missing");
  }
  return 0;
}

static int ancsSvcFound(uint16_t, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *) {
  if (err->status == 0 && svc) { aSvcStart = svc->start_handle; aSvcEnd = svc->end_handle; }
  else if (err->status == BLE_HS_EDONE) {
    if (aSvcStart) ble_gattc_disc_all_chrs(sConn, aSvcStart, aSvcEnd, ancsChrFound, nullptr);
    else Serial.println("phone  : no iPhone notification service (Android, or not shared) - media keys only");
  }
  return 0;
}

static void ancsStart() {
  aSvcStart = aSvcEnd = aNs = aCp = aDs = aNsCccd = aDsCccd = 0;
  aBusy = false; aPendN = 0; aCatN = 0; aLen = 0; sAncs = false;
  int rc = ble_gattc_disc_svc_by_uuid(sConn, &UUID_ANCS.u, ancsSvcFound, nullptr);
  if (rc) Serial.printf("phone  : ANCS discovery not started (%d)\n", rc);
}

// ---------------------------------------------------------------- GAP events (all of them, alongside the wrapper)
static struct ble_gap_event_listener sListener;

static int gapListener(struct ble_gap_event *ev, void *) {
  switch (ev->type) {
    case BLE_GAP_EVENT_ENC_CHANGE:
      if (ev->enc_change.status == 0 && ev->enc_change.conn_handle == sConn) {
        sSecure = true;
        Serial.println("phone  : link encrypted (paired)");
        ancsStart();
      } else if (ev->enc_change.status) {
        Serial.printf("phone  : encryption failed (%d) - forget the badge on the phone and pair again\n", ev->enc_change.status);
      }
      break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
      if (ev->notify_rx.conn_handle != sConn) break;
      uint8_t b[256];
      int n = OS_MBUF_PKTLEN(ev->notify_rx.om);
      if (n > (int)sizeof(b)) n = sizeof(b);
      os_mbuf_copydata(ev->notify_rx.om, 0, n, b);
      if (ev->notify_rx.attr_handle == aNs && n >= 8) {
        int evt = b[0], flags = b[1], cat = b[2];
        if (evt == 0 && !(flags & 0x04)) {          // added, and not one that was already there
          if (aPendN < 8) { memcpy(aPendUid[aPendN++], b + 4, 4); }
          if (aCatN < 8) { memcpy(aCatOf[aCatN], b + 4, 4); aCatOf[aCatN][4] = cat; aCatN++; }
          else { memmove(aCatOf[0], aCatOf[1], 7 * 5); memcpy(aCatOf[7], b + 4, 4); aCatOf[7][4] = cat; }
          if (aBusy && millis() - aReqAt > 3000) aBusy = false;   // a lost reply: carry on
          ancsRequestNext();
        }
      } else if (ev->notify_rx.attr_handle == aDs && aBusy) {
        if (aLen + n <= (int)sizeof(aBuf)) { memcpy(aBuf + aLen, b, n); aLen += n; }
        ancsTryParse();
      }
      break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // The phone forgot the badge but the badge still has its old keys: drop them, pair afresh.
      struct ble_gap_conn_desc d;
      if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
      Serial.println("phone  : phone paired again - old pairing replaced");
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default: break;
  }
  return 0;
}

// ---------------------------------------------------------------- server callbacks
class SrvCb : public BLEServerCallbacks {
  void onConnect(BLEServer *s, ble_gap_conn_desc *desc) override {
    sConn = desc->conn_handle;
    sConnected = true;
    sSecure = desc->sec_state.encrypted;
    Serial.printf("phone  : connected (%s)\n", sSecure ? "already paired" : "pairing...");
    if (!sSecure) BLESecurity::startSecurity(sConn);
    else ancsStart();
  }
  void onDisconnect(BLEServer *s, ble_gap_conn_desc *desc) override {
    sConnected = false; sSecure = false; sAncs = false;
    sConn = BLE_HS_CONN_HANDLE_NONE;
    Serial.println("phone  : disconnected");
    if (sAdvOn) BLEDevice::startAdvertising();
  }
};

// ---------------------------------------------------------------- API
void phoneBegin(const char *deviceName) {
  if (sStarted) return;
  uint32_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  BLEDevice::init(deviceName);
  BLESecurity::setAuthenticationMode(true, false, true);       // bonding, no MITM (no display code), SC
  BLESecurity::setCapability(BLE_HS_IO_NO_INPUT_OUTPUT);
  sServer = BLEDevice::createServer();
  sServer->setCallbacks(new SrvCb());
  sHid = new BLEHIDDevice(sServer);
  sInput = sHid->inputReport(1);
  sHid->manufacturer()->setValue("Sons of Sudo");     // v3.40: manufacturer() CREATES the characteristic;
                                                    // manufacturer(String) on its own wrote through a null pointer
  sHid->pnp(0x02, 0xE502, 0xA111, 0x0210);
  sHid->hidInfo(0x00, 0x01);
  sHid->reportMap(REPORT_MAP, sizeof(REPORT_MAP));
  sHid->startServices();
  sHid->setBatteryLevel(100);
  ble_gap_event_listener_register(&sListener, gapListener, nullptr);

  BLEAdvertising *adv = sServer->getAdvertising();
  BLEAdvertisementData ad, sr;
  ad.setFlags(0x06);
  ad.setAppearance(0x0180);                        // generic remote control
  ad.setCompleteServices(sHid->hidService()->getUUID());
  char sol[18] = { 17, 0x15 };                     // Service Solicitation, 128-bit: ANCS
  memcpy(sol + 2, UUID_ANCS.value, 16);
  ad.addData(sol, sizeof(sol));
  sr.setName(deviceName);
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);
  adv->start();
  sStarted = true;
  Serial.printf("phone  : Bluetooth on - pair \"%s\" on your phone (internal RAM %lu -> %lu bytes free)\n", deviceName,
                (unsigned long)heap0, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool phoneStarted() { return sStarted; }
bool phoneConnected() { return sConnected; }
bool phoneSecure() { return sConnected && sSecure; }
bool phoneNotifyReady() { return sConnected && sAncs; }

void phoneMediaKey(int key) {
  if (!sConnected || !sSecure || !sInput || key < 0 || key > 7) return;
  uint8_t v = 1 << key;
  sInput->setValue(&v, 1);
  sInput->notify();
  delay(12);
  v = 0;
  sInput->setValue(&v, 1);
  sInput->notify();
}

void phoneAdvertise(bool on) {
  sAdvOn = on;
  if (!sStarted) return;
  if (on) BLEDevice::startAdvertising();
  else sServer->getAdvertising()->stop();
}

void phoneForget() {
  ble_store_clear();
  Serial.println("phone  : all pairings deleted");
}

bool phoneNextNotification(char *app, int appLen, char *title, int titleLen, char *msg, int msgLen, int *category) {
  portENTER_CRITICAL(&nLock);
  if (nHead == nTail) { portEXIT_CRITICAL(&nLock); return false; }
  Notif n = nQueue[nTail];
  nTail = (nTail + 1) % 4;
  portEXIT_CRITICAL(&nLock);
  snprintf(app, appLen, "%s", n.app);
  snprintf(title, titleLen, "%s", n.title);
  snprintf(msg, msgLen, "%s", n.msg);
  *category = n.cat;
  return true;
}
