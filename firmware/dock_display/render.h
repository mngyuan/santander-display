// Screens. Each function clears the buffer and draws one full 250x122 frame.
// Pure drawing: no Arduino or network calls, so the desktop preview works.
#pragma once
#include "docks.h"

struct RenderCtx {
  int dataMin; // local minutes of the last good fetch, -1 if never
  bool stale;  // last fetch failed; showing older data
  bool busy;   // a fetch is under way
};

// Home dock on top (bikes, e-bikes and their batteries), destination dock
// below (free spaces).
void renderDocks(const Dock &home, const char *homeLabel, const Dock &dest, const char *destLabel,
                 const RenderCtx &ctx);
void renderMessage(const char *line1, const char *line2);

struct StatusInfo {
  char synced[24]; // "synced 08:15"
  char wifi[24];   // "wifi -61 dBm" or "err: ..."
  char nextWake[24];
  char docks[24];  // "docks 42 / 123"
  char version[24];
};
void renderStatus(const StatusInfo &info);
