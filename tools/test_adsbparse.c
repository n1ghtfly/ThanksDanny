// Host test for Badge/src/adsbparse.c:  gcc -I../Badge/src test_adsbparse.c ../Badge/src/adsbparse.c && ./a.out
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "adsbparse.h"

typedef struct { const char *s; int i; } Src;
static int rd(void *ctx) { Src *x = ctx; return x->s[x->i] ? (unsigned char)x->s[x->i++] : -1; }
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main(void) {
  const char *doc =
    "{\"ac\":[\n"
    " {\"hex\":\"484b1d\",\"type\":\"adsb_icao\",\"flight\":\"KLM1234 \",\"r\":\"PH-BXA\",\"t\":\"B738\",\"desc\":\"BOEING 737-800\",\"ownOp\":\"KLM ROYAL DUTCH AIRLINES\","
    "  \"alt_baro\":35000,\"alt_geom\":35750,\"gs\":452.3,\"track\":123.45,\"baro_rate\":-64,\"geom_rate\":-32,"
    "  \"squawk\":\"1234\",\"emergency\":\"none\",\"category\":\"A3\",\"nav_qnh\":1013.6,"
    "  \"nav_modes\":[\"autopilot\",\"tcas\"],\"lat\":52.312345,\"lon\":4.765432,\"nic\":8,"
    "  \"seen_pos\":0.4,\"mlat\":[],\"tisb\":[],\"messages\":12345,\"seen\":0.1,\"rssi\":-20.1},\n"
    " {\"hex\":\"4ca7b5\",\"flight\":\"RYR7AB  \",\"t\":\"B38M\",\"alt_baro\":\"ground\",\"gs\":12,"
    "  \"true_heading\":270.0,\"lat\":52.30,\"lon\":4.76,\"seen_pos\":45.0},\n"
    " {\"hex\":\"3c6444\",\"t\":\"A320\",\"alt_geom\":12000,\"tas\":300,\"true_heading\":90,"
    "  \"lat\":52.4,\"lon\":5.0e0,\"lastPosition\":{\"lat\":1,\"lon\":2,\"seen_pos\":9},\"note\":\"a \\\"quoted\\\" } ] thing\"},\n"
    " {\"hex\":\"abcdef\",\"flight\":\"NOPOS\",\"alt_baro\":1000},\n"
    " {\"hex\":\"~123456\",\"flight\":null,\"lat\":-33.5,\"lon\":-70.25,\"gs\":0}\n"
    "],\"msg\":\"No error\",\"now\":1792349334262,\"total\":5,\"ctime\":1792349334262,\"ptime\":3}";
  AdsbPlane a[8];
  Src s = { doc, 0 };
  int n = adsb_parse(rd, &s, a, 8);
  printf("parsed %d aircraft\n", n);
  CHECK(n == 4);                                     // the one without a position is dropped
  CHECK(!strcmp(a[0].flight, "KLM1234"));            // trailing space trimmed
  CHECK(!strcmp(a[0].type, "B738"));
  CHECK(a[0].alt == 35000 && a[0].has_alt);          // alt_baro preferred over alt_geom
  CHECK(fabsf(a[0].track - 123.45f) < 0.01f && a[0].has_track);
  CHECK(fabsf(a[0].gs - 452.3f) < 0.01f);
  CHECK(fabsf(a[0].lat - 52.312345f) < 1e-5f && fabsf(a[0].lon - 4.765432f) < 1e-5f);
  CHECK(!a[0].ground);
  CHECK(!strcmp(a[0].hex, "484b1d") && !strcmp(a[0].reg, "PH-BXA"));
  CHECK(!strcmp(a[0].desc, "BOEING 737-800"));
  CHECK(!strcmp(a[0].op, "KLM ROYAL DUTCH AIRLINES"));    // 24 chars: fits op[28]
  CHECK(!strcmp(a[0].squawk, "1234"));
  CHECK(a[0].has_vrate && a[0].vrate == -64);           // baro_rate wins over geom_rate
  CHECK(!a[1].has_vrate && a[1].reg[0] == 0);
  CHECK(a[1].ground && !a[1].has_alt);               // "ground" flagged
  CHECK(fabsf(a[1].track - 270.0f) < 0.01f);         // true_heading used when no track
  CHECK(fabsf(a[1].pos_age - 30.0f) < 0.01f);        // stale fix capped at 30 s
  CHECK(!strcmp(a[2].flight, "3c6444"));             // no callsign -> hex
  CHECK(a[2].alt == 12000 && fabsf(a[2].gs - 300) < 0.01f);   // alt_geom / tas fallbacks
  CHECK(fabsf(a[2].lat - 52.4f) < 1e-5f && fabsf(a[2].lon - 5.0f) < 1e-5f);  // nested object skipped
  CHECK(!strcmp(a[3].flight, "~123456") && fabsf(a[3].lon + 70.25f) < 1e-4f);  // null handled
  // limit respected
  s.i = 0; n = adsb_parse(rd, &s, a, 2); CHECK(n == 2);
  // empty list / error documents
  Src e = { "{\"ac\":[],\"msg\":\"No error\"}", 0 }; CHECK(adsb_parse(rd, &e, a, 8) == 0);
  Src e2 = { "{\"msg\":\"Too many requests\"}", 0 }; CHECK(adsb_parse(rd, &e2, a, 8) == 0);
  Src bad = { "<html>502 Bad Gateway</html>", 0 }; CHECK(adsb_parse(rd, &bad, a, 8) == -1);
  Src cut = { "{\"ac\":[{\"hex\":\"1\",\"lat\":5", 0 }; CHECK(adsb_parse(rd, &cut, a, 8) == -1);
  // json_flat_field on adsbdb-style answers (v3.15)
  const char *route = "{\"response\":{\"flightroute\":{\"callsign\":\"EZY85KP\",\"airline\":{\"name\":\"easyJet\","
    "\"icao\":\"EZY\",\"callsign\":\"EASY\"},\"origin\":{\"country_name\":\"Netherlands\",\"elevation\":-11,"
    "\"iata_code\":\"AMS\",\"icao_code\":\"EHAM\",\"municipality\":\"Amsterdam\",\"name\":\"Amsterdam Airport Schiphol\"},"
    "\"destination\":{\"iata_code\":\"LGW\",\"icao_code\":\"EGKK\",\"municipality\":\"London\",\"name\":\"London Gatwick\"}}}}";
  char v[40];
  CHECK(json_flat_field(route, "origin", "iata_code", v, sizeof(v)) && !strcmp(v, "AMS"));
  CHECK(json_flat_field(route, "destination", "municipality", v, sizeof(v)) && !strcmp(v, "London"));
  CHECK(json_flat_field(route, "airline", "name", v, sizeof(v)) && !strcmp(v, "easyJet"));
  CHECK(json_flat_field(route, "airline", "callsign", v, sizeof(v)) && !strcmp(v, "EASY"));   // not flightroute's
  CHECK(!json_flat_field(route, "origin", "nothere", v, sizeof(v)) && v[0] == 0);
  CHECK(json_flat_field(route, "origin", "elevation", v, sizeof(v)) && !strcmp(v, "-11"));
  const char *ac = "{\"response\":{\"aircraft\":{\"type\":\"737NG 8K2/W\",\"icao_type\":\"B738\",\"manufacturer\":\"Boeing\","
    "\"registration\":\"PH-BXA\",\"registered_owner_country_name\":\"Netherlands\",\"registered_owner\":\"KLM\",\"url_photo\":null}}}";
  CHECK(json_flat_field(ac, "aircraft", "registered_owner", v, sizeof(v)) && !strcmp(v, "KLM"));   // exact key
  CHECK(json_flat_field(ac, "aircraft", "manufacturer", v, sizeof(v)) && !strcmp(v, "Boeing"));
  CHECK(!json_flat_field(ac, "aircraft", "url_photo", v, sizeof(v)));                              // null
  CHECK(!json_flat_field("{\"response\":\"unknown callsign\"}", "flightroute", "callsign", v, sizeof(v)));
  CHECK(json_flat_field(route, "origin", "name", v, 8) && !strcmp(v, "Amsterd"));                  // cut to fit
  printf(fails ? "%d FAILURE(S)\n" : "all checks passed\n", fails);
  return fails != 0;
}
