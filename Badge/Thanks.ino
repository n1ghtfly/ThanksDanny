/*
 * Thanks.ino (v3.23) - clap detection: clap once and the badge says "Thanks Danny!"
 *
 * Microphones: the board's two MEMS mics go through an ES7210 4-channel ADC (I2C 0x40, same bus as
 * touch / IMU / ES8311). Its data comes in on GPIO 10, the DIN of the I2S port the Decider already
 * opens for the speaker (decI2s: same BCLK 9 / WS 45 / MCLK 16, 16 kHz stereo 16-bit), so the
 * microphones need no new pins - they only need the ES7210 set up and someone reading the RX side.
 * The ES7210 driver in src/ is Espressif's, from Waveshare's 06_ES7210 example for this board.
 *
 * Detector (a FreeRTOS task on core 0, so loop() and the touch polling never wait for it):
 *   - reads 8 ms blocks, takes peak and RMS over both mics;
 *   - keeps a slow running average of the room noise ("floor");
 *   - a block much louder than the floor AND than the block before it is an ONSET (claps start
 *     within a millisecond; voices and music swell);
 *   - the onset must also be BRIGHT: a clap is a broadband crack, so the sample-to-sample
 *     difference is about as big as the signal itself (ratio ~1.4); voiced speech is low-pitched
 *     and smooth (~0.3-0.5). That one test is what keeps "Ha!" and "stop!" from counting;
 *   - the onset is only a clap if the sound has died away again within CLAP_DECAY_MS (a door or a
 *     shout keeps going). Then CLAP_COOLDOWN_MS of deafness.
 *   - v3.24: it does not answer the first clap straight away. People clap several times, and the
 *     voice was lost in the applause - so claps are counted, and the badge answers once it has been
 *     quiet for CLAP_QUIET_MS (nothing louder than 4x the room noise);
 *   - while one of the badge's own sounds plays (Decider / Sos / Rumours busy flags) and for a moment after,
 *     everything is ignored, so the badge does not answer itself.
 * The task only raises a flag; thanksTick() in loop() does the talking and the drawing.
 *
 * Serial: "clap   :" lines - the noise floor every minute, each candidate with its numbers and why
 * it was or was not a clap. Tune CLAP_MIN_PEAK / CLAP_RATIO from those if it is too eager or deaf.
 *
 * Sorts after Sos.ino, so it can use decI2s / sndBusy (Decider.ino), rumBusy (Rumours.ino) and
 * sosPlay / sosMuted / sosSndBusy (Sos.ino).
 */

#include "thanks_sound.h"
#include "src/es7210.h"

#define CLAP_RATE        16000
#define CLAP_BLOCK       128                  // frames per block = 8 ms
#define CLAP_MIN_PEAK    5000                 // absolute: a block must peak above this (of 32767)
#define CLAP_RATIO       10.0f                // ... and its RMS be this many times the room noise
#define CLAP_ATTACK      4.0f                 // ... and this many times the block before it
#define CLAP_DECAY_MS    100                  // must have died down to CLAP_DECAY_LVL by then
#define CLAP_HF_MIN      0.6f                 // brightness: rms(diff) / rms - claps ~1.4, voices < 0.5
#define CLAP_DECAY_LVL   0.25f                // of the onset block's RMS
#define CLAP_COOLDOWN_MS 2000                // after answering
#define CLAP_GAP_MS      120                  // between claps of one round of applause
#define CLAP_QUIET_MS    700                  // v3.24: answer once it has been quiet this long ...
#define CLAP_ROUND_MAX   6000                 // ... or at the latest this long after the first clap
#define CLAP_SPK_GUARD   500                  // deaf this long after the speaker switches off
#define CLAP_MIC_GAIN    GAIN_24DB
#define THANKS_SHOW_MS   1800                 // the splash stays up this long
#define THANKS_GAIN      256                  // voice volume, 256 = full scale

static volatile bool clapPending = false;
static volatile uint32_t clapCount = 0;
static volatile int clapRoundSize = 0;       // claps in the round that was just answered
static bool clapReady = false;
static uint32_t thanksUntil = 0;              // splash on screen until (0 = no splash)

// ---------------------------------------------------------------- microphones

static bool clapMicInit() {
  if (!decAudioInit()) { Serial.println("clap   : no audio path - clap detection off"); return false; }
  Wire.beginTransmission(ES7210_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.printf("clap   : ES7210 not answering at 0x%02X - clap detection off\n", ES7210_ADDR);
    return false;
  }
  audio_hal_codec_config_t cfg = {};
  cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
  cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
  cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;            // the ESP32 drives the clocks
  cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
  cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_16BITS;
  esp_err_t r = ESP_OK;
  r |= es7210_adc_init(&Wire, &cfg);
  r |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  r |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2), (es7210_gain_value_t)CLAP_MIC_GAIN);
  r |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4), (es7210_gain_value_t)CLAP_MIC_GAIN);
  r |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);
  if (r != ESP_OK) { Serial.println("clap   : ES7210 setup failed - clap detection off"); return false; }
  Serial.printf("clap   : ES7210 ready at 0x%02X, mic gain %d dB\n", ES7210_ADDR, CLAP_MIC_GAIN * 3);
  return true;
}

// ---------------------------------------------------------------- detector task (core 0)

void clapTask(void *arg) {
  static int16_t buf[CLAP_BLOCK * 2];
  float floorRms = 200, prevRms = 200;
  uint32_t deafUntil = 0, lastLog = 0, blocks = 0, silentBlocks = 0;
  bool inEvent = false;
  float onsetRms = 0;
  int onsetPeak = 0;
  float onsetHf = 0;
  uint32_t onsetAt = 0;
  int roundClaps = 0;                       // v3.24: claps heard, not answered yet
  uint32_t roundStart = 0, lastLoud = 0;
  for (;;) {
    size_t got = decI2s.readBytes((char *)buf, sizeof(buf));
    uint32_t now = millis();
    int n = got / 2;
    if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    int peak = 0;
    float sum = 0, dsum = 0;
    for (int i = 0; i < n; i++) {
      int v = buf[i]; if (v < 0) v = -v;
      if (v > peak) peak = v;
      sum += (float)buf[i] * buf[i];
      if (i >= 2) { float d = (float)buf[i] - buf[i - 2]; dsum += d * d; }   // same mic (stereo)
    }
    float rms = sqrtf(sum / n);
    float hf = rms > 1 ? sqrtf(dsum / (n - 2)) / rms : 0;
    if (peak == 0) silentBlocks++;
    blocks++;

    // v3.24: the applause is over (or has gone on long enough) - now answer it.
    if (rms > floorRms * 4) lastLoud = now;
    if (roundClaps && (now - lastLoud >= CLAP_QUIET_MS || now - roundStart >= CLAP_ROUND_MAX)) {
      clapRoundSize = roundClaps;
      clapPending = true;
      deafUntil = now + CLAP_COOLDOWN_MS;
      printf("clap   : quiet again after %d clap(s) in %lu ms - answering\n", roundClaps,
             (unsigned long)(now - roundStart));
      roundClaps = 0;
    }
    if (sndBusy || sosSndBusy || rumBusy) deafUntil = now + CLAP_SPK_GUARD;   // our own speaker is playing
    if ((int32_t)(now - deafUntil) < 0) { inEvent = false; prevRms = rms; continue; }

    if (inEvent) {
      if (rms < onsetRms * CLAP_DECAY_LVL) {                         // died away: a clap
        inEvent = false;
        clapCount++;
        if (!roundClaps) roundStart = now;
        roundClaps++;
        lastLoud = now;
        deafUntil = now + CLAP_GAP_MS;                             // listen for the next one
        printf("clap   : CLAP #%lu - peak %d, rms %.0f (floor %.0f, x%.0f), brightness %.2f, gone in %lu ms\n",
               (unsigned long)clapCount, onsetPeak, onsetRms, floorRms, onsetRms / floorRms, onsetHf,
               (unsigned long)(now - onsetAt));
      } else if (now - onsetAt > CLAP_DECAY_MS) {                    // still loud: not a clap
        inEvent = false;
        deafUntil = now + 300;
        printf("clap   : ignored - loud for over %d ms (peak %d, rms %.0f, floor %.0f)\n",
               CLAP_DECAY_MS, onsetPeak, onsetRms, floorRms);
      }
    } else if (peak > CLAP_MIN_PEAK && rms > floorRms * CLAP_RATIO && rms > prevRms * CLAP_ATTACK) {
      if (hf >= CLAP_HF_MIN) {
        inEvent = true;
        onsetRms = rms; onsetPeak = peak; onsetAt = now; onsetHf = hf;
      } else {
        printf("clap   : ignored - too dull for a clap (brightness %.2f < %.2f, peak %d)\n", hf, CLAP_HF_MIN, peak);
        deafUntil = now + 300;
      }
    } else if (peak > CLAP_MIN_PEAK && rms > floorRms * CLAP_RATIO) {
      // loud but no sharp start (a voice getting louder) - nothing, and not part of the floor
    } else {
      floorRms = floorRms * 0.99f + rms * 0.01f;                      // ~1 s average of the room
      if (floorRms < 20) floorRms = 20;
    }
    prevRms = rms;

    if (now - lastLog > 60000) {
      lastLog = now;
      printf("clap   : listening - room noise rms %.0f, %lu blocks%s\n", floorRms, (unsigned long)blocks,
             silentBlocks == blocks ? " (ALL SILENT - mic data not arriving?)" : "");
    }
  }
}

// setup(): mics on and the detector running. Failures leave the badge as it was.
void thanksStart() {
  if (!clapMicInit()) return;
  if (xTaskCreatePinnedToCore(clapTask, "clap", 4096, nullptr, 2, nullptr, 0) != pdPASS) {
    Serial.println("clap   : could not start the detector task");
    return;
  }
  clapReady = true;
  Serial.printf("clap   : listening - clap for \"Thanks Danny!\" (said once the applause stops; %.2f s of voice)\n",
                SND_THANKS_LEN / (float)CLAP_RATE);
}

// ---------------------------------------------------------------- answer (loop)

static void thanksSplash() {
  gfx->fillScreen(RGB565(6, 4, 20));
  for (int i = 0; i < 90; i++) {                                   // confetti
    float a = (esp_random() % 6283) / 1000.0f;
    int r = 206 + esp_random() % 24;                                 // the rim outside the ring
    int x = 233 + (int)(cosf(a) * r), y = 233 + (int)(sinf(a) * r);
    static const uint16_t C[] = { RGB565(255, 210, 60), RGB565(90, 200, 255), RGB565(255, 90, 140),
                                  RGB565(120, 255, 140), RGB565(255, 255, 255) };
    gfx->fillRect(x, y, 4 + esp_random() % 5, 4 + esp_random() % 5, C[esp_random() % 5]);
  }
  gfx->drawCircle(233, 233, 200, RGB565(255, 210, 60));            // text is ~324 px wide: needs r >= 182
  gfx->drawCircle(233, 233, 199, RGB565(160, 120, 30));
  gfx->setFont(&UiTitle);
  gfx->setTextSize(2);
  const char *L[2] = { "THANKS", "DANNY!" };
  for (int i = 0; i < 2; i++) {
    int16_t x1, y1; uint16_t w, h;
    gfx->getTextBounds(L[i], 0, 0, &x1, &y1, &w, &h);
    gfx->setTextColor(i ? RGB565(255, 210, 60) : RGB565_WHITE);
    gfx->setCursor(233 - (int)w / 2 - x1, 220 + i * 72);
    gfx->print(L[i]);
  }
  gfx->setFont();
  gfx->setTextSize(1);
  char what[24];                                                   // v3.24: how many claps it heard
  if (clapRoundSize <= 1)      snprintf(what, sizeof(what), "*clap*");
  else if (clapRoundSize == 2) snprintf(what, sizeof(what), "*clap* *clap*");
  else                         snprintf(what, sizeof(what), "*clap* x%d", clapRoundSize);
  uiCentred(112, what, &UiBig, RGB565(150, 170, 210));
  gfx->flush();
}

// loop(): answer a clap. Voice on every screen (unless muted); the splash only on the quiet
// screens it can draw over and put back - the home screen, and the screensaver (which it wakes).
void thanksTick() {
  if (thanksUntil && (int32_t)(millis() - thanksUntil) >= 0) {
    thanksUntil = 0;
    if (screen == ST_HOME && !saverActive()) showScreen(ST_HOME);
  }
  if (!clapPending) return;
  clapPending = false;
  bool fromSaver = saverActive();
  if (fromSaver) saverWake(screen != ST_HOME, "clap");
  else saverPoke();
  Serial.printf("clap   : -> Thanks Danny! (%d clap%s)%s\n", clapRoundSize, clapRoundSize == 1 ? "" : "s",
                sosMuted ? " (muted: splash only)" : "");
  sosPlay(SND_THANKS, SND_THANKS_LEN, THANKS_GAIN);               // Sos.ino; does nothing when muted
  if (screen == ST_HOME) {
    thanksSplash();
    thanksUntil = millis() + THANKS_SHOW_MS;
    if (!thanksUntil) thanksUntil = 1;
  }
}
