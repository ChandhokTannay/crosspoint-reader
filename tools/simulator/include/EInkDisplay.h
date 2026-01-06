#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

class EInkDisplay {
 public:
  enum RefreshMode { FAST_REFRESH, HALF_REFRESH };

  static constexpr int DISPLAY_WIDTH = 800;
  static constexpr int DISPLAY_HEIGHT = 480;
  static constexpr int DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr int BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

 private:
  uint8_t frameBuffer[BUFFER_SIZE];

 public:
  EInkDisplay() { clearScreen(0xFF); }
  ~EInkDisplay() = default;

  void begin() {}
  void deepSleep() {}

  uint8_t* getFrameBuffer() { return frameBuffer; }
  const uint8_t* getFrameBuffer() const { return frameBuffer; }

  void clearScreen(const uint8_t color = 0xFF) { std::memset(frameBuffer, color, BUFFER_SIZE); }

  void displayBuffer(RefreshMode = FAST_REFRESH) {}
  void displayWindow(int, int, int, int) {}

  // Image rendering is handled by GfxRenderer which calls this in native coords.
  void drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) {
    // 1-bit bitmap, MSB first, row-major.
    const int bytesPerRow = (width + 7) / 8;

    for (int iy = 0; iy < height; iy++) {
      for (int ix = 0; ix < width; ix++) {
        const int srcByteIndex = iy * bytesPerRow + (ix / 8);
        const uint8_t srcBit = 7 - (ix % 8);
        const bool black = ((bitmap[srcByteIndex] >> srcBit) & 0x1) != 0;

        const int px = x + ix;
        const int py = y + iy;
        if (px < 0 || px >= DISPLAY_WIDTH || py < 0 || py >= DISPLAY_HEIGHT) {
          continue;
        }

        const int dstByteIndex = py * DISPLAY_WIDTH_BYTES + (px / 8);
        const uint8_t dstBit = 7 - (px % 8);
        if (black) {
          frameBuffer[dstByteIndex] &= ~(1 << dstBit);
        } else {
          frameBuffer[dstByteIndex] |= 1 << dstBit;
        }
      }
    }
  }

  // Grayscale methods are no-ops in the simulator.
  void grayscaleRevert() const {}
  void copyGrayscaleLsbBuffers(uint8_t*) const {}
  void copyGrayscaleMsbBuffers(uint8_t*) const {}
  void displayGrayBuffer() const {}
  void cleanupGrayscaleBuffers(uint8_t*) const {}
};
