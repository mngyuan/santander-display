// What each button does. Pure logic, shared by the firmware and the
// designer (tools/designer), so the mock buttons behave like the device.
#pragma once
#include <stdint.h>

enum UiKey : uint8_t { UK_UP, UK_DOWN, UK_OK, UK_BACK, UK_MENU };
enum UiAction : uint8_t { UA_NONE, UA_REDRAW, UA_FETCH };

struct UiState {
  uint8_t screen;   // which pair of docks is shown
  uint8_t nScreens; // from docks.json, 1..MAX_SCREENS
  bool status;      // status screen up
};

//   dial up / down  previous / next screen (wraps); leaves the status screen
//   dial press      fetch fresh data now
//   back            leave the status screen, else go to the first screen
//   menu            status screen on / off
inline UiAction uiHandle(UiState &s, UiKey k) {
  switch (k) {
    case UK_UP:
    case UK_DOWN:
      if (s.status) { s.status = false; return UA_REDRAW; }
      if (s.nScreens <= 1) return UA_NONE;
      s.screen = (uint8_t)((s.screen + (k == UK_DOWN ? 1 : s.nScreens - 1)) % s.nScreens);
      return UA_REDRAW;
    case UK_OK:
      s.status = false;
      return UA_FETCH;
    case UK_BACK:
      if (s.status) { s.status = false; return UA_REDRAW; }
      if (s.screen == 0) return UA_NONE;
      s.screen = 0;
      return UA_REDRAW;
    case UK_MENU:
      s.status = !s.status;
      return UA_REDRAW;
  }
  return UA_NONE;
}
