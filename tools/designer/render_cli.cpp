// The designer's renderer: the firmware's own ui.h and render.cpp, run on
// your computer. serve.py writes one request to stdin and reads one JSON
// answer from stdout, so the browser shows exactly what the panel would.
//
// Request (one item per line):
//   ui <screen> <nScreens> <status 0|1>
//   key up|down|ok|back|menu              optional; applied with ui.h first
//   ctx <dataMin> <stale 0|1> <busy 0|1>
//   slot <i> <id> <hasCounts> <closed> <bikes> <ebikes> <spaces> <docks> <hasBatt>
//   batt <i> <percent> [<percent> ...]     every e-bike at slot i, any order
//   label <i> <text to end of line>
//   status <line>|<line>|<line>|<line>|<line>
//   message <line1>|<line2>               draw this instead (boot screen)
// Answer: {"ui":{...},"action":"none|redraw|fetch","bits":"0101..."} with
// one character per pixel, row by row, 1 = black.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "docks.h"
#include "ui.h"
#include "render.h"
#include "gfx.h"
#include "EPD_Init.h" // EPD_W/EPD_H/USE_HORIZONTIAL

static bool pixelAt(int x, int y) {
  // Inverse of EPD_DrawPoint for USE_HORIZONTIAL == 2 (as tools/preview).
  const uint8_t *buf = gfxBuffer();
  int xp = y, yp = EPD_W - x - 1;
  int addr = xp / 8 + yp * ((EPD_H % 8 == 0) ? (EPD_H / 8) : (EPD_H / 8 + 1));
  return buf[addr] & (0x80 >> (xp % 8));
}

static void copyField(char *dst, size_t len, const std::string &src) {
  size_t n = src.size() < len - 1 ? src.size() : len - 1;
  memcpy(dst, src.data(), n);
  dst[n] = 0;
}

static void splitBars(const std::string &s, std::string *out, int n) {
  size_t start = 0;
  for (int i = 0; i < n; i++) {
    size_t bar = s.find('|', start);
    out[i] = s.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
    if (bar == std::string::npos) { for (int j = i + 1; j < n; j++) out[j].clear(); return; }
    start = bar + 1;
  }
}

int main() {
  UiState ui{0, 1, false};
  RenderCtx ctx{-1, false, false, 0, 1};
  static Dock slots[MAX_SLOTS];
  static char labels[MAX_SLOTS][DOCK_LABEL_LEN + 1];
  memset(slots, 0, sizeof slots);
  memset(labels, 0, sizeof labels);
  StatusInfo status;
  memset(&status, 0, sizeof status);
  bool haveKey = false, haveMessage = false;
  UiKey key = UK_OK;
  std::string msg[2];

  char line[512];
  while (fgets(line, sizeof line, stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    char cmd[16] = "";
    int used = 0;
    if (sscanf(line, "%15s %n", cmd, &used) < 1) continue;
    const char *rest = line + used;
    if (!strcmp(cmd, "ui")) {
      int s, n, st;
      if (sscanf(rest, "%d %d %d", &s, &n, &st) == 3) {
        if (n < 1) n = 1;
        if (n > MAX_SCREENS) n = MAX_SCREENS;
        ui = UiState{(uint8_t)(s >= 0 && s < n ? s : 0), (uint8_t)n, st != 0};
      }
    } else if (!strcmp(cmd, "key")) {
      haveKey = true;
      if (!strcmp(rest, "up")) key = UK_UP;
      else if (!strcmp(rest, "down")) key = UK_DOWN;
      else if (!strcmp(rest, "back")) key = UK_BACK;
      else if (!strcmp(rest, "menu")) key = UK_MENU;
      else key = UK_OK;
    } else if (!strcmp(cmd, "ctx")) {
      int m, st, b;
      if (sscanf(rest, "%d %d %d", &m, &st, &b) == 3) { ctx.dataMin = m; ctx.stale = st; ctx.busy = b; }
    } else if (!strcmp(cmd, "slot")) {
      int i, id, hc, cl, bk, eb, sp, dk, hb;
      if (sscanf(rest, "%d %d %d %d %d %d %d %d %d", &i, &id, &hc, &cl, &bk, &eb, &sp, &dk, &hb) == 9 &&
          i >= 0 && i < MAX_SLOTS) {
        Dock &d = slots[i];
        d.id = id; d.hasCounts = hc; d.closed = cl;
        d.bikes = bk; d.ebikes = eb; d.spaces = sp; d.docks = dk; d.hasBatt = hb;
      }
    } else if (!strcmp(cmd, "batt")) {
      char *end;
      long i = strtol(rest, &end, 10);
      if (end == rest || i < 0 || i >= MAX_SLOTS) continue;
      for (const char *p = end;;) {
        long pct = strtol(p, &end, 10);
        if (end == p) break;
        addBatt(slots[i], (int)pct, 0);
        p = end;
      }
    } else if (!strcmp(cmd, "label")) {
      char *end;
      long i = strtol(rest, &end, 10);
      if (end == rest || i < 0 || i >= MAX_SLOTS) continue;
      if (*end == ' ') end++;
      copyField(labels[i], sizeof labels[i], end);
    } else if (!strcmp(cmd, "status")) {
      std::string f[5];
      splitBars(rest, f, 5);
      copyField(status.synced, sizeof status.synced, f[0]);
      copyField(status.wifi, sizeof status.wifi, f[1]);
      copyField(status.nextWake, sizeof status.nextWake, f[2]);
      copyField(status.docks, sizeof status.docks, f[3]);
      copyField(status.version, sizeof status.version, f[4]);
    } else if (!strcmp(cmd, "message")) {
      haveMessage = true;
      splitBars(rest, msg, 2);
    }
  }

  UiAction action = haveKey ? uiHandle(ui, key) : UA_REDRAW;
  ctx.screen = ui.screen;
  ctx.nScreens = ui.nScreens;
  if (haveMessage) {
    renderMessage(msg[0].c_str(), msg[1].empty() ? nullptr : msg[1].c_str());
  } else if (ui.status) {
    renderStatus(status);
  } else {
    int h = ui.screen * 2, d = h + 1;
    renderDocks(slots[h], labels[h], slots[d], labels[d], ctx);
  }

  const char *names[] = {"none", "redraw", "fetch"};
  printf("{\"ui\":{\"screen\":%d,\"nScreens\":%d,\"status\":%s},\"action\":\"%s\",\"width\":%d,\"height\":%d,\"bits\":\"",
         ui.screen, ui.nScreens, ui.status ? "true" : "false", names[action], SCREEN_W, SCREEN_H);
  for (int y = 0; y < SCREEN_H; y++)
    for (int x = 0; x < SCREEN_W; x++) putchar(pixelAt(x, y) ? '1' : '0');
  printf("\"}\n");
  return 0;
}
