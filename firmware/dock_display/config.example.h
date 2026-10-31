// Copy to config.h and fill in. config.h is git-ignored.
#pragma once

// Checked by the sketch so a missing config.h fails with a clear message.
// (Without it the compiler silently picks up an unrelated config.h from the
// ESP32 core and reports dozens of "not declared" errors instead.)
#define DOCK_DISPLAY_CONFIG 1

// ---- Network ----
#define WIFI_SSID "your-2.4GHz-ssid"
#define WIFI_PASS "your-password"
#define WIFI_TIMEOUT_MS 15000
#define HTTP_TIMEOUT_MS 15000

// Optional TfL Unified API key (api-portal.tfl.gov.uk). Works without one,
// at a lower rate limit that's still far above what this device uses.
#define TFL_APP_KEY ""

#define NTP_SERVER "pool.ntp.org"
#define TZ_INFO "GMT0BST,M3.5.0/1,M10.5.0" // Europe/London

// ---- Docks ----
// The screens and their docks are in docks.json, not here. Edit them with the
// designer (python3 tools/designer/serve.py) or by hand; tools/flash.sh turns
// docks.json into docks_config.h when it builds.

// ---- Refresh cadence ----
// Every CADENCE_ACTIVE minutes between ACTIVE_FROM and ACTIVE_TO (minutes
// from midnight), every CADENCE_DEFAULT minutes otherwise, and not at all
// from NIGHT_FROM to MORNING_FROM (hours). A dial press fetches fresh data.
#define ACTIVE_FROM (7 * 60)
#define ACTIVE_TO (10 * 60)
#define ACTIVE_WEEKDAYS_ONLY 1
#define CADENCE_ACTIVE 5
#define CADENCE_DEFAULT 30
#define MORNING_FROM 6
#define NIGHT_FROM 23

// ---- Interaction ----
#define IDLE_MS 30000         // deep sleep after this long without a press
#define MIN_FETCH_GAP_S 20    // presses within this of the last fetch just redraw
#define SCROLL_SETTLE_MS 1500 // after scrolling to a screen, wait this long before fetching
#define SCREEN_STALE_S 120    // a screen whose numbers are older than this fetches when shown
#define FULL_REFRESH_EVERY 15 // partial updates before a full flash; 0 = always full
