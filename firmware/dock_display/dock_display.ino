// Dock display: CrowPanel ESP32-S3 2.13" e-paper (250x122, B/W).
//
// Shows live Santander Cycles numbers for two hard-coded docks: the home
// dock (bikes, e-bikes and each e-bike's battery) and the destination dock
// (free spaces to drop off). Data comes straight from TfL; no server of our own.
//
// Controls (pins from Elecrow's 2.13_key example):
//   any key except Menu   fetch fresh data now
//   menu                  status screen (press again, or Back, to leave)
//
// Board: ESP32S3 Dev Module, FlashSize=8M. Libraries: ArduinoJson v7.
// Display driver files (EPD.*, EPD_Init.*, EPDfont.h, spi.*) are copied from
// Elecrow's repo by tools/fetch_driver.sh.

#include <Arduino.h>
#include <Preferences.h>
#include <time.h>
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#include "config.h"
#ifndef DOCK_DISPLAY_CONFIG
#error "config.h missing or out of date: copy config.example.h to config.h in this folder and fill it in"
#endif
#include "docks.h"
#include "gfx.h"
#include "render.h"
#include "epd_io.h"
#include "net.h"
#include "santander.h"

#define FW_VERSION "0.1.0"

// ---- Buttons (active low) ----
static const int KEY_MENU = 2;  // HOME_KEY in Elecrow example
static const int KEY_BACK = 1;  // EXIT_KEY
static const int KEY_UP = 6;    // PRV_KEY  (dial up)
static const int KEY_DOWN = 4;  // NEXT_KEY (dial down)
static const int KEY_OK = 5;    // OK_KEY   (dial press)
static const int KEYS[] = {KEY_MENU, KEY_BACK, KEY_UP, KEY_DOWN, KEY_OK};
static const int NKEYS = sizeof(KEYS) / sizeof(KEYS[0]);

enum Event { EV_NONE, EV_REFRESH, EV_BACK, EV_MENU };

// ---- Docks ----
struct DockConfig {
  int32_t id;
  const char *query;
  const char *label;
};
static const DockConfig DOCKS[NDOCKS] = {
    {HOME_DOCK_ID, HOME_DOCK_QUERY, HOME_DOCK_LABEL},
    {DEST_DOCK_ID, DEST_DOCK_QUERY, DEST_DOCK_LABEL},
};
static int32_t dockIds[NDOCKS]; // from config, or looked up once and kept in flash

// ---- State kept across deep sleep ----
RTC_DATA_ATTR static Dock cache[NDOCKS];
RTC_DATA_ATTR static time_t lastSync = 0;  // last fetch that got counts for every dock
RTC_DATA_ATTR static time_t lastTry = 0;   // last fetch attempt
RTC_DATA_ATTR static bool lastFetchOk = false;
RTC_DATA_ATTR static int lastRssi = 0;
RTC_DATA_ATTR static char lastErr[32] = "";

static Preferences prefs;
static bool showStatus = false;
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
static void loadDockIds() {
  for (int i = 0; i < NDOCKS; i++) {
    dockIds[i] = DOCKS[i].id;
    if (dockIds[i] > 0) continue;
    char qKey[4], idKey[4];
    snprintf(qKey, sizeof qKey, "q%d", i);
    snprintf(idKey, sizeof idKey, "id%d", i);
    // Only trust the saved ID if it was found with the same query.
    if (prefs.getString(qKey, "") == DOCKS[i].query) dockIds[i] = prefs.getInt(idKey, 0);
  }
}

static bool resolveDockIds(char *err, int errLen) {
  for (int i = 0; i < NDOCKS; i++) {
    if (dockIds[i] > 0) continue;
    int32_t id = 0;
    char name[DOCK_NAME_LEN];
    if (!tflSearch(DOCKS[i].query, id, name, sizeof name, err, errLen)) return false;
    Serial.printf("dock: using BikePoints_%ld \"%s\" for %s\n", (long)id, name, DOCKS[i].label);
    dockIds[i] = id;
    char qKey[4], idKey[4];
    snprintf(qKey, sizeof qKey, "q%d", i);
    snprintf(idKey, sizeof idKey, "id%d", i);
    prefs.putString(qKey, DOCKS[i].query);
    prefs.putInt(idKey, id);
  }
  return true;
}

// ------------------------------------------------------------------ data
static void refreshData() {
  lastTry = time(nullptr);
  Serial.println("wifi: connecting");
  if (!wifiConnect()) {
    Serial.println("wifi: failed");
    lastFetchOk = false;
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
  if (!resolveDockIds(err, sizeof err)) {
    Serial.printf("dock: %s\n", err);
    lastFetchOk = false;
    strlcpy(lastErr, err, sizeof lastErr);
    wifiOff();
    return;
  }

  // Counts from TfL, batteries (and backup counts) from the GraphQL feed.
  Dock tfl[NDOCKS], gql[NDOCKS];
  memset(tfl, 0, sizeof tfl);
  memset(gql, 0, sizeof gql);
  for (int i = 0; i < NDOCKS; i++) {
    bool ok = tflBikePoint(dockIds[i], tfl[i], err, sizeof err);
    Serial.printf("tfl %ld: %s\n", (long)dockIds[i], ok ? tfl[i].name : err);
  }
  char gqlErr[32] = "";
  bool gqlOk = fetchSupply(dockIds, gql, NDOCKS, gqlErr, sizeof gqlErr);
  Serial.printf("gql: %s\n", gqlOk ? "ok" : gqlErr);
  wifiOff();

  bool allCounts = true;
  for (int i = 0; i < NDOCKS; i++) {
    Dock &c = cache[i];
    if (c.id != dockIds[i]) { memset(&c, 0, sizeof c); c.id = dockIds[i]; }
    if (tfl[i].name[0]) strlcpy(c.name, tfl[i].name, sizeof c.name);
    if (tfl[i].hasCounts) copyCounts(c, tfl[i]);
    else if (gql[i].hasCounts) copyCounts(c, gql[i]);
    else allCounts = false;
    // Batteries change as e-bikes come and go, so old ones aren't kept.
    copyBatt(c, gql[i]); // hasBatt false if the feed failed or didn't list this dock
    Serial.printf("dock %ld: %d bikes, %d e-bikes, %d spaces, %d batteries%s\n", (long)c.id, c.bikes,
                  c.ebikes, c.spaces, c.hasBatt ? c.nBatt : -1, c.closed ? ", closed" : "");
  }
  lastFetchOk = allCounts;
  if (allCounts) {
    lastSync = time(nullptr);
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
  if (showStatus) {
    StatusInfo info;
    fmtClock(lastSync, "synced", info.synced, sizeof info.synced);
    if (lastErr[0]) snprintf(info.wifi, sizeof info.wifi, "err: %s", lastErr);
    else snprintf(info.wifi, sizeof info.wifi, "wifi %d dBm", lastRssi);
    fmtClock(time(nullptr) + nextWakeSeconds(), "next", info.nextWake, sizeof info.nextWake);
    snprintf(info.docks, sizeof info.docks, "docks %ld / %ld", (long)dockIds[0], (long)dockIds[1]);
    snprintf(info.version, sizeof info.version, "fw %s", FW_VERSION);
    renderStatus(info);
    return;
  }
  RenderCtx ctx{-1, !lastFetchOk, busy};
  if (lastSync) {
    struct tm t;
    localtime_r(&lastSync, &t);
    ctx.dataMin = t.tm_hour * 60 + t.tm_min;
  }
  renderDocks(cache[0], DOCKS[0].label, cache[1], DOCKS[1].label, ctx);
}

// Fetch, showing "updating" while it runs. Presses right after a fetch just
// redraw, so mashing the dial doesn't hammer TfL.
static void refreshOnScreen() {
  showStatus = false;
  if (lastTry && time(nullptr) - lastTry < MIN_FETCH_GAP_S) { drawCurrent(); epdShowPartial(); return; }
  busy = true;
  drawCurrent();
  epdShowPartial();
  refreshData();
  busy = false;
  drawCurrent();
  epdShowPartial();
}

// ------------------------------------------------------------------ buttons
static bool pressed(int pin) { return digitalRead(pin) == LOW; }

static Event keyToEvent(int pin) {
  if (pin == KEY_MENU) return EV_MENU;
  if (pin == KEY_BACK) return EV_BACK;
  return EV_REFRESH;
}

static Event pollButtons() {
  for (int i = 0; i < NKEYS; i++) {
    int pin = KEYS[i];
    if (!pressed(pin)) continue;
    delay(25);
    if (!pressed(pin)) continue;
    uint32_t t0 = millis();
    while (pressed(pin) && millis() - t0 < 1000) delay(10);
    return keyToEvent(pin);
  }
  return EV_NONE;
}

static void handle(Event e) {
  switch (e) {
    case EV_REFRESH:
      refreshOnScreen();
      break;
    case EV_MENU:
      showStatus = !showStatus;
      drawCurrent();
      epdShowPartial();
      break;
    case EV_BACK:
      if (showStatus) { showStatus = false; drawCurrent(); epdShowPartial(); }
      else refreshOnScreen();
      break;
    default:
      break;
  }
}

static void interactiveSession(Event first) {
  handle(first);
  uint32_t lastInput = millis();
  while (millis() - lastInput < IDLE_MS) {
    Event e = pollButtons();
    if (e == EV_NONE) { delay(20); continue; }
    handle(e);
    lastInput = millis();
  }
  if (showStatus) { showStatus = false; drawCurrent(); epdShowPartial(); }
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
  loadDockIds();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("boot: fw %s, wake cause %d\n", FW_VERSION, (int)cause);

  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t bits = esp_sleep_get_ext1_wakeup_status();
    Event first = EV_REFRESH;
    for (int i = 0; i < NKEYS; i++)
      if (bits & (1ULL << KEYS[i])) first = keyToEvent(KEYS[i]);
    interactiveSession(first);
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    // Partial refresh, with a full flash every FULL_REFRESH_EVERY updates,
    // so a 5-minute cadence doesn't flash the screen each time.
    refreshData();
    drawCurrent();
    epdShowPartial();
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
