// adsbparse - a small streaming parser for adsb.fi's /api/v3 JSON (readsb "aircraft.json" format).
// Plain C, no allocation, no JSON library: it reads one byte at a time from a callback, keeps only
// the fields the radar draws, and skips everything else (nested arrays/objects included).
// Host-tested in tools/test_adsbparse.c. Used by Badge/Radar.ino.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float lat, lon;           // degrees
  float track;              // degrees, direction of travel (track, else true_heading)
  float gs;                 // ground speed, knots (gs, else tas)
  float pos_age;            // seconds since this position was measured (seen_pos)
  int   alt;                // feet (alt_baro, else alt_geom); valid when has_alt
  uint8_t has_pos, has_track, has_alt, ground;
  char  flight[9];          // callsign, trailing spaces trimmed (else the hex address)
  char  type[5];            // ICAO type, e.g. "B738"
} AdsbPlane;

// Next byte of the HTTP body, or -1 at the end / on a timeout.
typedef int (*adsb_read_fn)(void *ctx);

// Parses the whole document. Stores up to `max` aircraft that have a position into `out`.
// Returns how many were stored, or -1 if the document is not the expected JSON.
int adsb_parse(adsb_read_fn rd, void *ctx, AdsbPlane *out, int max);

#ifdef __cplusplus
}
#endif
