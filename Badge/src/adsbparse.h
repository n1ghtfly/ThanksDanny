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
  // v3.15, for the detail screen:
  char  hex[8];             // ICAO 24-bit address, e.g. "484b1d"
  char  reg[10];            // registration "r", e.g. "PH-BXA"
  char  desc[28];           // type description "desc", e.g. "BOEING 737-800"
  char  op[28];             // owner / operator "ownOp"
  char  squawk[5];
  int   vrate;              // feet per minute (baro_rate, else geom_rate); valid when has_vrate
  uint8_t has_vrate;
} AdsbPlane;

// Next byte of the HTTP body, or -1 at the end / on a timeout.
typedef int (*adsb_read_fn)(void *ctx);

// Parses the whole document. Stores up to `max` aircraft that have a position into `out`.
// Returns how many were stored, or -1 if the document is not the expected JSON.
int adsb_parse(adsb_read_fn rd, void *ctx, AdsbPlane *out, int max);

// v3.15: one string field from a small, flat JSON object inside a document, e.g. the adsbdb
// answer {"response":{"flightroute":{"origin":{"iata_code":"AMS",...},...}}}:
//   json_flat_field(body, "origin", "iata_code", out, n)  ->  "AMS"
// obj = NULL searches the whole document. Returns 1 if found with a non-null value, else 0
// (out is then ""). Assumes the object holds no nested objects, which is true for adsbdb.
int json_flat_field(const char *body, const char *obj, const char *field, char *out, int n);

#ifdef __cplusplus
}
#endif
