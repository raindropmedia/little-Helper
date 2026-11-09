#pragma once
#include <Arduino.h>

// Dauerhaft im NVS gespeicherte Konfiguration (Webkonfiguration + Gerät)
struct Settings {
  // WLAN / Web
  String wifiSsid, wifiPass;
  String webPass;    // optional: Passwort der Webkonfiguration (Benutzer "admin")
  String hookToken;  // optional: Token für /api/ring und /api/notify
  String tz = "CET-1CEST,M3.5.0,M10.5.0/3";  // POSIX-Zeitzone (Europe/Berlin)

  // Claude (OAuth, eigener Login des Dials)
  String claudeScope = "user:profile";
  String claudeAccess, claudeRefresh;
  uint32_t claudeExp = 0;  // Unix-Zeit

  // Microsoft Graph (Teams-Status + Kalender)
  String msClient, msTenant = "organizations";
  String msAccess, msRefresh;
  uint32_t msExp = 0;

  // Spotify
  String spClient, spRedirect = "http://127.0.0.1:8888/callback";
  String spAccess, spRefresh;
  uint32_t spExp = 0;

  // Gerät
  uint8_t bright = 160;
  bool sound = true;
  uint8_t vol = 140;
  uint8_t homeIdx = 2;
  uint8_t remindIdx = 3;
};

extern Settings S;

void settingsLoad();
void settingsSave();               // alles speichern
void settingsLock();               // Zugriff aus mehreren Tasks schützen
void settingsUnlock();

// Kopie unter Sperre
Settings settingsCopy();

constexpr uint16_t HOME_OPTS[] = {0, 30, 60, 120, 300};
constexpr uint8_t HOME_N = sizeof(HOME_OPTS) / sizeof(HOME_OPTS[0]);
constexpr uint8_t REMIND_OPTS[] = {0, 1, 2, 5, 10, 15};
constexpr uint8_t REMIND_N = sizeof(REMIND_OPTS) / sizeof(REMIND_OPTS[0]);
