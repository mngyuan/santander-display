// Thin drawing layer over Elecrow's EPD driver (EPD.cpp / EPDfont.h).
// Everything draws into the 250x122 ImageBW buffer; nothing touches the panel.
// Coordinates are landscape, origin top-left. Fonts: 12, 16, 24, 32, 48 px
// tall, each half as wide (6x12 ... 24x48). Text is opaque: its cell
// background is painted, so ink=false gives white-on-black.
#pragma once
#include <stdint.h>

#define SCREEN_W 250
#define SCREEN_H 122

void gfxClear();
void gfxPixel(int x, int y, bool ink);
void gfxFillRect(int x, int y, int w, int h, bool ink = true);
void gfxRect(int x, int y, int w, int h, bool ink = true);
void gfxHLine(int x, int y, int w, bool ink = true);
void gfxHLineDashed(int x, int y, int w, int on, int off);

int gfxTextWidth(const char *s, int size);
void gfxText(int x, int y, const char *s, int size, bool ink = true);
void gfxTextRight(int xRight, int y, const char *s, int size, bool ink = true);
void gfxTextCenter(int xCenter, int y, const char *s, int size, bool ink = true);

// Raw access for the panel I/O and preview tool.
const uint8_t *gfxBuffer();
int gfxBufferSize();
