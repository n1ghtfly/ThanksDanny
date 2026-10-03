/*
 * AudioProbe — can this board play audio?
 * Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *
 * Answering that with evidence rather than a datasheet claim. The signal path here is
 * NOT the bare-I2S-DAC path used on the other boards:
 *
 *   ESP32-S3 I2S  ->  ES8311 codec @ 0x18 (must be configured over I2C)  ->  amp
 *                     whose enable is GPIO 46, HIGH to pass audio to the speaker
 *
 * So this sketch proves each stage in order and prints what it found:
 *   1. the codec answers on I2C at 0x18
 *   2. the I2S bus comes up (16 kHz, 16-bit, stereo - the vendor's own settings)
 *   3. the codec accepts its clock/volume configuration
 *   4. the amplifier is enabled
 *   5. four rising notes come out of the speaker
 *
 * es8311.c/es8311.h/es8311_reg.h are Waveshare's vendored copy of Espressif's ES8311
 * driver (examples/arduino/examples/07_ES8311 in waveshareteam/ESP32-S3-Touch-AMOLED-1.75C).
 * Keeping their driver rather than re-deriving the register sequence is deliberate.
 */

#include "Wire.h"
#include "ESP_I2S.h"
#include "es8311.h"
#include <math.h>

#define IIC_SDA   15
#define IIC_SCL   14
#define I2S_MCK   16
#define I2S_BCK   9
#define I2S_WS    45
#define I2S_DO    8
#define I2S_DI    10
#define PA_PIN    46

#define SAMPLE_RATE   16000
#define TONE_AMP      12000          // ~ -8 dBFS: clearly audible, not harsh
#define FADE_MS       12             // ramp in/out so notes do not click

static I2SClass   i2s;
static es8311_handle_t codec = nullptr;

// One note: a mono sine written as interleaved stereo, generated in small blocks so
// nothing large has to live in RAM.
void tone(float freq, int ms) {
  static int16_t block[256 * 2];
  const int total = SAMPLE_RATE * ms / 1000;
  const int fade  = SAMPLE_RATE * FADE_MS / 1000;

  for (int done = 0; done < total; done += 256) {
    int n = (total - done < 256) ? (total - done) : 256;
    for (int i = 0; i < n; i++) {
      int   k = done + i;
      float env = 1.0f;
      if (k < fade)                  env = (float)k / fade;
      else if (k > total - fade)     env = (float)(total - k) / fade;
      int16_t v = (int16_t)(sinf(2.0f * PI * freq * (float)k / SAMPLE_RATE) * TONE_AMP * env);
      block[i * 2]     = v;
      block[i * 2 + 1] = v;
    }
    i2s.write((uint8_t *)block, n * 4);
  }
  Serial.printf("  note %.0f Hz for %d ms\n", freq, ms);
}

bool configureCodec() {
  codec = es8311_create(0, ES8311_ADDRRES_0);
  if (!codec) { Serial.println("! es8311_create returned null"); return false; }

  es8311_clock_config_t clk = {};
  clk.mclk_inverted     = false;
  clk.sclk_inverted     = false;
  clk.mclk_from_mclk_pin = true;               // MCLK is wired on this board (GPIO 16)
  clk.mclk_frequency    = SAMPLE_RATE * 256;
  clk.sample_frequency  = SAMPLE_RATE;

  if (es8311_init(codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    Serial.println("! es8311_init failed"); return false;
  }
  if (es8311_sample_frequency_config(codec, clk.mclk_frequency, clk.sample_frequency) != ESP_OK) {
    Serial.println("! es8311_sample_frequency_config failed"); return false;
  }
  es8311_microphone_config(codec, false);      // playback only: leave the mics out of it
  int got = 0;
  if (es8311_voice_volume_set(codec, 85, &got) != ESP_OK) {
    Serial.println("! es8311_voice_volume_set failed"); return false;
  }
  Serial.printf("codec  : ES8311 configured, volume asked 85 got %d\n", got);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== AudioProbe: can this board play audio? ===");

  pinMode(PA_PIN, OUTPUT);
  digitalWrite(PA_PIN, LOW);                   // silent until everything is ready
  Serial.printf("amp    : PA GPIO %d parked LOW\n", PA_PIN);

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  Wire.beginTransmission(ES8311_ADDRRES_0);
  bool present = (Wire.endTransmission() == 0);
  Serial.printf("codec  : ES8311 @ 0x%02X %s\n", ES8311_ADDRRES_0,
                present ? "answers on I2C" : "NOT FOUND - stopping here");
  if (!present) return;

  i2s.setPins(I2S_BCK, I2S_WS, I2S_DO, I2S_DI, I2S_MCK);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("! i2s.begin failed - bus did not come up");
    return;
  }
  Serial.printf("i2s    : up, %d Hz 16-bit stereo (BCK %d, WS %d, DO %d, DI %d, MCK %d)\n",
                SAMPLE_RATE, I2S_BCK, I2S_WS, I2S_DO, I2S_DI, I2S_MCK);

  if (!configureCodec()) return;

  digitalWrite(PA_PIN, HIGH);
  Serial.println("amp    : PA HIGH - the speaker is live now");
  delay(250);

  Serial.println("tone   : four rising notes, A4 C5 E5 A5");
  tone(440.0f, 400);
  tone(523.25f, 400);
  tone(659.25f, 400);
  tone(880.0f, 700);
  Serial.println("tone   : done");

  digitalWrite(PA_PIN, LOW);
  Serial.println("amp    : PA LOW again, so nothing hums while idle");
  Serial.println("result : if you heard four rising notes, audio on this board works");
  Serial.println("ready");
}

void loop() {
  delay(1000);
}
