// Renders every screen to 250x122 PNGs on your computer, using the real
// render code and Elecrow's bitmap fonts. Iterate on layout here instead of
// flashing the board each time.
//
//   ./tools/preview/build.sh   ->  tools/preview/out/*.png
#include <stdio.h>
#include <string.h>
#include <initializer_list>
#include "docks.h"
#include "render.h"
#include "gfx.h"
#include "EPD_Init.h" // EPD_W/EPD_H/USE_HORIZONTIAL

static bool pixelAt(int x, int y) {
  // Inverse of EPD_DrawPoint for USE_HORIZONTIAL == 2.
  const uint8_t *buf = gfxBuffer();
  int xp = y, yp = EPD_W - x - 1;
  int addr = xp / 8 + yp * ((EPD_H % 8 == 0) ? (EPD_H / 8) : (EPD_H / 8 + 1));
  return buf[addr] & (0x80 >> (xp % 8));
}

static void save(const char *name) {
  char path[256];
  snprintf(path, sizeof path, "out/%s.pbm", name);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P1\n%d %d\n", SCREEN_W, SCREEN_H);
  for (int y = 0; y < SCREEN_H; y++) {
    for (int x = 0; x < SCREEN_W; x++) fputc(pixelAt(x, y) ? '1' : '0', f);
    fputc('\n', f);
  }
  fclose(f);
  printf("wrote %s\n", path);
}

static Dock dock(int id, int bikes, int ebikes, int spaces, int docks, std::initializer_list<int> batt) {
  Dock d;
  memset(&d, 0, sizeof d);
  d.id = id;
  d.hasCounts = true;
  d.bikes = bikes; d.ebikes = ebikes; d.spaces = spaces; d.docks = docks;
  d.hasBatt = true;
  strcpy(d.rangeUnit, "mi");
  for (int p : batt) addBatt(d, p, p * 40 / 100);
  return d;
}

static bool check(bool ok, const char *what) {
  if (!ok) printf("FAIL: %s\n", what);
  return ok;
}

// addBatt keeps the fullest MAX_BATT, highest first.
static bool testAddBatt() {
  Dock d = dock(1, 0, 0, 0, 0, {30, 90, 10, 55, 100, 0, 72, 64, 81, 5});
  bool ok = check(d.nBatt == 10, "nBatt counts all");
  int want[] = {100, 90, 81, 72, 64, 55, 30, 10};
  for (int i = 0; i < MAX_BATT; i++) ok &= check(d.batt[i] == want[i], "sorted, lowest dropped");
  Dock e = dock(1, 0, 0, 0, 0, {20, 40});
  ok &= check(e.nBatt == 2 && e.batt[0] == 40 && e.batt[1] == 20, "short list sorted");
  return ok;
}

int main() {
  if (!testAddBatt()) return 1;

  Dock home = dock(42, 7, 3, 21, 33, {87, 64, 31});
  Dock dest = dock(123, 30, 2, 5, 38, {});
  RenderCtx ctx{8 * 60 + 15, false, false};

  renderDocks(home, "WENLOCK RD", dest, "OLD ST", ctx);           save("01_typical");

  Dock many = dock(42, 1, 11, 20, 33, {95, 88, 80, 77, 61, 52, 40, 33, 20, 12, 4});
  renderDocks(many, "WENLOCK RD", dest, "OLD ST", ctx);           save("02_many_ebikes");

  Dock empty = dock(42, 0, 0, 33, 33, {});
  Dock full = dock(123, 36, 2, 0, 38, {});
  renderDocks(empty, "WENLOCK RD", full, "OLD ST", ctx);          save("03_home_empty_dest_full");

  Dock noBatt = home;
  noBatt.hasBatt = false;
  RenderCtx stale{7 * 60 + 50, true, false};
  renderDocks(noBatt, "WENLOCK RD", dest, "OLD ST", stale);       save("04_stale_battery_na");

  RenderCtx busy{7 * 60 + 50, false, true};
  renderDocks(home, "WENLOCK RD", dest, "OLD ST", busy);          save("05_updating");

  Dock closed = dest;
  closed.closed = true;
  Dock one = dock(42, 1, 1, 30, 33, {100});
  Dock oneSpace = dock(123, 36, 0, 1, 38, {});
  renderDocks(one, "WENLOCK RD", closed, "OLD ST", ctx);          save("06_dest_closed");
  renderDocks(one, "WENLOCK RD", oneSpace, "OLD ST", ctx);        save("07_one_space");

  Dock none;
  memset(&none, 0, sizeof none);
  RenderCtx never{-1, true, false};
  renderDocks(none, "WENLOCK RD", none, "OLD ST", never);         save("08_no_data");

  renderMessage("dock display", "connecting...");                 save("09_boot");

  StatusInfo info = {"synced 08:15", "wifi -61 dBm", "next 08:20", "docks 42 / 123", "fw 0.1.0"};
  renderStatus(info);                                             save("10_status");
  return 0;
}
