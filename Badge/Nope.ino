/*
 * Nope (v3.7, v3.8) - Danny's official reasons for saying no (and, since v3.8, for saying yes).
 *
 * When the Decider lands on DISAPPROVED, a card slides over the bottom of the badge:
 *   OFFICIAL REASON:
 *   My energy is solar-powered, and it's nighttime.
 * The reasons are No-as-a-Service's list (github.com/hotheadhacker/no-as-a-service, MIT), copied
 * once by tools/prep_no_reasons.py onto our GitHub Pages site as no/reasons.txt. The badge keeps
 * a copy in flash (/no/reasons.txt), refreshed by the background update at start-up only when
 * no/index.txt (the file's byte size) says it changed - so a reason costs no network at all and
 * works offline. Until the first download, a few built-in reasons stand in.
 *
 * Picking one: seek to a random byte, skip to the start of the next line, read that line. No
 * index and no line count needed for a ~56 kB file; lines after long lines are a little more
 * likely, which nobody will ever notice.
 */

#define NOPE_DIR   "/no"
#define NOPE_FILE  "/no/reasons.txt"
#define NOPE_COLS  24                     // must match COLS / ROWS in tools/prep_no_reasons.py
#define NOPE_ROWS  4

static const char *NOPE_BUILTIN[] = {     // from the same list, for before the first download
  "My energy is solar-powered, and it's nighttime.",
  "My schedule is in airplane mode.",
  "I've been preparing my whole life to say no to this exact request.",
  "The dress code for that sounds like 'effort', so I'm out.",
  "My calendar at work has a big red 'NOPE' on that date.",
};

// Fetch no/reasons.txt when the site's copy differs from ours. Runs in the background update.
void nopeSync() {
  if (WiFi.status() != WL_CONNECTED) return;
  int code = 0;
  String idx = httpGetToString(String(PIC_HOST) + "/no/index.txt", &code);
  long want = idx.toInt();
  if (code != 200 || want <= 0) {
    Serial.printf("nope   : index -> HTTP %d, keeping what we have\n", code);
    return;
  }
  File f = LittleFS.open(NOPE_FILE, FILE_READ);
  long have = f ? (long)f.size() : -1;
  if (f) f.close();
  if (have == want) { Serial.printf("nope   : reasons up to date (%ld bytes)\n", have); return; }
  if (!LittleFS.exists(NOPE_DIR)) LittleFS.mkdir(NOPE_DIR);
  uint32_t got = 0;
  bool bundle = true;
  int c = httpGetToFile(String(PIC_HOST) + "/no/reasons.txt", NOPE_FILE, &got, &bundle);
  File g = LittleFS.open(NOPE_FILE, FILE_READ);
  long now = g ? (long)g.size() : -1;
  if (g) g.close();
  Serial.printf("nope   : reasons.txt -> HTTP %d, %ld of %ld bytes\n", c, now, want);
  if (now != want) LittleFS.remove(NOPE_FILE);     // a half file would give half reasons
}

// One random reason into out (always something, even with no file).
void nopeReason(char *out, int n) {
  out[0] = 0;
  File f = LittleFS.open(NOPE_FILE, FILE_READ);
  if (f && f.size() > 64) {
    size_t size = f.size();
    for (int tries = 0; tries < 6 && !out[0]; tries++) {
      f.seek(esp_random() % size);
      f.readStringUntil('\n');                       // the rest of the line we landed in
      if (!f.available()) f.seek(0);                 // ran off the end: wrap to the top
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() && line[0] != '#') snprintf(out, n, "%s", line.c_str());
    }
  }
  if (f) f.close();
  if (!out[0]) {
    const int k = sizeof(NOPE_BUILTIN) / sizeof(NOPE_BUILTIN[0]);
    snprintf(out, n, "%s", NOPE_BUILTIN[esp_random() % k]);
  }
}

static const char *YES_REASONS[] = {     // your list (v3.8): shown when Danny approves
  "Consider me fully on board.",
  "I was practically waiting for you to ask - count me in.",
  "My inner peace committee reviewed the proposal and approved it unanimously.",
  "Future Me just sent a thank-you note for saying yes to this.",
  "Every fiber of my being just gave a standing ovation.",
  "Saying yes to this is the easiest decision I'll make all week.",
  "You had me at hello, but the rest sounds fantastic too.",
  "I just checked my calendar and cleared a runway specifically for this.",
  "My enthusiasm is 100% genuine and my availability is completely real.",
  "The stars, my schedule, and my mood have aligned: absolutely.",
  "This passes the vibe check with flying colors.",
  "I would climb Mount Everest barefoot to be part of this.",
  "Sign me up before you change your mind.",
  "If saying yes is wrong, I don't want to be right.",
  "I'm all in - hook, line, and sinker.",
  "My couch gave me permission to leave just for this.",
  "I ran the numbers and saying yes is mathematically optimal.",
  "My gut feeling just did a celebratory dance: let's do it.",
  "This is precisely what I needed on my plate today.",
  "I've got both hands raised and I'm ready to roll.",
  "Wild horses couldn't drag me away from this opportunity.",
  "A resounding, enthusiastic, unhesitating yes from my end.",
  "Consider your request granted with zero fine print.",
  "My Wi-Fi of motivation just hit gigabit speeds for this.",
  "I'm in, and I brought snacks.",
  "This aligns perfectly with where I want to spend my energy.",
  "Saying yes to this just cured my fatigue.",
  "I couldn't say no to this even if I practiced in the mirror.",
  "My calendar just high-fived me for putting this on it.",
  "I'm already mentally at the starting line.",
  "You didn't even have to finish the sentence - I'm down.",
  "Let's make it happen.",
  "The multiverse converged on a timeline where I say an emphatic yes.",
  "I'm volunteering as tribute - gladly.",
  "My response is freshly baked, warm, and a solid yes.",
  "I'm putting on real pants for this; that's how committed I am.",
  "Count me present, accounted for, and ready.",
  "This is a 10/10 idea and I want front-row seats.",
  "My to-do list just welcomed this with open arms.",
  "Yes, without an ounce of hesitation or regret.",
};
#define YES_N (int)(sizeof(YES_REASONS) / sizeof(YES_REASONS[0]))

// The card over the bottom of the result badge (Decider.ino calls this before the pill): an
// excuse in red on DISAPPROVED, one of YES_REASONS in green on APPROVED.
// Kept inside the round glass: the corners at x 80/386, y 404 are 229 px from the centre.
#define NOPE_X 80
#define NOPE_Y 294
#define NOPE_W 306
#define NOPE_H 110
void nopeDrawCard(bool approved) {
  char reason[160];
  if (approved) snprintf(reason, sizeof(reason), "%s", YES_REASONS[esp_random() % YES_N]);
  else          nopeReason(reason, sizeof(reason));
  const uint16_t *badge = approved ? DEC_POS : DEC_NEG;
  const uint16_t edge = approved ? RGB565(60, 200, 90) : RGB565(230, 60, 60);
  const uint16_t head = approved ? RGB565(110, 235, 130) : RGB565(255, 90, 90);

  // Word-wrap to NOPE_COLS (the prep script only keeps reasons that fit in NOPE_ROWS lines).
  char lines[NOPE_ROWS][NOPE_COLS + 1];
  int nl = 0;
  const char *p = reason;
  while (*p && nl < NOPE_ROWS) {
    while (*p == ' ') p++;
    int len = strlen(p);
    int take = len <= NOPE_COLS ? len : NOPE_COLS;
    if (len > NOPE_COLS) {                           // break at the last space that fits
      int sp = take;
      while (sp > 0 && p[sp] != ' ') sp--;
      if (sp > 0) take = sp;
    }
    memcpy(lines[nl], p, take);
    lines[nl][take] = 0;
    nl++;
    p += take;
  }
  if (*p && nl == NOPE_ROWS) {                       // longer than planned: end with "..."
    int l = strlen(lines[NOPE_ROWS - 1]);
    if (l > NOPE_COLS - 3) l = NOPE_COLS - 3;
    strcpy(lines[NOPE_ROWS - 1] + l, "...");
  }

  // Slide the card up from the bottom edge (it covers the thumb and QUALITY CONTROL, not Danny).
  const int frames = 6;
  for (int f = 1; f <= frames; f++) {
    float t = (float)f / frames;
    int y = NOPE_Y + (int)((1.0f - t * (2.0f - t)) * (LCD_HEIGHT - NOPE_Y));   // ease out
    memcpy(gfx->getFramebuffer() + (size_t)NOPE_Y * LCD_WIDTH, badge + (size_t)NOPE_Y * LCD_WIDTH,
           (size_t)(LCD_HEIGHT - NOPE_Y) * LCD_WIDTH * 2);                       // badge underneath
    gfx->fillRoundRect(NOPE_X, y, NOPE_W, NOPE_H, 16, RGB565(12, 16, 34));
    gfx->drawRoundRect(NOPE_X, y, NOPE_W, NOPE_H, 16, edge);
    gfx->drawRoundRect(NOPE_X + 1, y + 1, NOPE_W - 2, NOPE_H - 2, 15, edge);
    centred(y + 8, "OFFICIAL REASON:", 2, head);
    int ty = y + 30 + (NOPE_ROWS - nl) * 9;          // centre short reasons vertically
    for (int i = 0; i < nl; i++) centred(ty + i * 19, lines[i], 2, RGB565_WHITE);
    flushRect(0, NOPE_Y, LCD_WIDTH, LCD_HEIGHT - NOPE_Y);
  }
  Serial.printf("ask    : official reason (%s) - %s\n", approved ? "yes" : "no", reason);
}
