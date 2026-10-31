// Wi-Fi and clock. Device only.
#pragma once

bool wifiConnect();
void wifiOff();
bool syncClock(); // NTP; returns true once local time is valid
int wifiRssi();
