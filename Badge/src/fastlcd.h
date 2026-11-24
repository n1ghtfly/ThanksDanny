// Fast pixel path to the CO5300 (v3.41) - see fastlcd.cpp.
#pragma once
#include <Arduino_GFX_Library.h>

// Call after gfx->begin(): adds a second device on the panel's SPI bus (the GFX bus must be created
// with is_shared_interface = true) and allocates two DMA buffers in internal RAM. false = not available;
// fastLcdBlit() then falls back to the library's own path.
bool fastLcdBegin(Arduino_CO5300 *panel, int csPin, int32_t hz);
bool fastLcdReady();

// Send a w x h rectangle at (x, y) to the glass. src points at its top-left pixel (native RGB565 as in
// the canvas), `stride` = pixels per source row. x, y, w, h even (the CO5300 rule; flushRect() rounds).
void fastLcdBlit(int x, int y, int w, int h, const uint16_t *src, int stride);

// v3.44: like fastLcdBlit, but the pixels are not read from a buffer: fn(row, dst, ctx) fills each row
// (w pixels, ALREADY byte-swapped = big-endian RGB565) straight into the DMA buffer. Nothing goes through
// the PSRAM canvas. false = the fast path is not available (nothing was sent).
typedef void (*FastRowFn)(int row, uint16_t *dst, void *ctx);
bool fastLcdStream(int x, int y, int w, int h, FastRowFn fn, void *ctx);

// Arduino_Canvas whose flush() takes the fast path.
class FastCanvas : public Arduino_Canvas {
public:
  FastCanvas(int16_t w, int16_t h, Arduino_G *output) : Arduino_Canvas(w, h, output) {}
  void flush(bool force_flush = false) override {
    if (fastLcdReady()) fastLcdBlit(0, 0, width(), height(), getFramebuffer(), width());
    else Arduino_Canvas::flush(force_flush);
  }
};
