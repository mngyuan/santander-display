#include "gfx.h"
#include "EPD.h"
#include <string.h>

extern uint8_t ImageBW[ALLSCREEN_BYTES];

void gfxClear() { memset(ImageBW, 0x00, ALLSCREEN_BYTES); } // 0 bits = white

void gfxPixel(int x, int y, bool ink) {
  if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return; // driver has no bounds check
  EPD_DrawPoint(x, y, ink ? BLACK : WHITE);
}

void gfxFillRect(int x, int y, int w, int h, bool ink) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) gfxPixel(i, j, ink);
}

void gfxRect(int x, int y, int w, int h, bool ink) {
  if (w <= 0 || h <= 0) return;
  gfxHLine(x, y, w, ink);
  gfxHLine(x, y + h - 1, w, ink);
  for (int j = y; j < y + h; j++) { gfxPixel(x, j, ink); gfxPixel(x + w - 1, j, ink); }
}

void gfxHLine(int x, int y, int w, bool ink) {
  for (int i = x; i < x + w; i++) gfxPixel(i, y, ink);
}

void gfxHLineDashed(int x, int y, int w, int on, int off) {
  for (int i = 0; i < w; i++)
    if (i % (on + off) < on) gfxPixel(x + i, y, true);
}

static bool validSize(int size) {
  return size == 12 || size == 16 || size == 24 || size == 32 || size == 48;
}

int gfxTextWidth(const char *s, int size) { return (int)strlen(s) * (size / 2); }

void gfxText(int x, int y, const char *s, int size, bool ink) {
  if (!validSize(size)) return;
  // Driver only renders ' '..'~' and stops at anything else; sanitise and clip.
  char buf[64];
  int maxChars = (SCREEN_W - x) / (size / 2);
  int n = 0;
  for (; s[n] && n < (int)sizeof(buf) - 1 && n < maxChars; n++)
    buf[n] = (s[n] >= ' ' && s[n] <= '~') ? s[n] : '?';
  buf[n] = 0;
  if (n == 0 || x < 0 || y < 0 || y + size > SCREEN_H) return;
  EPD_ShowString(x, y, buf, ink ? BLACK : WHITE, size);
}

void gfxTextRight(int xRight, int y, const char *s, int size, bool ink) {
  gfxText(xRight - gfxTextWidth(s, size), y, s, size, ink);
}

void gfxTextCenter(int xCenter, int y, const char *s, int size, bool ink) {
  gfxText(xCenter - gfxTextWidth(s, size) / 2, y, s, size, ink);
}

const uint8_t *gfxBuffer() { return ImageBW; }
int gfxBufferSize() { return ALLSCREEN_BYTES; }
