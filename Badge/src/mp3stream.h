// mp3stream - stream an MP3 through the Helix fixed-point decoder (src/helix, RealNetworks
// RPSL/RCSL, see src/helix/LICENSE.txt). Plain C with callbacks, so the same loop runs on the
// badge (LittleFS file -> I2S) and in the PC-side test that checked it against ffmpeg.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Fill buf with up to n bytes; return the count, 0 at end of file.
typedef int (*mp3_read_fn)(void *ctx, unsigned char *buf, int n);
// One decoded frame: `samples` per channel, interleaved if channels == 2. Return 0 to stop.
typedef int (*mp3_pcm_fn)(void *ctx, const short *pcm, int samples, int channels, int rate);

typedef struct {
  int frames;       // frames decoded
  int skipped;      // bytes skipped while resyncing after a bad frame
  int rate;         // sample rate of the last frame
  int channels;     // channels of the last frame
  int stopped;      // 1 if the pcm callback asked to stop
  int error;        // 0, or -4 if the decoder could not be allocated
  int tagBytes;     // size of an ID3v2 tag skipped at the start (0 if none)
} mp3_result;

// Decode the whole stream. Allocates the decoder (~25 kB) for the call and frees it after.
mp3_result mp3_stream(mp3_read_fn rd, mp3_pcm_fn out, void *ctx);

#ifdef __cplusplus
}
#endif
