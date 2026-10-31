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
// HOME is where you pick a bike up, DEST is where you drop it off.
// *_ID is the number in TfL's "BikePoints_<n>". Leave it 0 and the device
// looks the dock up once with TfL's search (*_QUERY), saves the ID in flash,
// and prints every match to the serial log. Put the ID here once you've
// checked it's the right dock. OpenStreetMap lists Wenlock Road as 42.
#define HOME_DOCK_ID 0
#define HOME_DOCK_QUERY "Wenlock Road"
#define HOME_DOCK_LABEL "WENLOCK RD"
#define DEST_DOCK_ID 0
#define DEST_DOCK_QUERY "Old Street Station"
#define DEST_DOCK_LABEL "OLD ST"

// ---- Refresh cadence ----
// Every CADENCE_ACTIVE minutes between ACTIVE_FROM and ACTIVE_TO (minutes
// from midnight), every CADENCE_DEFAULT minutes otherwise, and not at all
// from NIGHT_FROM to MORNING_FROM (hours). Any button fetches fresh data.
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
#define FULL_REFRESH_EVERY 15 // partial updates before a full flash; 0 = always full
