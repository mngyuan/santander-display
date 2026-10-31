#include <Arduino.h>
#include "EPD.h"
#include "epd_io.h"
#include "config.h"
#ifndef DOCK_DISPLAY_CONFIG
#error "config.h missing or out of date: copy config.example.h to config.h in this folder and fill it in"
#endif

extern uint8_t ImageBW[ALLSCREEN_BYTES];

// Which panel controller the fetched driver is for (tools/fetch_driver.sh).
// Elecrow's v1.2 header (JD79661) declares a 128-row buffer; the older
// SSD1680 one declares 122. Both lay ImageBW out the same way in landscape.
#if EPD_H == 128
#define PANEL_JD79661 1
#else
#define PANEL_JD79661 0
#endif

// Last frame on the panel, kept across deep sleep so the first partial update
// after a button wake has a correct "old" image to diff against.
RTC_DATA_ATTR static uint8_t lastFrame[ALLSCREEN_BYTES];
RTC_DATA_ATTR static bool lastFrameValid = false;
RTC_DATA_ATTR static uint16_t partialCount = 0;

static const int EPD_POWER_PIN = 7; // from Elecrow examples: HIGH = panel powered

void epdPowerOn() {
  static bool cycled = false;
  pinMode(EPD_POWER_PIN, OUTPUT);
  if (!cycled) {
    // Cut panel power briefly once per boot, so a panel left mid-update by a
    // reset starts clean.
    digitalWrite(EPD_POWER_PIN, LOW);
    delay(500);
    cycled = true;
  }
  digitalWrite(EPD_POWER_PIN, HIGH);
  delay(10);
}

static void remember() {
  memcpy(lastFrame, ImageBW, ALLSCREEN_BYTES);
  lastFrameValid = true;
}

#if PANEL_JD79661
// ---------------------------------------------------------------- JD79661
// Sequence from Elecrow's arduino-v1.2 example: EPD_Init, data to 0x13, load
// a waveform (GC = full, DU = fast), refresh with 0x17, deep sleep with 0x07.
// BUSY is LOW while busy on this chip (the SSD1680 is the other way round).

// Elecrow's v1.2 text code draws glyphs mirrored, i.e. the panel shows the
// buffer flipped left-right. Flip the whole frame here instead, so lines,
// boxes and text all come out the right way. Set to 0 if the screen reads
// backwards.
#ifndef EPD_MIRROR_X
#define EPD_MIRROR_X 1
#endif

static const int ROW_BYTES = ALLSCREEN_BYTES / EPD_W; // one landscape column of pixels

static bool waitIdle(const char *what, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (EPD_ReadBUSY == 0) {
    if (millis() - start > timeoutMs) {
      Serial.printf("epd: %s timed out (busy stuck low)\n", what);
      return false;
    }
    delay(1);
  }
  return true;
}

static void sendFrame(uint8_t reg, const uint8_t *frame) {
  EPD_WR_REG(reg);
  for (int r = 0; r < EPD_W; r++) {
    const uint8_t *row = frame + (EPD_MIRROR_X ? (EPD_W - 1 - r) : r) * ROW_BYTES;
    for (int b = 0; b < ROW_BYTES; b++) EPD_WR_DATA8(row[b]);
  }
}

// Load a waveform. Elecrow's lut_GC/lut_DU swap the 0x22/0x23 tables on
// every other call, which reverses the black<->white transitions when we
// also send the real "old" image, so the second refresh after a boot left
// the panel unchanged. Always load them the same way instead.
static void writeLut(uint8_t reg, const unsigned char *lut) {
  EPD_WR_REG(reg);
  for (int i = 0; i < 56; i++) EPD_WR_DATA8(lut[i]);
}

static void loadLut(bool fast) {
  if (fast) {
    writeLut(0x20, lut_R20_DU); writeLut(0x21, lut_R21_DU); writeLut(0x22, lut_R22_DU);
    writeLut(0x23, lut_R23_DU); writeLut(0x24, lut_R24_DU);
  } else {
    writeLut(0x20, lut_R20_GC); writeLut(0x21, lut_R21_GC); writeLut(0x22, lut_R22_GC);
    writeLut(0x23, lut_R23_GC); writeLut(0x24, lut_R24_GC);
  }
}

static void jdShow(bool fast) {
  epdPowerOn();
  EPD_Init(); // hardware reset + panel setup; also wakes it from deep sleep
  waitIdle("reset", 2000);
  EPD_WR_REG(0x50); // data polarity, as in Elecrow's EPD_DisplayImage
  EPD_WR_DATA8(0xD7);
  if (lastFrameValid) {
    sendFrame(0x10, lastFrame); // "old" image: what the panel shows now
  } else {
    static const uint8_t blank[ALLSCREEN_BYTES] = {0}; // unknown: assume white
    sendFrame(0x10, blank);
  }
  sendFrame(0x13, ImageBW); // "new" image
  loadLut(fast);
  EPD_WR_REG(0x17); // refresh
  EPD_WR_DATA8(0xA5);
  waitIdle("refresh", 10000);
  EPD_Sleep();
  remember();
}

void epdShowFull() {
  Serial.println("epd: full (jd79661)");
  jdShow(false);
  partialCount = 0;
  Serial.println("epd: done");
}

void epdShowPartial() {
  if (!lastFrameValid || partialCount >= FULL_REFRESH_EVERY) { epdShowFull(); return; }
  jdShow(true);
  partialCount++;
}

#else
// ---------------------------------------------------------------- SSD1680
// Write a frame to the SSD1680 "old" RAM (0x26), same bit inversion as
// EPD_DisplayImage uses for the "new" RAM (0x24). Address counters wrap after
// a full-screen write, the same way Elecrow's EPD_Clear_R26H relies on.
static void writeOldRam(const uint8_t *frame) {
  EPD_WR_REG(0x26);
  for (uint32_t i = 0; i < ALLSCREEN_BYTES; i++) EPD_WR_DATA8(~frame[i]);
}

void epdShowFull() {
  Serial.println("epd: full (ssd1680)");
  epdPowerOn();
  // Elecrow's sequence: clear to white with a full update, then draw with a
  // partial update on top. Gives a clean panel each time.
  EPD_Init();
  EPD_ALL_Fill(WHITE);
  EPD_Update();
  EPD_Clear_R26H();
  EPD_DisplayImage(ImageBW);
  EPD_PartUpdate();
  EPD_Sleep();
  remember();
  partialCount = 0;
  Serial.println("epd: done");
}

void epdShowPartial() {
  if (!lastFrameValid || partialCount >= FULL_REFRESH_EVERY) { epdShowFull(); return; }
  epdPowerOn();
  EPD_Init(); // hardware reset; panel RAM is rewritten below, so its state doesn't matter
  writeOldRam(lastFrame);
  EPD_DisplayImage(ImageBW);
  EPD_PartUpdate();
  EPD_Sleep();
  remember();
  partialCount++;
}
#endif
