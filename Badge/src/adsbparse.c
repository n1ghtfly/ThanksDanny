// adsbparse - see adsbparse.h.
#include "adsbparse.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  adsb_read_fn rd;
  void *ctx;
  int back;                 // one byte of push-back, -2 = none
} P;

static int next(P *p) {
  if (p->back != -2) { int c = p->back; p->back = -2; return c; }
  return p->rd(p->ctx);
}
static void unread(P *p, int c) { p->back = c; }
static int ws(P *p) {       // next non-whitespace byte
  int c;
  do { c = next(p); } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
  return c;
}

// The opening quote has been read. Copies up to n-1 bytes (escapes kept as the escaped char).
static int str(P *p, char *out, int n) {
  int k = 0;
  for (;;) {
    int c = next(p);
    if (c < 0) return 0;
    if (c == '"') break;
    if (c == '\\') { c = next(p); if (c < 0) return 0; }
    if (out && k < n - 1) out[k++] = (char)c;
  }
  if (out && n > 0) out[k] = 0;
  return 1;
}

// A number / true / false / null whose first byte is c. Leaves the delimiter unread.
static int scalar(P *p, int c, char *out, int n) {
  int k = 0;
  while (c >= 0 && c != ',' && c != '}' && c != ']' && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
    if (k < n - 1) out[k++] = (char)c;
    c = next(p);
  }
  out[k] = 0;
  if (c < 0) return 0;
  unread(p, c);
  return 1;
}

// Skips any value whose first byte is c.
static int skip(P *p, int c) {
  if (c == '"') return str(p, 0, 0);
  if (c == '{' || c == '[') {
    int depth = 1;
    while (depth > 0) {
      c = next(p);
      if (c < 0) return 0;
      if (c == '"') { if (!str(p, 0, 0)) return 0; }
      else if (c == '{' || c == '[') depth++;
      else if (c == '}' || c == ']') depth--;
    }
    return 1;
  }
  char tmp[32];
  return scalar(p, c, tmp, sizeof(tmp));
}

static void cpy(char *dst, int n, const char *src) {   // copy, cut to fit
  int k = 0;
  while (src[k] && k < n - 1) { dst[k] = src[k]; k++; }
  dst[k] = 0;
}

static void trim(char *s) {
  int n = (int)strlen(s);
  while (n > 0 && s[n - 1] == ' ') s[--n] = 0;
}

// One aircraft object; the '{' has been read.
static int plane(P *p, AdsbPlane *a) {
  memset(a, 0, sizeof(*a));
  char hex[8] = "";
  int have_track = 0, have_heading = 0, have_gs = 0, have_baro = 0;
  float heading = 0, tas = 0;
  int alt_geom = 0, have_geom = 0;
  for (;;) {
    int c = ws(p);
    if (c == ',') c = ws(p);                        // between two fields
    if (c == '}') break;
    if (c != '"') return 0;
    char key[16];
    if (!str(p, key, sizeof(key))) return 0;
    if (ws(p) != ':') return 0;
    c = ws(p);
    if (c == '"') {
      char s[16];
      if (!str(p, s, sizeof(s))) return 0;
      if (!strcmp(key, "flight"))        { cpy(a->flight, sizeof(a->flight), s); trim(a->flight); }
      else if (!strcmp(key, "hex"))      { cpy(hex, sizeof(hex), s); }
      else if (!strcmp(key, "t"))        { cpy(a->type, sizeof(a->type), s); }
      else if (!strcmp(key, "alt_baro") && !strcmp(s, "ground")) a->ground = 1;
    } else if (c == '{' || c == '[') {
      if (!skip(p, c)) return 0;
    } else {
      char num[32];
      if (!scalar(p, c, num, sizeof(num))) return 0;
      if (num[0] == 'n' || num[0] == 't' || num[0] == 'f') continue;   // null / true / false
      float v = (float)atof(num);
      if (!strcmp(key, "lat"))               { a->lat = v; a->has_pos |= 1; }
      else if (!strcmp(key, "lon"))          { a->lon = v; a->has_pos |= 2; }
      else if (!strcmp(key, "track"))        { a->track = v; have_track = 1; }
      else if (!strcmp(key, "true_heading")) { heading = v; have_heading = 1; }
      else if (!strcmp(key, "gs"))           { a->gs = v; have_gs = 1; }
      else if (!strcmp(key, "tas"))          { tas = v; }
      else if (!strcmp(key, "alt_baro"))     { a->alt = (int)v; have_baro = 1; }
      else if (!strcmp(key, "alt_geom"))     { alt_geom = (int)v; have_geom = 1; }
      else if (!strcmp(key, "seen_pos"))     { a->pos_age = v; }
    }
  }
  a->has_pos = (a->has_pos == 3);
  if (!have_track && have_heading) { a->track = heading; have_track = 1; }
  a->has_track = (uint8_t)have_track;
  if (!have_gs) a->gs = tas;
  if (have_baro) a->has_alt = 1;
  else if (have_geom) { a->alt = alt_geom; a->has_alt = 1; }
  if (!a->flight[0]) cpy(a->flight, sizeof(a->flight), hex);
  if (a->pos_age < 0) a->pos_age = 0;
  if (a->pos_age > 30) a->pos_age = 30;
  return 1;
}

int adsb_parse(adsb_read_fn rd, void *ctx, AdsbPlane *out, int max) {
  P p = { rd, ctx, -2 };
  int n = 0;
  if (ws(&p) != '{') return -1;
  for (;;) {
    int c = ws(&p);
    if (c == '}') return n;
    if (c != '"') return -1;
    char key[16];
    if (!str(&p, key, sizeof(key))) return -1;
    if (ws(&p) != ':') return -1;
    c = ws(&p);
    if (!strcmp(key, "ac") && c == '[') {
      for (;;) {
        c = ws(&p);
        if (c == ']') break;
        if (c != '{') return -1;
        AdsbPlane a;
        if (!plane(&p, &a)) return -1;
        if (a.has_pos && n < max) out[n++] = a;
        c = ws(&p);
        if (c == ']') break;
        if (c != ',') return -1;
      }
    } else if (!skip(&p, c)) {
      return -1;
    }
    c = ws(&p);
    if (c == '}') return n;
    if (c != ',') return -1;
  }
}
