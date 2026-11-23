/*
 * Remote (v3.38) - control the badge from a phone browser: http://badge-<name>.local/remote
 * (or the badge's IP + /remote). Same Wi-Fi only; no app needed.
 *
 *   - speaker VOLUME (0..100, saved) and the Sounds on/off switch;
 *   - a SOUNDBOARD: slap, drum roll, Oracle chime, a random rumour;
 *   - OPEN any app on the badge, or go home;
 *   - ASK THE ORACLE, SLAP another badge, look after DANNY-GOTCHI (coffee / thanks / sleep, with his bars).
 *
 * Routes (registered by webBegin()):
 *   GET /remote          the page (one self-contained HTML document, below)
 *   GET /api/state       JSON: name, version, volume, muted, screen, gotchi needs, who is online
 *   GET /api/do?...      vol=N | mute=0/1 | play=slap|drum|chime|rumour | app=NAME | home=1 | oracle=1 |
 *                        slap=CLAUDIO|DANNY|WALTER | gotchi=coffee|thanks|sleep        -> {"ok":true}
 * Volume and mute act at once; everything that draws or plays is QUEUED and done by remoteTick() in
 * loop(), so a 2 s Oracle animation never runs inside the web request.
 * No password: anyone on the same Wi-Fi can use it (as with the setup page).
 * Sorts after Radar.ino: uses Gotchi / Oracle / Decider directly, Rumours / Sos through functions.
 */

#define REM_VERSION "3.38"

static int remCmd = 0, remArg = 0;          // one queued action (the page sends one at a time)
#define REM_PLAY    1
#define REM_APP     2
#define REM_HOME    3
#define REM_ORACLE  4
#define REM_SLAP    5
#define REM_GOTCHI  6

#include "remote_page.h"                  // REM_PAGE: the HTML (a header - ctags cannot read raw strings)

static const char *remScreenName(int s) {
  switch (s) {
    case ST_HOME: return "HOME"; case ST_MENU: return "SETTINGS"; case ST_GALLERY: case ST_GALLERY_SYNC: return "PICTURES";
    case ST_DECIDER: return "ASK DANNY"; case ST_RUMOURS: return "RUMOURS"; case ST_SLAP: case ST_SLAPPED: return "SLAP";
    case ST_VOTE: return "VOTE"; case ST_RADAR: return "RADAR"; case ST_ORACLE: return "ORACLE"; case ST_CLOCK: return "CLOCK";
    case ST_GOTCHI: return "GOTCHI"; default: return "OTHER";
  }
}

static void remState() {
  int c, mo, e; bool asleep, dead;
  gotchiState(&c, &mo, &e, &asleep, &dead);
  String j = "{\"name\":\"";
  const char *me = sosMeLabel();
  j += sosMeIndex() >= 0 ? me : "Thanks Danny";
  j += "\",\"ver\":\"" REM_VERSION "\",\"vol\":"; j += decGetVolume();
  j += ",\"muted\":"; j += sosIsMuted() ? "true" : "false";
  j += ",\"link\":"; j += sosLinkUp() ? "true" : "false";
  j += ",\"screen\":\""; j += remScreenName(screen);
  j += "\",\"gotchi\":{\"c\":"; j += c; j += ",\"m\":"; j += mo; j += ",\"e\":"; j += e;
  j += ",\"asleep\":"; j += asleep ? "true" : "false"; j += ",\"dead\":"; j += dead ? "true" : "false";
  j += "},\"badges\":[";
  for (int i = 0; i < 3; i++) {
    if (i) j += ",";
    j += "{\"name\":\""; j += sosNameOf(i); j += "\",\"me\":"; j += i == sosMeIndex() ? "true" : "false";
    j += ",\"up\":"; j += sosIsOnline(i) ? "true" : "false"; j += "}";
  }
  j += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

static void remReply(bool ok, const char *err) {
  String j = ok ? String("{\"ok\":true}") : String("{\"ok\":false,\"err\":\"") + err + "\"}";
  server.send(200, "application/json", j);
}

static void remDo() {
  if (server.hasArg("vol")) { decSetVolume(server.arg("vol").toInt()); remReply(true, ""); return; }
  if (server.hasArg("mute")) { sosSetMuted(server.arg("mute").toInt() != 0); remReply(true, ""); return; }
  if (remCmd) { remReply(false, "Busy - try again"); return; }
  if (server.hasArg("play")) {
    String p = server.arg("play");
    if (sosIsMuted()) { remReply(false, "Sounds are off"); return; }
    remArg = p == "slap" ? 0 : p == "drum" ? 1 : p == "chime" ? 2 : p == "rumour" ? 3 : -1;
    if (remArg < 0) { remReply(false, "Unknown sound"); return; }
    remCmd = REM_PLAY;
  } else if (server.hasArg("app")) {
    String a = server.arg("app");
    remArg = -1;
    for (int i = 0; i < APP_COUNT; i++) if (a.equalsIgnoreCase(APP_NAME[i])) remArg = i;
    if (remArg < 0) { remReply(false, "Unknown app"); return; }
    remCmd = REM_APP;
  } else if (server.hasArg("home")) {
    remCmd = REM_HOME;
  } else if (server.hasArg("oracle")) {
    remCmd = REM_ORACLE;
  } else if (server.hasArg("slap")) {
    String w = server.arg("slap");
    remArg = -1;
    for (int i = 0; i < 3; i++) if (w.equalsIgnoreCase(sosNameOf(i))) remArg = i;
    if (remArg < 0 || remArg == sosMeIndex()) { remReply(false, "Pick another badge"); return; }
    if (!sosLinkUp()) { remReply(false, "Badge network is down"); return; }
    remCmd = REM_SLAP;
  } else if (server.hasArg("gotchi")) {
    String g = server.arg("gotchi");
    remArg = g == "coffee" ? 0 : g == "thanks" ? 1 : g == "sleep" ? 2 : -1;
    if (remArg < 0) { remReply(false, "Unknown action"); return; }
    remCmd = REM_GOTCHI;
  } else { remReply(false, "Nothing to do"); return; }
  remReply(true, "");
}

void remoteRoutes() {
  server.on("/remote", []() { server.send_P(200, "text/html", REM_PAGE); });
  server.on("/api/state", remState);
  server.on("/api/do", remDo);
}

// loop(): carry out a queued action from the remote page.
void remoteTick() {
  if (!remCmd) return;
  int cmd = remCmd, arg = remArg;
  remCmd = 0;
  Serial.printf("remote : command %d (%d)\n", cmd, arg);
  if (saverActive()) saverWake(cmd == REM_PLAY || cmd == REM_SLAP || cmd == REM_GOTCHI, "remote");
  switch (cmd) {
    case REM_PLAY:
      if (arg == 0) sosPlaySlapSound();
      else if (arg == 1) decSoundStart();
      else if (arg == 2) sosPlay(SND_ORA_REVEAL, SND_ORA_REVEAL_LEN, ORA_VOL);
      else {
        int rumoursCount();
        int n = rumoursCount();
        if (n > 0) rumoursPlay(esp_random() % n);
      }
      break;
    case REM_APP:    openApp(arg); break;
    case REM_HOME:   showScreen(ST_HOME); break;
    case REM_ORACLE: if (screen != ST_ORACLE) showScreen(ST_ORACLE); oraAsk(); break;
    case REM_SLAP:   sosRemoteSlap(arg, 6); break;
    case REM_GOTCHI: gotchiAction(arg); break;
  }
}
