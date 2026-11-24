/*
 * fastlcd (v3.41) - a faster way to get the PSRAM canvas onto the CO5300.
 *
 * Why: Arduino_ESP32QSPI::writePixels() sends a frame in 1024-pixel (2 kB) pieces and, for each piece,
 * first byte-swaps it from PSRAM into one DMA buffer and then starts the transfer and WAITS for it
 * (spi_device_polling_start + polling_end). Nothing overlaps: ~213 rounds of "swap, send, wait" made a
 * full 466x466 frame 46 ms, about four times what the 80 MHz quad bus can carry (~11 ms).
 *
 * Here: two 16 kB DMA buffers in internal RAM. While one is on the wire (queued, interrupt-driven DMA),
 * the CPU swaps the next rows into the other. The address window (CASET / PASET / RAMWR) still goes
 * through the library - Arduino_CO5300 caches the window and would otherwise skip setting it later.
 *
 * Bus sharing: the panel's own SPI device (inside Arduino_ESP32QSPI) and ours sit on the same host.
 * That only works when the GFX bus is created with is_shared_interface = true - otherwise the library
 * keeps the bus acquired forever and our acquire would wait for ever. Chip select is a plain GPIO in
 * both (spics_io_num = -1), so CS stays low across all the pieces of one frame.
 */

#include <Arduino.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include "fastlcd.h"

#define FAST_PX   8192                         // pixels per piece (16 kB; the bus allows 16392 bytes)
#define FAST_HOST SPI2_HOST                    // = ESP32QSPI_SPI_HOST

static Arduino_CO5300 *sPanel = nullptr;
static spi_device_handle_t sDev = nullptr;
static uint32_t *sBuf[2] = { nullptr, nullptr };
static spi_transaction_ext_t sT[2];
static int sCs = -1;
static bool sReady = false;

bool fastLcdBegin(Arduino_CO5300 *panel, int csPin, int32_t hz) {
  sPanel = panel;
  sCs = csPin;
  for (int i = 0; i < 2; i++) {
    sBuf[i] = (uint32_t *)heap_caps_aligned_alloc(16, FAST_PX * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!sBuf[i]) return false;
  }
  spi_device_interface_config_t dev = {};
  dev.command_bits = 8;
  dev.address_bits = 24;
  dev.mode = 0;
  dev.clock_source = SPI_CLK_SRC_DEFAULT;
  dev.clock_speed_hz = hz;
  dev.spics_io_num = -1;                       // CS by hand, like the library
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  dev.queue_size = 2;
  if (spi_bus_add_device(FAST_HOST, &dev, &sDev) != ESP_OK) return false;
  sReady = true;
  return true;
}

bool fastLcdReady() { return sReady; }

// Swap the bytes of each pixel (the panel wants big-endian RGB565), two pixels per 32-bit word.
static inline void swapRows(uint32_t *dst, const uint16_t *src, int w, int rows, int stride) {
  int pairs = w >> 1;
  for (int r = 0; r < rows; r++) {
    const uint32_t *s = (const uint32_t *)(src + (size_t)r * stride);
    for (int i = 0; i < pairs; i++) {
      uint32_t v = s[i];
      dst[i] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    }
    dst += pairs;
  }
}

void fastLcdBlit(int x, int y, int w, int h, const uint16_t *src, int stride) {
  if (w <= 0 || h <= 0) return;
  if (!sReady || (w & 1) || (((uintptr_t)src) & 3) || (stride & 1) || w > FAST_PX) {
    sPanel->draw16bitRGBBitmap(x, y, (uint16_t *)src, w, h);   // the library's own (slow) path
    return;
  }
  sPanel->startWrite();
  sPanel->writeAddrWindow(x, y, w, h);         // CASET, PASET, RAMWR - via the library
  sPanel->endWrite();

  spi_device_acquire_bus(sDev, portMAX_DELAY);
  gpio_set_level((gpio_num_t)sCs, 0);
  int rowsPer = FAST_PX / w;
  int inFlight = 0, k = 0;
  bool first = true;
  spi_transaction_t *done;
  for (int row = 0; row < h; row += rowsPer) {
    int n = h - row < rowsPer ? h - row : rowsPer;
    if (inFlight == 2) { spi_device_get_trans_result(sDev, &done, portMAX_DELAY); inFlight--; }
    swapRows(sBuf[k], src + (size_t)row * stride, w, n, stride);
    spi_transaction_ext_t *t = &sT[k];
    memset(t, 0, sizeof(*t));
    if (first) {                               // "write pixels, quad": cmd 0x32, address 0x003C00 (RAMWRC)
      t->base.flags = SPI_TRANS_MODE_QIO;
      t->base.cmd = 0x32;
      t->base.addr = 0x003C00;
      first = false;
    } else {                                   // the rest: data only, CS still low
      t->base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
    }
    t->base.tx_buffer = sBuf[k];
    t->base.length = (size_t)n * w * 16;       // bits
    spi_device_queue_trans(sDev, &t->base, portMAX_DELAY);
    inFlight++;
    k ^= 1;
  }
  while (inFlight) { spi_device_get_trans_result(sDev, &done, portMAX_DELAY); inFlight--; }
  gpio_set_level((gpio_num_t)sCs, 1);
  spi_device_release_bus(sDev);
}
