// Dock display: CrowPanel ESP32-S3 2.13" e-paper (250x122, B/W).
//
// Shows live Santander Cycles numbers for up to four screens, each a pair of
// docks from docks.json: the home dock (bikes, e-bikes and each e-bike's
// battery) and the destination dock (free spaces to drop off). Data comes
// straight from TfL; no server of our own.
//
// Controls (pins from Elecrow's 2.13_key example; logic in ui.h):
//   dial up / down  previous / next screen
//   dial press      fetch fresh data now
//   back            leave the status screen, else go to the first screen
//   menu            status screen on / off
//
// Board: ESP32S3 Dev Module, FlashSize=8M. Libraries: ArduinoJson v7.
// Display driver files (EPD.*, EPD_Init.*, EPDfont.h, spi.*) are copied from
// Elecrow's repo by tools/fetch_driver.sh. docks_config.h is generated from
// docks.json by tools/docks_json.py (tools/flash.sh runs it).

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#include "config.h"
#ifndef DOCK_DISPLAY_CONFIG
#error "config.h missing or out of date: copy config.example.h to config.h in this folder and fill it in"
#endif
#if __has_include("docks_config.h")
#include "docks_config.h"
#else
#error "docks_config.h missing: run python3 tools/docks_json.py (or tools/flash.sh) to make it from docks.json"
#endif
#include "docks.h"
#include "ui.h"
#include "render.h"
#include "epd_io.h"
#include "net.h"
#include "santander.h"

#define FW_VERSION "0.2.1"

// Added in 0.2.0; defaults here so an older config.h still builds.
#ifndef SCROLL_SETTLE_MS
#define SCROLL_SETTLE_MS 1500 // after scrolling to a screen, wait this long before fetching
#endif
#ifndef SCREEN_STALE_S
#define SCREEN_STALE_S 120    // a screen whose numbers are older than this fetches when shown
#endif

// ---- Buttons (active low) ----
static const int KEY_MENU = 2;  // HOME_KEY in Elecrow example
static const int KEY_BACK = 1;  // EXIT_KEY
static const int KEY_UP = 6;    // PRV_KEY  (dial up)
static const int KEY_DOWN = 4;  // NEXT_KEY (dial down)
static const int KEY_OK = 5;    // OK_KEY   (dial press)
static const int KEYS[] = {KEY_MENU, KEY_BACK, KEY_UP, KEY_DOWN, KEY_OK};
static const int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);
static const int NO_KEY = -1;

// ---- Docks: slot = screen * 2 + role ----
static const int NSLOTS = NSCREENS * 2;
static_assert(NSCREENS >= 1 && NSCREENS <= MAX_SCREENS, "docks.json needs 1 to 4 screens");
static int32_t slotIds[MAX_SLOTS]; // from docks.json, or looked up once and kept in flash

static const DockRef &slotRef(int slot) { return SCREENS[slot / 2].dock[slot % 2]; }

// ---- State kept across deep sleep ----
RTC_DATA_ATTR static Dock cache[MAX_SLOTS];
RTC_DATA_ATTR static time_t countsAt[MAX_SLOTS]; // when each slot's counts last arrived
RTC_DATA_ATTR static uint8_t curScreen = 0;
RTC_DATA_ATTR static time_t lastSync = 0;  // last fetch that got the shown screen's counts
RTC_DATA_ATTR static time_t lastTry = 0;   // last fetch attempt
RTC_DATA_ATTR static int lastRssi = 0;
RTC_DATA_ATTR static char lastErr[32] = "";

static Preferences prefs;
static UiState ui;
static bool busy = false;

// ------------------------------------------------------------------ time
static bool localNow(struct tm &t) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false; // clock not set yet
  localtime_r(&now, &t);
  return true;
}

static bool isNight(const struct tm &t) { return t.tm_hour >= NIGHT_FROM || t.tm_hour < MORNING_FROM; }

static bool isActive(const struct tm &t) {
  int m = t.tm_hour * 60 + t.tm_min;
  bool day = !ACTIVE_WEEKDAYS_ONLY || (t.tm_wday >= 1 && t.tm_wday <= 5);
  return day && m >= ACTIVE_FROM && m < ACTIVE_TO;
}

static int cadenceMin(const struct tm &t) {
  if (isNight(t)) return 9 * 60; // cut short at MORNING_FROM below
  return isActive(t) ? CADENCE_ACTIVE : CADENCE_DEFAULT;
}

// Mode changes we want to wake for on time.
static bool isBoundary(const struct tm &t) {
  int m = t.tm_hour * 60 + t.tm_min;
  return m == MORNING_FROM * 60 || m == ACTIVE_FROM || m == ACTIVE_TO;
}

// Seconds until the next timer wake: the cadence for now, cut short at the
// next mode boundary (e.g. ACTIVE_FROM) so fast updates start on time.
static uint32_t nextWakeSeconds() {
  struct tm t;
  if (!localNow(t)) return 30 * 60;
  time_t now = time(nullptr);
  int cad = cadenceMin(t);
  for (int m = 1; m <= cad; m++) {
    time_t then = now + m * 60;
    struct tm tt;
    localtime_r(&then, &tt);
    if (isBoundary(tt)) return m * 60 - tt.tm_sec + 5;
  }
  return cad * 60;
}

// ------------------------------------------------------------------ docks
static void slotKeys(int slot, char *qKey, char *idKey) {
  snprintf(qKey, 5, "q%d", slot);
  snprintf(idKey, 5, "id%d", slot);
}

static void loadSlotIds() {
  for (int s = 0; s < MAX_SLOTS; s++) slotIds[s] = 0;
  for (int s = 0; s < NSLOTS; s++) {
    const DockRef &ref = slotRef(s);
    slotIds[s] = ref.id;
    if (ref.id > 0) continue;
    char qKey[5], idKey[5];
    slotKeys(s, qKey, idKey);
    // Only trust a saved ID if it was found with the same query.
    if (prefs.getString(qKey, "") == ref.query) slotIds[s] = prefs.getInt(idKey, 0);
  }
}

// Looks up docks that docks.json gives only a query for. The same query in
// two slots is searched once. Returns false if any lookup failed.
static bool resolveSlotIds(char *err, int errLen) {
  bool ok = true;
  for (int s = 0; s < NSLOTS; s++) {
    if (slotIds[s] > 0) continue;
    const DockRef &ref = slotRef(s);
    int32_t id = 0;
    for (int o = 0; o < s; o++)
      if (slotIds[o] > 0 && slotRef(o).id == 0 && !strcmp(slotRef(o).query, ref.query)) id = slotIds[o];
    char name[DOCK_NAME_LEN] = "";
    if (!id && !tflSearch(ref.query, id, name, sizeof name, err, errLen)) { ok = false; continue; }
    if (name[0]) Serial.printf("dock: using BikePoints_%ld \"%s\" for %s\n", (long)id, name, ref.label);
    slotIds[s] = id;
    char qKey[5], idKey[5];
    slotKeys(s, qKey, idKey);
    prefs.putString(qKey, ref.query);
    prefs.putInt(idKey, id);
  }
  return ok;
}

// When a screen's numbers last arrived (the older of its two docks), 0 if never.
static time_t screenTime(int screen) {
  time_t a = countsAt[screen * 2], b = countsAt[screen * 2 + 1];
  return (a && b) ? (a < b ? a : b) : 0;
}

static bool screenNeedsFetch(int screen) {
  time_t t = screenTime(screen);
  return !t || time(nullptr) - t > SCREEN_STALE_S;
}

// ------------------------------------------------------------------ data
// Counts for the shown screen come from TfL. One GraphQL request brings
// batteries for every screen's docks, plus counts for the other screens, so
// scrolling to them shows recent numbers without another fetch.
static void refreshData() {
  lastTry = time(nullptr);
  Serial.println("wifi: connecting");
  if (!wifiConnect()) {
    Serial.println("wifi: failed");
    strlcpy(lastErr, "wifi", sizeof lastErr);
    wifiOff();
    return;
  }
  lastRssi = wifiRssi();
  Serial.printf("wifi: ok, %d dBm\n", lastRssi);
  bool clockOk = syncClock();
  Serial.printf("ntp: %s\n", clockOk ? "ok" : "failed");
  lastTry = time(nullptr);

  char err[32] = "";
  if (!resolveSlotIds(err, sizeof err)) Serial.printf("dock: %s\n", err);

  static Dock tfl[MAX_SLOTS], gql[MAX_SLOTS]; // static: too big for the stack together
  memset(tfl, 0, sizeof tfl);
  memset(gql, 0, sizeof gql);
  int shown[2] = {ui.screen * 2, ui.screen * 2 + 1};
  for (int s : shown) {
    if (slotIds[s] <= 0) continue;
    if (s == shown[1] && slotIds[s] == slotIds[shown[0]] && tfl[shown[0]].hasCounts) { tfl[s] = tfl[shown[0]]; continue; }
    bool ok = tflBikePoint(slotIds[s], tfl[s], err, sizeof err);
    Serial.printf("tfl %ld: %s\n", (long)slotIds[s], ok ? tfl[s].name : err);
  }
  char gqlErr[32] = "";
  bool gqlOk = fetchSupply(slotIds, gql, NSLOTS, gqlErr, sizeof gqlErr);
  Serial.printf("gql: %s\n", gqlOk ? "ok" : gqlErr);
  wifiOff();

  time_t now = time(nullptr);
  bool shownOk = true;
  for (int s = 0; s < NSLOTS; s++) {
    Dock &c = cache[s];
    if (c.id != slotIds[s]) { memset(&c, 0, sizeof c); c.id = slotIds[s]; countsAt[s] = 0; }
    if (tfl[s].name[0]) strlcpy(c.name, tfl[s].name, sizeof c.name);
    bool got = true;
    if (tfl[s].hasCounts) copyCounts(c, tfl[s]);
    else if (gql[s].hasCounts) copyCounts(c, gql[s]);
    else got = false;
    if (got) countsAt[s] = now;
    if (!got && (s == shown[0] || s == shown[1])) shownOk = false;
    // Batteries change as e-bikes come and go, so old ones aren't kept.
    copyBatt(c, gql[s]); // hasBatt false if the feed failed or didn't list this dock
    Serial.printf("slot %d dock %ld: %d bikes, %d e-bikes, %d spaces, %d batteries%s%s\n", s, (long)c.id,
                  c.bikes, c.ebikes, c.spaces, c.hasBatt ? c.nBatt : -1, c.closed ? ", closed" : "",
                  got ? "" : ", not updated");
  }
  if (shownOk) {
    lastSync = now;
    lastErr[0] = 0;
    if (!gqlOk) strlcpy(lastErr, gqlErr, sizeof lastErr); // shown on the status screen only
  } else {
    strlcpy(lastErr, err[0] ? err : "no counts", sizeof lastErr);
  }
}

// ------------------------------------------------------------------ drawing
static void fmtClock(time_t ts, const char *prefix, char *out, int len) {
  if (!ts) { snprintf(out, len, "%s never", prefix); return; }
  struct tm t;
  localtime_r(&ts, &t);
  snprintf(out, len, "%s %02d:%02d", prefix, t.tm_hour, t.tm_min);
}

static void drawCurrent() {
  int home = ui.screen * 2, dest = home + 1;
  if (ui.status) {
    StatusInfo info;
    fmtClock(lastSync, "synced", info.synced, sizeof info.synced);
    if (lastErr[0]) snprintf(info.wifi, sizeof info.wifi, "err: %s", lastErr);
    else snprintf(info.wifi, sizeof info.wifi, "wifi %d dBm", lastRssi);
    fmtClock(time(nullptr) + nextWakeSeconds(), "next", info.nextWake, sizeof info.nextWake);
    snprintf(info.docks, sizeof info.docks, "screen %d: %ld/%ld", ui.screen + 1, (long)slotIds[home], (long)slotIds[dest]);
    snprintf(info.version, sizeof info.version, "fw %s", FW_VERSION);
    renderStatus(info);
    return;
  }
  RenderCtx ctx{-1, false, busy, ui.screen, ui.nScreens};
  time_t t = screenTime(ui.screen);
  if (t) {
    struct tm tm;
    localtime_r(&t, &tm);
    ctx.dataMin = tm.tm_hour * 60 + tm.tm_min;
  }
  // "!" when the last fetch didn't refresh this screen.
  ctx.stale = lastTry && (countsAt[home] < lastTry || countsAt[dest] < lastTry);
  renderDocks(cache[home], slotRef(home).label, cache[dest], slotRef(dest).label, ctx);
}

static void show() {
  drawCurrent();
  epdShowPartial();
}

// Fetch, showing "updating" while it runs. Presses right after a fetch just
// redraw, so mashing the dial doesn't hammer TfL.
static void refreshOnScreen() {
  ui.status = false;
  if (lastTry && time(nullptr) - lastTry < MIN_FETCH_GAP_S) { show(); return; }
  busy = true;
  show();
  refreshData();
  busy = false;
  show();
}

// ------------------------------------------------------------------ buttons
static bool pressed(int pin) { return digitalRead(pin) == LOW; }

static UiKey pinToKey(int pin) {
  switch (pin) {
    case KEY_UP: return UK_UP;
    case KEY_DOWN: return UK_DOWN;
    case KEY_BACK: return UK_BACK;
    case KEY_MENU: return UK_MENU;
    default: return UK_OK;
  }
}

// Returns the pin of a debounced press, or NO_KEY.
static int pollButtons() {
  for (int i = 0; i < NKEYS; i++) {
    int pin = KEYS[i];
    if (!pressed(pin)) continue;
    delay(25);
    if (!pressed(pin)) continue;
    uint32_t t0 = millis();
    while (pressed(pin) && millis() - t0 < 1000) delay(10);
    return pin;
  }
  return NO_KEY;
}

// After scrolling (or waking) onto a screen with old numbers, fetch once the
// dial has been still for SCROLL_SETTLE_MS, so scrolling past doesn't fetch.
static uint32_t fetchDueAt = 0;

static void apply(UiKey key) {
  UiAction a = uiHandle(ui, key);
  curScreen = ui.screen;
  fetchDueAt = 0;
  if (a == UA_FETCH) { refreshOnScreen(); return; }
  if (a == UA_REDRAW) show();
  if (!ui.status && screenNeedsFetch(ui.screen)) fetchDueAt = millis() + SCROLL_SETTLE_MS;
}

static void interactiveSession(UiKey first) {
  apply(first);
  uint32_t lastInput = millis();
  while (millis() - lastInput < IDLE_MS) {
    int pin = pollButtons();
    if (pin != NO_KEY) {
      apply(pinToKey(pin));
      lastInput = millis();
      continue;
    }
    if (fetchDueAt && (int32_t)(millis() - fetchDueAt) >= 0) {
      fetchDueAt = 0;
      refreshOnScreen();
      lastInput = millis();
    }
    delay(20);
  }
  if (ui.status) { ui.status = false; show(); }
}

// ------------------------------------------------------------------ sleep
static void sleepNow() {
  uint64_t mask = 0;
  for (int i = 0; i < NKEYS; i++) {
    mask |= 1ULL << KEYS[i];
    rtc_gpio_pullup_en((gpio_num_t)KEYS[i]);
    rtc_gpio_pulldown_dis((gpio_num_t)KEYS[i]);
  }
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  uint32_t secs = nextWakeSeconds();
  esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
  Serial.printf("sleep %lu s\n", (unsigned long)secs);
  Serial.flush();
  esp_deep_sleep_start();
}

// ------------------------------------------------------------------ main
void setup() {
  Serial.begin(115200);
  setenv("TZ", TZ_INFO, 1);
  tzset();
  epdPowerOn();
  for (int i = 0; i < NKEYS; i++) {
    rtc_gpio_deinit((gpio_num_t)KEYS[i]); // hand pins back from RTC after ext1 wake
    pinMode(KEYS[i], INPUT_PULLUP);
  }
  prefs.begin("docks", false);
  loadSlotIds();
  ui.nScreens = NSCREENS;
  ui.screen = curScreen < NSCREENS ? curScreen : 0;
  ui.status = false;

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("boot: fw %s, wake cause %d, screen %d of %d\n", FW_VERSION, (int)cause, ui.screen + 1, NSCREENS);

  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t bits = esp_sleep_get_ext1_wakeup_status();
    UiKey first = UK_OK;
    for (int i = 0; i < NKEYS; i++)
      if (bits & (1ULL << KEYS[i])) first = pinToKey(KEYS[i]);
    interactiveSession(first);
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    // Partial refresh, with a full flash every FULL_REFRESH_EVERY updates,
    // so a 5-minute cadence doesn't flash the screen each time.
    refreshData();
    show();
  } else {
    renderMessage("dock display", "connecting...");
    epdShowFull();
    refreshData();
    drawCurrent();
    epdShowFull();
  }
  sleepNow();
}

void loop() {}
