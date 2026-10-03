#include <string.h>
#include "mp3stream.h"
#include "helix/mp3dec.h"

#define INBUF 4096

static unsigned char s_in[INBUF];
static short s_pcm[MAX_NCHAN * MAX_NGRAN * MAX_NSAMP];

// Move the unread bytes to the front of the buffer and read more behind them.
// Returns the new number of valid bytes; sets *eof when the source is exhausted.
static int refill(mp3_read_fn rd, void *ctx, unsigned char **p, int have, int *eof) {
  if (*p != s_in && have > 0) memmove(s_in, *p, have);
  *p = s_in;
  if (*eof) return have;
  int n = rd(ctx, s_in + have, INBUF - have);
  if (n <= 0) *eof = 1; else have += n;
  return have;
}

mp3_result mp3_stream(mp3_read_fn rd, mp3_pcm_fn out, void *ctx) {
  mp3_result r;
  memset(&r, 0, sizeof(r));
  HMP3Decoder dec = MP3InitDecoder();
  if (!dec) { r.error = -4; return r; }

  unsigned char *p = s_in;
  int have = 0, eof = 0;

  // Skip an ID3v2 tag at the start instead of scanning through it: tags can be large (cover
  // art, or the C2PA content-credentials block some tools attach) and their bytes can look like
  // MP3 frame headers. Header: "ID3", version(2), flags(1), size(4 x 7-bit "syncsafe").
  have = refill(rd, ctx, &p, have, &eof);
  if (have >= 10 && p[0] == 'I' && p[1] == 'D' && p[2] == '3') {
    long tag = 10 + (((long)p[6] & 0x7f) << 21 | ((long)p[7] & 0x7f) << 14 |
                     ((long)p[8] & 0x7f) << 7 | ((long)p[9] & 0x7f));
    if (p[5] & 0x10) tag += 10;                  // footer present
    r.tagBytes = (int)tag;
    while (tag > 0 && have > 0) {
      int k = tag < have ? (int)tag : have;
      p += k; have -= k; tag -= k;
      if (have == 0) have = refill(rd, ctx, &p, have, &eof);
    }
  }

  for (;;) {
    if (have < INBUF / 2) have = refill(rd, ctx, &p, have, &eof);
    if (have <= 0) break;

    int off = MP3FindSyncWord(p, have);
    if (off < 0) {                        // no frame header anywhere in the buffer
      if (eof) break;
      r.skipped += have > 3 ? have - 3 : 0;
      p += have > 3 ? have - 3 : 0;       // keep 3 bytes: a header may straddle the refill
      have = have > 3 ? 3 : have;
      have = refill(rd, ctx, &p, have, &eof);
      continue;
    }
    p += off; have -= off;

    unsigned char *start = p;
    int left = have;
    int err = MP3Decode(dec, &p, &left, s_pcm, 0);
    if (err == ERR_MP3_NONE) {
      have = left;
      MP3FrameInfo fi;
      MP3GetLastFrameInfo(dec, &fi);
      r.frames++;
      r.rate = fi.samprate;
      r.channels = fi.nChans;
      int perCh = fi.outputSamps / (fi.nChans > 0 ? fi.nChans : 1);
      if (!out(ctx, s_pcm, perCh, fi.nChans, fi.samprate)) { r.stopped = 1; break; }
    } else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
      // The frame was consumed into the bit reservoir but gives no audio yet (normal for the
      // first frame or two). Helix has already advanced p/left past it.
      have = left;
    } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
      // Not enough bytes for this frame: nothing consumed. Get more, or stop at the end.
      p = start;
      if (eof) break;
      have = refill(rd, ctx, &p, have, &eof);
    } else {
      // Corrupt frame: step past this sync word and look for the next one.
      p = start + 1; have -= 1;
      r.skipped++;
    }
  }
  MP3FreeDecoder(dec);
  return r;
}
