// Santander Cycles data, fetched straight from TfL. Device only.
//
// Same two sources as the iOS app (CycleHireService / EBikeBatteryService):
//   - TfL Unified API, one BikePoint per dock: bike, e-bike and space counts.
//   - Santander Cycles website GraphQL feed: e-bike battery levels. It only
//     answers for all of London at once (~100s of KB), so it's read as a
//     stream and only our docks are kept.
#pragma once
#include "docks.h"

// TfL search by name. Takes the first match whose name starts with `query`
// (or the first match), and prints every match to Serial.
bool tflSearch(const char *query, int32_t &id, char *name, int nameLen, char *err, int errLen);

// Counts and name for one dock. On failure `out` is untouched.
bool tflBikePoint(int32_t id, Dock &out, char *err, int errLen);

// Batteries (and counts) for up to MAX_SLOTS docks from the GraphQL feed, in
// one request: every screen's docks at once. Slots with id <= 0 are skipped;
// docks not in the feed are left with hasBatt = false. Returns false if the
// feed failed.
bool fetchSupply(const int32_t *ids, Dock *out, int n, char *err, int errLen);
