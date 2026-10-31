// Plain data model shared by the network code, renderer and desktop preview.
// No Arduino dependencies, so it compiles natively.
#pragma once
#include <stdint.h>
#include <string.h>

#define MAX_SCREENS 4                // pairs of docks, switched with the dial
#define MAX_SLOTS (MAX_SCREENS * 2)  // slot = screen * 2 + role
#define ROLE_HOME 0                  // pick a bike up here
#define ROLE_DEST 1                  // drop it off here
#define MAX_BATT 8                   // e-bike batteries kept per dock, highest first
#define DOCK_NAME_LEN 40
#define DOCK_LABEL_LEN 16            // on-screen label, ASCII; checked by tools/docks_json.py

// One dock as configured in docks.json (see tools/docks_json.py).
struct DockRef {
  int32_t id;         // BikePoints_<id>; 0 = look it up with `query`
  const char *query;  // TfL search text, used only when id is 0
  const char *label;  // on-screen name, e.g. "WENLOCK RD"
};

struct ScreenConfig {
  DockRef dock[2]; // [ROLE_HOME], [ROLE_DEST]
};

struct Dock {
  int32_t id;               // TfL "BikePoints_<id>"; the GraphQL stationId is the same number
  char name[DOCK_NAME_LEN]; // TfL commonName, e.g. "Wenlock Road , Hoxton"

  // Counts: TfL BikePoint API, or the Santander GraphQL feed if TfL failed.
  bool hasCounts;
  bool closed;     // locked, not installed, or offline
  int16_t bikes;   // classic bikes
  int16_t ebikes;
  int16_t spaces;  // empty docks
  int16_t docks;   // total docks, -1 if unknown

  // Batteries: Santander GraphQL feed only.
  bool hasBatt;            // the feed answered and listed this dock
  uint8_t nBatt;           // e-bikes listed (may be more than MAX_BATT)
  uint8_t batt[MAX_BATT];  // percent, highest first
  uint8_t range[MAX_BATT]; // distance remaining, rounded, in rangeUnit
  char rangeUnit[4];       // "mi", "km" or ""
};

inline int keptBatt(const Dock &d) { return d.nBatt < MAX_BATT ? d.nBatt : MAX_BATT; }

// Adds one e-bike, keeping the MAX_BATT fullest in descending order.
inline void addBatt(Dock &d, int pct, int range) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  if (range < 0) range = 0;
  if (range > 255) range = 255;
  int kept = keptBatt(d);
  if (d.nBatt < 255) d.nBatt++;
  int pos = kept;
  while (pos > 0 && d.batt[pos - 1] < pct) pos--;
  if (pos >= MAX_BATT) return; // emptier than every one kept
  int last = kept < MAX_BATT ? kept : MAX_BATT - 1;
  for (int i = last; i > pos; i--) { d.batt[i] = d.batt[i - 1]; d.range[i] = d.range[i - 1]; }
  d.batt[pos] = (uint8_t)pct;
  d.range[pos] = (uint8_t)range;
}

inline void copyCounts(Dock &dst, const Dock &src) {
  dst.hasCounts = src.hasCounts;
  dst.closed = src.closed;
  dst.bikes = src.bikes;
  dst.ebikes = src.ebikes;
  dst.spaces = src.spaces;
  dst.docks = src.docks;
}

inline void copyBatt(Dock &dst, const Dock &src) {
  dst.hasBatt = src.hasBatt;
  dst.nBatt = src.nBatt;
  memcpy(dst.batt, src.batt, sizeof dst.batt);
  memcpy(dst.range, src.range, sizeof dst.range);
  memcpy(dst.rangeUnit, src.rangeUnit, sizeof dst.rangeUnit);
}

inline void fmtHM(int minutes, char *out) {
  if (minutes < 0) { strcpy(out, "--:--"); return; }
  int h = (minutes / 60) % 24, m = minutes % 60;
  out[0] = '0' + h / 10; out[1] = '0' + h % 10; out[2] = ':';
  out[3] = '0' + m / 10; out[4] = '0' + m % 10; out[5] = 0;
}
