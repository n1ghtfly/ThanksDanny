/*
 * Vote (v3.9) - questions from the website, answered on the badges.
 *
 * Someone asks a question on the poll website (web/poll.html, hosted on the GitHub Pages site)
 * with 2-4 answers. It reaches every badge over the badge network; a badge that has not answered
 * it yet pops up "<NAME> ASKS:" with the question and one button per answer, wherever it is
 * (not on top of a slap, a swing, a playing rumour or the setup pages). Tap an answer and the
 * screen turns into live results: a bar per answer with who picked it, updated as the others vote.
 * LATER puts the question away until the next start-up.
 *
 * Topics (keep in step with web/poll.html, tools/sos_cli.py and server/mosquitto/acl):
 *   sos/poll/<asker>          "<id>|<question>|<answer 1>|<answer 2>[|<3>|<4>]"   retained
 *                             an empty retained message closes (removes) the asker's question
 *   sos/vote/<asker>/<voter>  "<id>|<answer index 0-3>"                           retained
 * One open question per asker (a new one replaces the old). <id> is the asker's clock in seconds,
 * so a vote for an older question is simply ignored. The ACL only lets <asker> be the publisher's
 * own login for sos/poll, and <voter> be the publisher's own login for sos/vote: no fake questions,
 * no ballot stuffing. Retained, so a badge that was off sees open questions and the votes so far
 * when it comes back - and knows from its own retained vote that it already answered.
 *
 * Last file in the sketch (after Sos.ino), so it uses SOS_N, SOS_ID, SOS_NAME, sosMe, sosPlay.
 */

#define VOTE_MAXOPT 4
#define VOTE_COLS   24
#define VOTE_ROWS   4

struct VotePoll {
  char id[16];                        // "" = no open question from this asker
  char q[112];
  char opt[VOTE_MAXOPT][16];
  int  nopt;
  char later[16];                     // id this badge said LATER to (until the next start-up)
};
static VotePoll vPoll[SOS_N];
static char     vVoteId[SOS_N][SOS_N][16];    // [asker][voter] -> poll id that vote belongs to
static int8_t   vVote[SOS_N][SOS_N];          // [asker][voter] -> answer index, -1 none
static int      vShow = -1;                   // asker whose question is on screen
static bool     vInit = false;

static const uint16_t VOTE_COL[VOTE_MAXOPT] = {
  RGB565(40, 170, 80), RGB565(200, 60, 60), RGB565(50, 120, 210), RGB565(210, 150, 30)
};

static void voteInit() {
  if (vInit) return;
  vInit = true;
  memset(vPoll, 0, sizeof(vPoll));
  memset(vVoteId, 0, sizeof(vVoteId));
  memset(vVote, -1, sizeof(vVote));
}

static int voteWho(const char *name, int len) {
  for (int i = 0; i < SOS_N; i++)
    if ((int)strlen(SOS_ID[i]) == len && !strncmp(SOS_ID[i], name, len)) return i;
  return -1;
}

// The badge's font is plain ASCII; anything else shows as '?'.
static void voteCopy(char *dst, int n, const char *src, int len) {
  if (len > n - 1) len = n - 1;
  for (int i = 0; i < len; i++) {
    unsigned char c = (unsigned char)src[i];
    dst[i] = (c >= 32 && c < 127) ? (char)c : '?';
  }
  dst[len] = 0;
}

// This voter's answer to the asker's current question, or -1.
static int voteOf(int asker, int voter) {
  if (!vPoll[asker].id[0] || strcmp(vVoteId[asker][voter], vPoll[asker].id)) return -1;
  return vVote[asker][voter];
}

// From sosDrain() (Sos.ino) for every sos/poll/... and sos/vote/... message.
void voteOnMessage(const char *topic, const char *payload) {
  voteInit();
  bool changed = false;
  int asker = -1;
  if (!strncmp(topic, "sos/poll/", 9)) {
    asker = voteWho(topic + 9, strlen(topic + 9));
    if (asker < 0) return;
    VotePoll &p = vPoll[asker];
    if (!payload[0]) {                                      // closed
      Serial.printf("vote   : %s closed their question\n", SOS_NAME[asker]);
      p.id[0] = 0;
      changed = true;
    } else {
      // "<id>|<question>|<a1>|<a2>..." - split on '|'
      const char *f[2 + VOTE_MAXOPT];
      int fl[2 + VOTE_MAXOPT], nf = 0;
      const char *s = payload;
      while (nf < 2 + VOTE_MAXOPT) {
        const char *bar = strchr(s, '|');
        f[nf] = s;
        fl[nf] = bar ? (int)(bar - s) : (int)strlen(s);
        nf++;
        if (!bar) break;
        s = bar + 1;
      }
      if (nf < 4) { Serial.printf("vote   : ignored a malformed question from %s\n", SOS_NAME[asker]); return; }
      char id[16];
      voteCopy(id, sizeof(id), f[0], fl[0]);
      if (strcmp(id, p.id)) changed = true;
      snprintf(p.id, sizeof(p.id), "%s", id);
      voteCopy(p.q, sizeof(p.q), f[1], fl[1]);
      p.nopt = nf - 2;
      for (int i = 0; i < p.nopt; i++) voteCopy(p.opt[i], sizeof(p.opt[i]), f[2 + i], fl[2 + i]);
      Serial.printf("vote   : %s asks \"%s\" (%d answers)\n", SOS_NAME[asker], p.q, p.nopt);
    }
  } else if (!strncmp(topic, "sos/vote/", 9)) {
    const char *a = topic + 9, *slash = strchr(a, '/');
    if (!slash) return;
    asker = voteWho(a, slash - a);
    int voter = voteWho(slash + 1, strlen(slash + 1));
    if (asker < 0 || voter < 0) return;
    const char *bar = strchr(payload, '|');
    if (!payload[0] || !bar) { vVoteId[asker][voter][0] = 0; vVote[asker][voter] = -1; changed = true; }
    else {
      voteCopy(vVoteId[asker][voter], sizeof(vVoteId[0][0]), payload, bar - payload);
      int n = atoi(bar + 1);
      vVote[asker][voter] = (n >= 0 && n < VOTE_MAXOPT) ? n : -1;
      changed = true;
      if (voteOf(asker, voter) >= 0)
        Serial.printf("vote   : %s voted \"%s\" on %s's question\n", SOS_NAME[voter],
                      vPoll[asker].opt[vVote[asker][voter]], SOS_NAME[asker]);
    }
  }
  if (changed && screen == ST_VOTE && asker == vShow) {    // live update
    if (saverActive()) saverRedraw = true;                  // under the screensaver: redraw on wake
    else showVote();
  }
}

// An open question this badge has not answered (and not put off): its asker, or -1.
int voteWaiting() {
  voteInit();
  if (sosMe < 0) return -1;
  for (int a = 0; a < SOS_N; a++) {
    VotePoll &p = vPoll[a];
    if (p.id[0] && p.nopt >= 2 && voteOf(a, sosMe) < 0 && strcmp(p.later, p.id)) return a;
  }
  return -1;
}

// ---------------------------------------------------------------- the screen

#define VBTN_W 156
#define VBTN_H 56
#define VLATER_X 168
#define VLATER_Y 396
#define VLATER_W 130
#define VLATER_H 40

// Answer button i of n: two side by side, or a 2x2 grid for three or four.
static void voteBtnRect(int i, int n, int *x, int *y) {
  int col = i % 2, row = i / 2;
  *x = 233 - VBTN_W - 5 + col * (VBTN_W + 10);
  if (n <= 2) *y = 236;
  else        *y = 204 + row * (VBTN_H + 10);
  if (n == 3 && i == 2) *x = 233 - VBTN_W / 2;              // the third one centred
}

static void votePill(const char *label) {
  gfx->fillRoundRect(VLATER_X, VLATER_Y, VLATER_W, VLATER_H, 14, RGB565(10, 14, 30));
  gfx->drawRoundRect(VLATER_X, VLATER_Y, VLATER_W, VLATER_H, 14, RGB565(120, 200, 255));
  centred(VLATER_Y + 12, label, 2, RGB565_WHITE);
}

void showVote() {
  voteInit();
  if (vShow < 0 || !vPoll[vShow].id[0]) vShow = voteWaiting();
  gfx->fillScreen(RGB565(8, 10, 20));
  if (vShow < 0) {                                          // closed meanwhile
    centred(200, "question closed", 2, RGB565(200, 200, 210));
    votePill("HOME");
    gfx->flush();
    return;
  }
  VotePoll &p = vPoll[vShow];
  char head[32];
  snprintf(head, sizeof(head), "%s ASKS:", SOS_NAME[vShow]);
  centred(58, head, 2, RGB565(255, 175, 40));

  // The question, word-wrapped at spaces (the website keeps it to 4 x 24).
  const char *s = p.q;
  int line = 0;
  while (*s && line < VOTE_ROWS) {
    while (*s == ' ') s++;
    int len = strlen(s), take = len <= VOTE_COLS ? len : VOTE_COLS;
    if (len > VOTE_COLS) { int sp = take; while (sp > 0 && s[sp] != ' ') sp--; if (sp > 0) take = sp; }
    char buf[VOTE_COLS + 1];
    memcpy(buf, s, take); buf[take] = 0;
    centred(88 + line * 22, buf, 2, RGB565_WHITE);
    s += take;
    line++;
  }

  int mine = voteOf(vShow, sosMe);
  if (mine < 0) {
    // Not answered yet: the answer buttons.
    for (int i = 0; i < p.nopt; i++) {
      int x, y;
      voteBtnRect(i, p.nopt, &x, &y);
      gfx->fillRoundRect(x, y, VBTN_W, VBTN_H, 16, VOTE_COL[i]);
      gfx->drawRoundRect(x, y, VBTN_W, VBTN_H, 16, RGB565_WHITE);
      int tw = strlen(p.opt[i]) * 12;
      gfx->setTextSize(2);
      gfx->setTextColor(RGB565_WHITE);
      gfx->setCursor(x + (VBTN_W - tw) / 2, y + (VBTN_H - 16) / 2);
      gfx->print(p.opt[i]);
    }
    votePill("LATER");
  } else {
    // Answered: live results. One row per answer - name, bar, count and who.
    int total = 0, count[VOTE_MAXOPT] = { 0 };
    for (int v = 0; v < SOS_N; v++) { int k = voteOf(vShow, v); if (k >= 0) { count[k]++; total++; } }
    for (int i = 0; i < p.nopt; i++) {
      int y = 200 + i * 44;
      char lab[24];
      snprintf(lab, sizeof(lab), "%s%s", i == mine ? "> " : "", p.opt[i]);
      gfx->setTextSize(2);
      gfx->setTextColor(i == mine ? RGB565(255, 220, 120) : RGB565(220, 220, 230));
      gfx->setCursor(78, y);
      gfx->print(lab);
      char who[12] = "";
      for (int v = 0; v < SOS_N; v++) if (voteOf(vShow, v) == i) { char c[3] = { SOS_NAME[v][0], ' ', 0 }; strcat(who, c); }
      gfx->setTextColor(RGB565(170, 170, 185));
      gfx->setCursor(388 - (int)strlen(who) * 12, y);
      gfx->print(who);
      gfx->fillRoundRect(78, y + 20, 310, 12, 6, RGB565(30, 34, 50));
      if (count[i]) gfx->fillRoundRect(78, y + 20, 310 * count[i] / SOS_N, 12, 6, VOTE_COL[i]);
    }
    // Who is still thinking.
    char wait[48] = "";
    for (int v = 0; v < SOS_N; v++)
      if (voteOf(vShow, v) < 0) { if (wait[0]) strcat(wait, ", "); strcat(wait, SOS_NAME[v]); }
    char status[64];
    if (wait[0]) snprintf(status, sizeof(status), "waiting for %s", wait);
    else         snprintf(status, sizeof(status), "everyone voted!");
    bool big = strlen(status) <= 28;                        // size 2 fits up to 28 characters here
    centred(VLATER_Y - (big ? 26 : 20), status, big ? 2 : 1, wait[0] ? RGB565(150, 150, 165) : RGB565(120, 230, 140));
    votePill("HOME");
  }
  gfx->flush();
}

// Called by showScreen(ST_VOTE) from sosPoll() when a question is waiting: pick it and ding.
// (Also reached when the screensaver redraws this screen on wake: then nothing new is waiting,
// or it is the same question, and the screen is just drawn again - no sound.)
void voteArrive() {
  int w = voteWaiting();
  if (w >= 0 && w != vShow) {
    vShow = w;
    sosPlay(SND_SWISH, SND_SWISH_LEN, 150);
    Serial.printf("vote   : asking for an answer to %s's question\n", SOS_NAME[vShow]);
  }
}

void voteTap(int x, int y) {
  bool onPill = x >= VLATER_X - 10 && x <= VLATER_X + VLATER_W + 10 && y >= VLATER_Y - 10 && y <= VLATER_Y + VLATER_H + 12;
  if (vShow < 0 || !vPoll[vShow].id[0]) { if (onPill) showScreen(ST_HOME); return; }
  VotePoll &p = vPoll[vShow];
  if (voteOf(vShow, sosMe) >= 0) {                          // results: HOME
    if (onPill) { vShow = -1; showScreen(ST_HOME); }
    return;
  }
  if (onPill) {                                             // LATER: not again until a restart
    snprintf(p.later, sizeof(p.later), "%s", p.id);
    Serial.printf("vote   : later for %s's question\n", SOS_NAME[vShow]);
    vShow = -1;
    showScreen(ST_HOME);
    return;
  }
  for (int i = 0; i < p.nopt; i++) {
    int bx, by;
    voteBtnRect(i, p.nopt, &bx, &by);
    if (x >= bx - 6 && x <= bx + VBTN_W + 6 && y >= by - 6 && y <= by + VBTN_H + 6) {
      char topic[64], msg[32];
      snprintf(topic, sizeof(topic), "sos/vote/%s/%s", SOS_ID[vShow], SOS_ID[sosMe]);
      snprintf(msg, sizeof(msg), "%s|%d", p.id, i);
      bool ok = sosnet_publish(topic, msg, true);
      // Count it at once; the broker echoes it back (we subscribe to sos/vote/#), which is harmless.
      snprintf(vVoteId[vShow][sosMe], sizeof(vVoteId[0][0]), "%s", p.id);
      vVote[vShow][sosMe] = i;
      Serial.printf("vote   : voted \"%s\" (%s)\n", p.opt[i], ok ? "sent" : "NOT queued");
      showVote();
      return;
    }
  }
}
