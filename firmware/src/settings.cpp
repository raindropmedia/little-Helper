#include "settings.h"

#include <Preferences.h>

Settings S;
static SemaphoreHandle_t s_mx = nullptr;
static Preferences prefs;

void settingsLock() {
  if (!s_mx) s_mx = xSemaphoreCreateRecursiveMutex();
  xSemaphoreTakeRecursive(s_mx, portMAX_DELAY);
}
void settingsUnlock() { xSemaphoreGiveRecursive(s_mx); }

Settings settingsCopy() {
  settingsLock();
  Settings c = S;
  settingsUnlock();
  return c;
}

void settingsLoad() {
  settingsLock();
  prefs.begin("dial", true);
  S.wifiSsid = prefs.getString("ssid", "");
  S.wifiPass = prefs.getString("wpass", "");
  S.webPass = prefs.getString("webpw", "");
  S.hookToken = prefs.getString("hook", "");
  S.tz = prefs.getString("tz", S.tz);
  S.claudeScope = prefs.getString("c_scope", S.claudeScope);
  S.claudeAccess = prefs.getString("c_at", "");
  S.claudeRefresh = prefs.getString("c_rt", "");
  S.claudeExp = prefs.getUInt("c_exp", 0);
  S.msClient = prefs.getString("m_cid", "");
  S.msTenant = prefs.getString("m_ten", S.msTenant);
  S.msAccess = prefs.getString("m_at", "");
  S.msRefresh = prefs.getString("m_rt", "");
  S.msExp = prefs.getUInt("m_exp", 0);
  S.spClient = prefs.getString("s_cid", "");
  S.spRedirect = prefs.getString("s_red", S.spRedirect);
  S.spAccess = prefs.getString("s_at", "");
  S.spRefresh = prefs.getString("s_rt", "");
  S.spExp = prefs.getUInt("s_exp", 0);
  S.bright = prefs.getUChar("bright", S.bright);
  S.sound = prefs.getBool("sound", S.sound);
  S.vol = prefs.getUChar("vol", S.vol);
  S.homeIdx = prefs.getUChar("home", S.homeIdx);
  S.remindIdx = prefs.getUChar("remind", S.remindIdx);
  prefs.end();
  if (S.homeIdx >= HOME_N) S.homeIdx = 2;
  if (S.remindIdx >= REMIND_N) S.remindIdx = 3;
  settingsUnlock();
}

void settingsSave() {
  settingsLock();
  prefs.begin("dial", false);
  prefs.putString("ssid", S.wifiSsid);
  prefs.putString("wpass", S.wifiPass);
  prefs.putString("webpw", S.webPass);
  prefs.putString("hook", S.hookToken);
  prefs.putString("tz", S.tz);
  prefs.putString("c_scope", S.claudeScope);
  prefs.putString("c_at", S.claudeAccess);
  prefs.putString("c_rt", S.claudeRefresh);
  prefs.putUInt("c_exp", S.claudeExp);
  prefs.putString("m_cid", S.msClient);
  prefs.putString("m_ten", S.msTenant);
  prefs.putString("m_at", S.msAccess);
  prefs.putString("m_rt", S.msRefresh);
  prefs.putUInt("m_exp", S.msExp);
  prefs.putString("s_cid", S.spClient);
  prefs.putString("s_red", S.spRedirect);
  prefs.putString("s_at", S.spAccess);
  prefs.putString("s_rt", S.spRefresh);
  prefs.putUInt("s_exp", S.spExp);
  prefs.putUChar("bright", S.bright);
  prefs.putBool("sound", S.sound);
  prefs.putUChar("vol", S.vol);
  prefs.putUChar("home", S.homeIdx);
  prefs.putUChar("remind", S.remindIdx);
  prefs.end();
  settingsUnlock();
}
