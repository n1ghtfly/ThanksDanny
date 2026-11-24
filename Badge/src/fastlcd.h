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

// Arduino_Canvas whose flush() takes the fast path.
class FastCanvas : public Arduino_Canvas {
public:
  FastCanvas(int16_t w, int16_t h, Arduino_G *output) : Arduino_Canvas(w, h, output) {}
  void flush(bool force_flush = false) override {
    if (fastLcdReady()) fastLcdBlit(0, 0, width(), height(), getFramebuffer(), width());
    else Arduino_Canvas::flush(force_flush);
  }
};
