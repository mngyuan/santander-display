#include "render.h"
#include "gfx.h"
#include <stdio.h>

// Layout (pixels). Placeholder engineering layout: shows every value the
// device fetches. Fonts are 12/16/24/32/48 px tall and half as wide.
static const int MARGIN = 6;
static const int RIGHT = SCREEN_W - MARGIN; // 244
static const int SPLIT_Y = 79;              // dashed line between the two docks

// Big number with a small label to its right, bottoms aligned. Returns the x
// just past the label.
static int bigCount(int x, int y, int n, const char *label) {
  char num[8];
  snprintf(num, sizeof num, "%d", n);
  gfxText(x, y, num, 32);
  x += gfxTextWidth(num, 32) + 3;
  gfxText(x, y + 18, label, 12);
  return x + gfxTextWidth(label, 12);
}

// Inverted word, for states that need to stand out (CLOSED, FULL, EMPTY).
static void badge(int x, int y, const char *word, int size) {
  int w = gfxTextWidth(word, size);
  gfxFillRect(x - 3, y - 2, w + 6, size + 3);
  gfxText(x, y, word, size, false);
}

// Battery outline 18x9 with a 2x5 nub, filled in proportion to pct.
static void battery(int x, int y, int pct) {
  gfxRect(x, y, 18, 9);
  gfxFillRect(x + 18, y + 2, 2, 5);
  int fill = (pct * 14 + 50) / 100;
  if (pct > 0 && fill < 1) fill = 1;
  gfxFillRect(x + 2, y + 2, fill, 5);
}

// One 5x5 square per screen after the label; the current one filled.
static void pageDots(int x, const RenderCtx &ctx) {
  if (ctx.nScreens <= 1) return;
  for (int i = 0; i < ctx.nScreens; i++) {
    if (i == ctx.screen) gfxFillRect(x + i * 8, 5, 5, 5);
    else gfxRect(x + i * 8, 5, 5, 5);
  }
}

static void header(const char *label, const RenderCtx &ctx) {
  gfxText(MARGIN, 2, label, 12);
  pageDots(MARGIN + gfxTextWidth(label, 12) + 6, ctx);
  char hm[6];
  fmtHM(ctx.dataMin, hm);
  const char *right = ctx.busy ? "updating" : hm;
  gfxTextRight(RIGHT, 2, right, 12);
  if (ctx.stale && !ctx.busy) {
    int x = RIGHT - gfxTextWidth(right, 12) - 10;
    gfxFillRect(x - 1, 1, 8, 14);
    gfxText(x, 2, "!", 12, false);
  }
}

static void homeDock(const Dock &d, const char *label, const RenderCtx &ctx) {
  header(label, ctx);
  if (!d.hasCounts) { gfxText(MARGIN, 26, "no data yet", 16); return; }
  if (d.closed) { badge(MARGIN + 3, 24, "CLOSED", 24); return; }

  int x = bigCount(MARGIN, 16, d.bikes, d.bikes == 1 ? "bike" : "bikes");
  bigCount(x + 12, 16, d.ebikes, d.ebikes == 1 ? "e-bike" : "e-bikes");
  char line[16];
  snprintf(line, sizeof line, "%d free", d.spaces);
  gfxTextRight(RIGHT, 34, line, 12);
  if (d.bikes + d.ebikes == 0) badge(RIGHT - gfxTextWidth("EMPTY", 12), 18, "EMPTY", 12);

  // Battery row: fullest first, percent under each.
  const int rowY = 54;
  if (!d.hasBatt) { gfxText(MARGIN, rowY + 2, d.ebikes ? "battery n/a" : "", 12); return; }
  if (d.nBatt == 0) { gfxText(MARGIN, rowY + 2, "no e-bikes", 12); return; }
  int shown = keptBatt(d);
  bool more = d.nBatt > shown;
  if (more && shown == MAX_BATT) shown--; // make room for "+N"
  const int slot = 29;
  for (int i = 0; i < shown; i++) {
    int sx = MARGIN + i * slot;
    battery(sx, rowY, d.batt[i]);
    char pct[5];
    snprintf(pct, sizeof pct, "%d", d.batt[i]);
    gfxTextCenter(sx + 10, rowY + 11, pct, 12);
  }
  if (more) {
    char rest[6];
    snprintf(rest, sizeof rest, "+%d", d.nBatt - shown);
    gfxText(MARGIN + shown * slot, rowY + 5, rest, 12);
  }
}

static void destDock(const Dock &d, const char *label) {
  const int y = SPLIT_Y + 4;
  gfxText(MARGIN, y + 2, label, 16);
  if (!d.hasCounts) { gfxTextRight(RIGHT, y + 6, "no data yet", 16); return; }
  if (d.closed) { badge(RIGHT - gfxTextWidth("CLOSED", 24), y + 6, "CLOSED", 24); return; }

  char line[32];
  if (d.docks > 0) snprintf(line, sizeof line, "%d bikes, %d docks", d.bikes + d.ebikes, d.docks);
  else snprintf(line, sizeof line, "%d bikes", d.bikes + d.ebikes);
  gfxText(MARGIN, y + 22, line, 12);

  if (d.spaces == 0) { badge(RIGHT - gfxTextWidth("FULL", 32), y + 4, "FULL", 32); return; }
  const char *word = d.spaces == 1 ? "space" : "spaces";
  char num[8];
  snprintf(num, sizeof num, "%d", d.spaces);
  int x = RIGHT - gfxTextWidth(word, 12) - 3 - gfxTextWidth(num, 32);
  bigCount(x, y + 4, d.spaces, word);
}

void renderDocks(const Dock &home, const char *homeLabel, const Dock &dest, const char *destLabel,
                 const RenderCtx &ctx) {
  gfxClear();
  homeDock(home, homeLabel, ctx);
  gfxHLineDashed(MARGIN, SPLIT_Y, SCREEN_W - 2 * MARGIN, 2, 2);
  destDock(dest, destLabel);
}

void renderMessage(const char *line1, const char *line2) {
  gfxClear();
  gfxTextCenter(SCREEN_W / 2, 38, line1, 16);
  if (line2) gfxTextCenter(SCREEN_W / 2, 64, line2, 12);
}

void renderStatus(const StatusInfo &info) {
  gfxClear();
  gfxText(MARGIN, 3, "status", 12);
  const char *lines[] = {info.synced, info.wifi, info.nextWake, info.docks, info.version};
  for (int i = 0; i < 5; i++) gfxText(MARGIN, 22 + i * 18, lines[i], 16);
}
