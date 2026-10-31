#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "net.h"
#include "config.h"
#ifndef DOCK_DISPLAY_CONFIG
#error "config.h missing or out of date: copy config.example.h to config.h in this folder and fill it in"
#endif

bool wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) return false;
    delay(100);
  }
  return true;
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

int wifiRssi() { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }

bool syncClock() {
  configTzTime(TZ_INFO, NTP_SERVER);
  struct tm t;
  return getLocalTime(&t, 10000) && t.tm_year > 120;
}
