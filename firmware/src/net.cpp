// Netzwerk-Task (Core 0): WLAN, Zeit, OAuth-Logins und alle HTTPS-Abfragen.
// Es läuft immer nur eine TLS-Verbindung gleichzeitig (Heap ohne PSRAM ist knapp).
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <initializer_list>
#include <utility>

#include "config.h"
#include "settings.h"
#include "state.h"

// Root-Zertifikate (Mozilla), eingebettet über board_build.embed_files
extern const uint8_t crt_bundle_start[] asm("_binary_data_x509_crt_bundle_bin_start");

// ----------------------------------------------------------- OAuth-Konstanten
// Claude: dieselben öffentlichen Werte wie der Claude-Code-Login (manueller Code-Flow)
static const char* CL_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
static const char* CL_AUTHORIZE = "https://claude.com/cai/oauth/authorize";
static const char* CL_TOKEN = "https://platform.claude.com/v1/oauth/token";
static const char* CL_REDIRECT = "https://platform.claude.com/oauth/code/callback";
static const char* CL_USAGE = "https://api.anthropic.com/api/oauth/usage";
static const char* CL_BETA = "oauth-2025-04-20";

static const char* MS_SCOPES = "Presence.ReadWrite Calendars.Read User.Read offline_access";
static const char* GRAPH = "https://graph.microsoft.com/v1.0";

static const char* SP_SCOPES = "user-read-playback-state user-modify-playback-state";

// ----------------------------------------------------------- Zustand
static AppState s_st;
static SemaphoreHandle_t s_mx;
static QueueHandle_t s_jobs;
static volatile Focus s_focus = Focus::None;
static String s_clVerifier, s_clState, s_spVerifier, s_spState;
static String s_msDeviceCode, s_msUserId;
static uint32_t s_msPollAt = 0, s_msInterval = 5, s_msExpiresAt = 0;

#define LOCK() xSemaphoreTake(s_mx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mx)

template <size_t N>
static void setStr(char (&dst)[N], const char* src) {
  strlcpy(dst, src ? src : "", N);
}

static void touch() { s_st.seq++; }

// ----------------------------------------------------------- Hilfsfunktionen
static time_t nowUtc() { return time(nullptr); }
static bool timeValid() { return nowUtc() > 1700000000; }

static String urlEncode(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String o;
  o.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~')
      o += c;
    else {
      o += '%';
      o += hex[(c >> 4) & 0xF];
      o += hex[c & 0xF];
    }
  }
  return o;
}

static String b64url(const uint8_t* data, size_t len) {
  unsigned char buf[96];
  size_t olen = 0;
  mbedtls_base64_encode(buf, sizeof(buf), &olen, data, len);
  String s;
  for (size_t i = 0; i < olen; i++) {
    char c = buf[i];
    if (c == '+') c = '-';
    else if (c == '/') c = '_';
    else if (c == '=') continue;
    s += c;
  }
  return s;
}

static String randomToken(size_t bytes) {
  uint8_t b[48];
  for (size_t i = 0; i < bytes; i++) b[i] = esp_random() & 0xFF;
  return b64url(b, bytes);
}

static String pkceChallenge(const String& verifier) {
  uint8_t h[32];
  mbedtls_sha256_ret((const unsigned char*)verifier.c_str(), verifier.length(), h, 0);
  return b64url(h, 32);
}

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (int64_t)doe - 719468;
}

// ISO-8601 -> Unix-Zeit (UTC). Ohne Zeitzonenangabe wird UTC angenommen.
static time_t parseIso(const char* s) {
  if (!s || strlen(s) < 19) return 0;
  int Y, M, D, h, m, sec;
  if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int64_t t = daysFromCivil(Y, M, D) * 86400LL + h * 3600 + m * 60 + sec;
  const char* p = s + 19;
  while (*p == '.' || isdigit((unsigned char)*p)) p++;
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    sscanf(p + 1, "%2d:%2d", &oh, &om);
    int off = oh * 3600 + om * 60;
    t += (*p == '+') ? -off : off;
  }
  return (time_t)t;
}

static String isoUtc(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return b;
}

static String queryParam(const String& url, const char* key) {
  String k = String(key) + "=";
  int i = url.indexOf("?" + k);
  if (i < 0) i = url.indexOf("&" + k);
  if (i < 0) i = url.indexOf("#" + k);
  if (i < 0) return url.startsWith(k) ? url.substring(k.length()) : "";
  int s = i + 1 + k.length();
  int e = s;
  while (e < (int)url.length() && url[e] != '&' && url[e] != '#') e++;
  String v = url.substring(s, e);
  v.replace("%2F", "/");
  v.replace("%3D", "=");
  return v;
}

// ----------------------------------------------------------- HTTPS
using Hdr = std::pair<const char*, String>;
static char s_netErr[40] = "";  // letzte Verbindungs-Fehlerursache
static uint32_t s_retryAfter = 0;  // Sekunden aus "Retry-After" der letzten 429-Antwort

// Führt eine HTTPS-Anfrage aus. Antwort-JSON (optional gefiltert) landet in out.
static int https(const char* method, const String& url, const String& body, const char* ctype,
                 std::initializer_list<Hdr> headers, JsonDocument* out, JsonDocument* filter = nullptr) {
  WiFiClientSecure client;
  client.setCACertBundle(crt_bundle_start);
  client.setTimeout(12);
  HTTPClient http;
  http.useHTTP10(true);  // kein Chunked-Encoding -> Stream direkt parsebar
  http.setTimeout(12000);
  http.setConnectTimeout(8000);
  http.setUserAgent("ClaudeDial/2.0");
  if (!http.begin(client, url)) return -1;
  for (const auto& h : headers) http.addHeader(h.first, h.second);
  if (ctype) http.addHeader("Content-Type", ctype);
  if (body.length() == 0 && strcmp(method, "GET") != 0) http.addHeader("Content-Length", "0");
  static const char* hdrKeys[] = {"Retry-After"};
  http.collectHeaders(hdrKeys, 1);
  int code = http.sendRequest(method, body);
  s_retryAfter = code == 429 ? (uint32_t)http.header("Retry-After").toInt() : 0;
  if (code < 0) {  // Ursache merken: TLS-Fehlercode oder Verbindungsproblem
    char buf[64];
    int e = client.lastError(buf, sizeof(buf));
    const char* why = e == -0x2700 ? "Zertifikat" : e == -0x7F00 || e == -0x10 ? "kein RAM" : e ? "TLS" : "Verbindung";
    if (e) snprintf(s_netErr, sizeof(s_netErr), "%s -0x%X", why, -e);
    else snprintf(s_netErr, sizeof(s_netErr), "%s (%d), RAM %uk", why, code, (unsigned)(ESP.getMaxAllocHeap() / 1024));
    log_w("%s %s fehlgeschlagen: %s / %s", method, url.c_str(), s_netErr, buf);
  }
  if (out && code > 0) {
    DeserializationError e = filter ? deserializeJson(*out, http.getStream(), DeserializationOption::Filter(*filter))
                                    : deserializeJson(*out, http.getStream());
    if (e && code == 200) log_w("JSON-Fehler %s bei %s", e.c_str(), url.c_str());
  }
  http.end();
  log_i("%s %s -> %d (heap %u)", method, url.c_str(), code, ESP.getFreeHeap());
  return code;
}

static void httpErr(char* dst, size_t n, int code) {
  if (code < 0) strlcpy(dst, s_netErr, n);
  else snprintf(dst, n, "HTTP %d", code);
}

// ----------------------------------------------------------- Claude
String netClaudeLoginUrl() {
  LOCK();
  s_clVerifier = randomToken(32);
  s_clState = randomToken(24);
  String v = s_clVerifier, st = s_clState;
  UNLOCK();
  Settings c = settingsCopy();
  return String(CL_AUTHORIZE) + "?code=true&client_id=" + CL_CLIENT_ID + "&response_type=code&redirect_uri=" +
         urlEncode(CL_REDIRECT) + "&scope=" + urlEncode(c.claudeScope) + "&code_challenge=" + pkceChallenge(v) +
         "&code_challenge_method=S256&state=" + st;
}

static bool claudeStoreTokens(JsonDocument& doc) {
  const char* at = doc["access_token"] | "";
  if (!*at) return false;
  settingsLock();
  S.claudeAccess = at;
  const char* rt = doc["refresh_token"] | "";
  if (*rt) S.claudeRefresh = rt;
  uint32_t ex = doc["expires_in"] | 0;
  S.claudeExp = (timeValid() && ex) ? nowUtc() + ex : 0;
  settingsUnlock();
  settingsSave();
  return true;
}

static void claudeExchange(const char* pasted) {
  String p = pasted;
  p.trim();
  String code = p, state;
  int hash = p.indexOf('#');
  if (hash >= 0) {
    code = p.substring(0, hash);
    state = p.substring(hash + 1);
  }
  if (p.startsWith("http")) {  // ganze Callback-URL eingefügt
    code = queryParam(p, "code");
    state = queryParam(p, "state");
  }
  LOCK();
  String verifier = s_clVerifier, expState = s_clState;
  UNLOCK();
  JsonDocument req, res;
  req["grant_type"] = "authorization_code";
  req["code"] = code;
  req["redirect_uri"] = CL_REDIRECT;
  req["client_id"] = CL_CLIENT_ID;
  req["code_verifier"] = verifier;
  req["state"] = state.length() ? state : expState;
  String body;
  serializeJson(req, body);
  int rc = verifier.length() ? https("POST", CL_TOKEN, body, "application/json", {}, &res) : -100;
  LOCK();
  if (rc == 200 && claudeStoreTokens(res)) {
    s_st.claude.linked = true;
    setStr(s_st.claude.err, "");
  } else if (rc == -100) {
    setStr(s_st.claude.err, "Login neu starten");
  } else {
    httpErr(s_st.claude.err, sizeof(s_st.claude.err), rc);
    log_w("Claude-Token-Tausch fehlgeschlagen: %d %s", rc, (const char*)(res["error_description"] | res["error"] | ""));
  }
  touch();
  UNLOCK();
}

static bool claudeRefresh() {
  Settings c = settingsCopy();
  if (!c.claudeRefresh.length()) return false;
  JsonDocument req, res;
  req["grant_type"] = "refresh_token";
  req["refresh_token"] = c.claudeRefresh;
  req["client_id"] = CL_CLIENT_ID;
  req["scope"] = c.claudeScope;
  String body;
  serializeJson(req, body);
  int rc = https("POST", CL_TOKEN, body, "application/json", {}, &res);
  if (rc == 200 && claudeStoreTokens(res)) return true;
  LOCK();
  snprintf(s_st.claude.err, sizeof(s_st.claude.err), "Token-Refresh %d", rc);
  if (rc == 400 || rc == 401) {  // Refresh-Token ungültig -> neu anmelden
    setStr(s_st.claude.err, "Bitte neu anmelden");
  }
  touch();
  UNLOCK();
  return false;
}

static void parseWindow(JsonVariantConst w, float& util, time_t& reset) {
  if (w.isNull()) {
    util = -1;
    reset = 0;
    return;
  }
  util = w["utilization"] | -1.0f;
  reset = parseIso(w["resets_at"] | "");
}

// Rate-Limit der Nutzungs-Schnittstelle: Wartezeit nach HTTP 429
static uint32_t s_clBlockedUntil = 0, s_clBackoffS = 300;

static void claudePoll() {
  if (s_clBlockedUntil && (int32_t)(millis() - s_clBlockedUntil) < 0) return;  // noch gesperrt
  Settings c = settingsCopy();
  if (!c.claudeAccess.length()) return;
  if (timeValid() && c.claudeExp && nowUtc() > (time_t)c.claudeExp - 120) {
    if (!claudeRefresh()) return;
    c = settingsCopy();
  }
  JsonDocument doc;
  int rc = https("GET", CL_USAGE, "", nullptr,
                 {{"Authorization", "Bearer " + c.claudeAccess}, {"anthropic-beta", CL_BETA}}, &doc);
  if (rc == 401 && claudeRefresh()) {
    c = settingsCopy();
    doc.clear();
    rc = https("GET", CL_USAGE, "", nullptr,
               {{"Authorization", "Bearer " + c.claudeAccess}, {"anthropic-beta", CL_BETA}}, &doc);
  }
  LOCK();
  ClaudeUsage& u = s_st.claude;
  if (rc == 200) {
    u.ok = true;
    setStr(u.err, "");
    parseWindow(doc["five_hour"], u.fiveH, u.fiveReset);
    parseWindow(doc["seven_day"], u.week, u.weekReset);
    parseWindow(doc["seven_day_opus"], u.opus, u.opusReset);
    parseWindow(doc["seven_day_sonnet"], u.sonnet, u.sonnetReset);
    JsonVariantConst ex = doc["extra_usage"];
    u.extraEnabled = ex["is_enabled"] | false;
    u.extraUtil = ex["utilization"] | -1.0f;
    u.extraUsed = ex["used_credits"] | 0.0f;
    u.extraLimit = ex["monthly_limit"] | 0.0f;
    u.updated = nowUtc();
    s_clBackoffS = 300;
    s_clBlockedUntil = 0;
  } else if (rc == 429) {
    // Server-Vorgabe beachten, sonst exponentiell warten (5 min ... 1 h)
    uint32_t wait = max(s_retryAfter, s_clBackoffS);
    s_clBackoffS = min<uint32_t>(s_clBackoffS * 2, 3600);
    s_clBlockedUntil = millis() + wait * 1000UL;
    if (!s_clBlockedUntil) s_clBlockedUntil = 1;
    u.ok = u.updated != 0;
    if (timeValid()) {
      time_t next = nowUtc() + wait;
      struct tm tm;
      localtime_r(&next, &tm);
      snprintf(u.err, sizeof(u.err), "Rate-Limit, neu um %02d:%02d", tm.tm_hour, tm.tm_min);
    } else {
      snprintf(u.err, sizeof(u.err), "Rate-Limit, warte %lu min", (unsigned long)(wait / 60));
    }
    log_w("Claude-Nutzung: 429, warte %lu s", (unsigned long)wait);
  } else {
    u.ok = u.updated != 0;  // alte Werte weiter anzeigen
    if (rc == 403) setStr(u.err, "Scope fehlt (user:profile)");
    else httpErr(u.err, sizeof(u.err), rc);
  }
  touch();
  UNLOCK();
}

// ----------------------------------------------------------- Microsoft Graph
static String msAuthority(const Settings& c) {
  return "https://login.microsoftonline.com/" + (c.msTenant.length() ? c.msTenant : String("organizations")) +
         "/oauth2/v2.0";
}

static bool msStoreTokens(JsonDocument& doc) {
  const char* at = doc["access_token"] | "";
  if (!*at) return false;
  settingsLock();
  S.msAccess = at;
  const char* rt = doc["refresh_token"] | "";
  if (*rt) S.msRefresh = rt;
  uint32_t ex = doc["expires_in"] | 3600;
  S.msExp = timeValid() ? nowUtc() + ex : 0;
  settingsUnlock();
  settingsSave();
  return true;
}

static void msStart() {
  Settings c = settingsCopy();
  LOCK();
  s_st.ms = MsLogin();
  if (!c.msClient.length()) {
    setStr(s_st.ms.msg, "Client-ID fehlt (Webkonfiguration)");
    touch();
    UNLOCK();
    return;
  }
  UNLOCK();
  JsonDocument res;
  String body = "client_id=" + urlEncode(c.msClient) + "&scope=" + urlEncode(MS_SCOPES);
  int rc = https("POST", msAuthority(c) + "/devicecode", body, "application/x-www-form-urlencoded", {}, &res);
  LOCK();
  if (rc == 200) {
    s_msDeviceCode = (const char*)(res["device_code"] | "");
    s_msInterval = res["interval"] | 5;
    s_msExpiresAt = millis() + (uint32_t)(res["expires_in"] | 900) * 1000UL;
    s_msPollAt = millis() + s_msInterval * 1000UL;
    s_st.ms.pending = true;
    setStr(s_st.ms.userCode, res["user_code"] | "");
    setStr(s_st.ms.url, res["verification_uri"] | "https://microsoft.com/devicelogin");
    setStr(s_st.ms.msg, "Code im Browser eingeben");
  } else {
    snprintf(s_st.ms.msg, sizeof(s_st.ms.msg), "Fehler %d: %s", rc, (const char*)(res["error"] | ""));
  }
  touch();
  UNLOCK();
}

static void msDevicePoll() {
  if (!s_st.ms.pending || millis() < s_msPollAt) return;
  if (millis() > s_msExpiresAt) {
    LOCK();
    s_st.ms.pending = false;
    setStr(s_st.ms.msg, "Code abgelaufen");
    touch();
    UNLOCK();
    return;
  }
  Settings c = settingsCopy();
  JsonDocument res;
  String body = "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" + urlEncode(c.msClient) +
                "&device_code=" + urlEncode(s_msDeviceCode);
  int rc = https("POST", msAuthority(c) + "/token", body, "application/x-www-form-urlencoded", {}, &res);
  s_msPollAt = millis() + s_msInterval * 1000UL;
  const char* err = res["error"] | "";
  LOCK();
  if (rc == 200 && msStoreTokens(res)) {
    s_st.ms.pending = false;
    s_st.msLinked = true;
    setStr(s_st.ms.msg, "Microsoft verbunden");
    s_msUserId = "";
  } else if (!strcmp(err, "slow_down")) {
    s_msInterval += 5;
  } else if (strcmp(err, "authorization_pending") != 0) {
    s_st.ms.pending = false;
    snprintf(s_st.ms.msg, sizeof(s_st.ms.msg), "Abbruch: %s", err);
  }
  touch();
  UNLOCK();
}

static bool msRefresh() {
  Settings c = settingsCopy();
  if (!c.msRefresh.length()) return false;
  JsonDocument res;
  String body = "grant_type=refresh_token&client_id=" + urlEncode(c.msClient) + "&refresh_token=" +
                urlEncode(c.msRefresh) + "&scope=" + urlEncode(MS_SCOPES);
  int rc = https("POST", msAuthority(c) + "/token", body, "application/x-www-form-urlencoded", {}, &res);
  if (rc == 200 && msStoreTokens(res)) return true;
  LOCK();
  snprintf(s_st.teams.err, sizeof(s_st.teams.err), "MS-Refresh %d", rc);
  setStr(s_st.cal.err, s_st.teams.err);
  touch();
  UNLOCK();
  return false;
}

// Graph-Anfrage mit automatischem Token-Refresh
static int graph(const char* method, const String& path, const String& body, JsonDocument* out,
                 JsonDocument* filter = nullptr, bool utcPrefer = false) {
  Settings c = settingsCopy();
  if (!c.msAccess.length()) return -2;
  if (timeValid() && c.msExp && nowUtc() > (time_t)c.msExp - 120) {
    if (!msRefresh()) return 401;
    c = settingsCopy();
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    int rc = utcPrefer ? https(method, String(GRAPH) + path, body, body.length() ? "application/json" : nullptr,
                               {{"Authorization", "Bearer " + c.msAccess}, {"Prefer", "outlook.timezone=\"UTC\""}}, out,
                               filter)
                       : https(method, String(GRAPH) + path, body, body.length() ? "application/json" : nullptr,
                               {{"Authorization", "Bearer " + c.msAccess}}, out, filter);
    if (rc != 401 || attempt == 1) return rc;
    if (!msRefresh()) return rc;
    c = settingsCopy();
    if (out) out->clear();
  }
  return -1;
}

static void teamsPoll() {
  JsonDocument doc;
  int rc = graph("GET", "/me/presence", "", &doc);
  LOCK();
  TeamsState& t = s_st.teams;
  if (rc == 200) {
    char oldAct[24];
    setStr(oldAct, t.activity);
    t.ok = true;
    setStr(t.err, "");
    setStr(t.availability, doc["availability"] | "PresenceUnknown");
    setStr(t.activity, doc["activity"] | "");
    bool wasCall = !strcmp(oldAct, "InACall") || !strcmp(oldAct, "InAConferenceCall");
    bool isCall = !strcmp(t.activity, "InACall") || !strcmp(t.activity, "InAConferenceCall");
    if (isCall && !wasCall && oldAct[0]) {
      s_st.ringing = false;
      s_st.notifyId++;
      setStr(s_st.notifyText, "Teams: Anruf laeuft");
      setStr(s_st.notifyLevel, "call");
    }
  } else {
    t.ok = false;
    httpErr(t.err, sizeof(t.err), rc);
  }
  touch();
  UNLOCK();
}

static void setPresence(const char* key) {
  if (!s_msUserId.length()) {
    JsonDocument me;
    if (graph("GET", "/me?$select=id", "", &me) == 200) s_msUserId = (const char*)(me["id"] | "");
  }
  if (!s_msUserId.length()) return;
  String base = "/users/" + s_msUserId + "/presence/";
  int rc;
  if (!strcmp(key, "Reset")) {
    rc = graph("POST", base + "clearUserPreferredPresence", "{}", nullptr);
  } else {
    const char* act = !strcmp(key, "Offline") ? "OffWork" : key;
    String body = String("{\"availability\":\"") + key + "\",\"activity\":\"" + act + "\",\"expirationDuration\":\"PT4H\"}";
    rc = graph("POST", base + "setUserPreferredPresence", body, nullptr);
  }
  if (rc != 200 && rc != 204) {
    LOCK();
    snprintf(s_st.teams.err, sizeof(s_st.teams.err), "Setzen: HTTP %d", rc);
    s_st.notifyId++;
    setStr(s_st.notifyText, s_st.teams.err);
    setStr(s_st.notifyLevel, "warn");
    touch();
    UNLOCK();
  }
  teamsPoll();
}

static void calendarPoll() {
  if (!timeValid()) return;
  time_t now = nowUtc();
  String path = "/me/calendarView?startDateTime=" + isoUtc(now - 3600) + "&endDateTime=" + isoUtc(now + 36 * 3600) +
                "&$select=subject,start,end,location,isAllDay,showAs,isCancelled&$orderby=start/dateTime&$top=15";
  JsonDocument filter, doc;
  JsonObject f = filter["value"][0].to<JsonObject>();
  f["subject"] = true;
  f["start"]["dateTime"] = true;
  f["end"]["dateTime"] = true;
  f["location"]["displayName"] = true;
  f["isAllDay"] = true;
  f["showAs"] = true;
  f["isCancelled"] = true;
  int rc = graph("GET", path, "", &doc, &filter, true);
  LOCK();
  CalendarState& c = s_st.cal;
  if (rc == 200) {
    c.ok = true;
    setStr(c.err, "");
    c.n = 0;
    for (JsonObjectConst e : doc["value"].as<JsonArrayConst>()) {
      if (c.n >= 8) break;
      if (e["isCancelled"] | false) continue;
      time_t end = parseIso(e["end"]["dateTime"] | "");
      if (end <= now) continue;  // bereits vorbei
      CalEvent& ev = c.ev[c.n++];
      setStr(ev.subject, e["subject"] | "(ohne Titel)");
      setStr(ev.location, e["location"]["displayName"] | "");
      setStr(ev.showAs, e["showAs"] | "");
      ev.start = parseIso(e["start"]["dateTime"] | "");
      ev.end = end;
      ev.allDay = e["isAllDay"] | false;
    }
    c.updated = now;
  } else {
    c.ok = c.updated != 0;
    httpErr(c.err, sizeof(c.err), rc);
  }
  touch();
  UNLOCK();
}

// ----------------------------------------------------------- Spotify
String netSpotifyLoginUrl() {
  Settings c = settingsCopy();
  LOCK();
  s_spVerifier = randomToken(48);
  s_spState = randomToken(16);
  String v = s_spVerifier, st = s_spState;
  UNLOCK();
  return "https://accounts.spotify.com/authorize?client_id=" + urlEncode(c.spClient) +
         "&response_type=code&redirect_uri=" + urlEncode(c.spRedirect) + "&code_challenge_method=S256&code_challenge=" +
         pkceChallenge(v) + "&state=" + st + "&scope=" + urlEncode(SP_SCOPES);
}

static bool spStoreTokens(JsonDocument& doc) {
  const char* at = doc["access_token"] | "";
  if (!*at) return false;
  settingsLock();
  S.spAccess = at;
  const char* rt = doc["refresh_token"] | "";
  if (*rt) S.spRefresh = rt;
  uint32_t ex = doc["expires_in"] | 3600;
  S.spExp = timeValid() ? nowUtc() + ex : 0;
  settingsUnlock();
  settingsSave();
  return true;
}

static void spExchange(const char* pasted) {
  String p = pasted;
  p.trim();
  String code = p.indexOf("code=") >= 0 ? queryParam(p, "code") : p;
  Settings c = settingsCopy();
  LOCK();
  String verifier = s_spVerifier;
  UNLOCK();
  JsonDocument res;
  String body = "grant_type=authorization_code&code=" + urlEncode(code) + "&redirect_uri=" + urlEncode(c.spRedirect) +
                "&client_id=" + urlEncode(c.spClient) + "&code_verifier=" + urlEncode(verifier);
  int rc = verifier.length() ? https("POST", "https://accounts.spotify.com/api/token", body,
                                     "application/x-www-form-urlencoded", {}, &res)
                             : -100;
  LOCK();
  if (rc == 200 && spStoreTokens(res)) {
    s_st.sp.linked = true;
    setStr(s_st.sp.err, "");
  } else if (rc == -100) {
    setStr(s_st.sp.err, "Login neu starten");
  } else {
    snprintf(s_st.sp.err, sizeof(s_st.sp.err), "Token %d %s", rc, (const char*)(res["error"] | ""));
  }
  touch();
  UNLOCK();
}

static bool spRefresh() {
  Settings c = settingsCopy();
  if (!c.spRefresh.length()) return false;
  JsonDocument res;
  String body = "grant_type=refresh_token&refresh_token=" + urlEncode(c.spRefresh) + "&client_id=" + urlEncode(c.spClient);
  int rc = https("POST", "https://accounts.spotify.com/api/token", body, "application/x-www-form-urlencoded", {}, &res);
  return rc == 200 && spStoreTokens(res);
}

static int spotify(const char* method, const String& path, JsonDocument* out = nullptr, JsonDocument* filter = nullptr) {
  Settings c = settingsCopy();
  if (!c.spAccess.length()) return -2;
  if (timeValid() && c.spExp && nowUtc() > (time_t)c.spExp - 60) {
    if (!spRefresh()) return 401;
    c = settingsCopy();
  }
  int rc = https(method, "https://api.spotify.com/v1/me/player" + path, "", nullptr,
                 {{"Authorization", "Bearer " + c.spAccess}}, out, filter);
  if (rc == 401 && spRefresh()) {
    c = settingsCopy();
    if (out) out->clear();
    rc = https(method, "https://api.spotify.com/v1/me/player" + path, "", nullptr,
               {{"Authorization", "Bearer " + c.spAccess}}, out, filter);
  }
  return rc;
}

static void spotifyPoll() {
  JsonDocument filter, doc;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["device"]["name"] = true;
  filter["device"]["volume_percent"] = true;
  filter["device"]["supports_volume"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["show"]["name"] = true;
  int rc = spotify("GET", "", &doc, &filter);
  LOCK();
  SpotifyState& s = s_st.sp;
  if (rc == 200) {
    s.ok = s.active = true;
    setStr(s.err, "");
    s.playing = doc["is_playing"] | false;
    s.progressMs = doc["progress_ms"] | 0;
    s.durationMs = doc["item"]["duration_ms"] | 0;
    setStr(s.title, doc["item"]["name"] | "");
    const char* artist = doc["item"]["artists"][0]["name"] | (const char*)(doc["item"]["show"]["name"] | "");
    setStr(s.artist, artist);
    setStr(s.device, doc["device"]["name"] | "");
    s.volume = doc["device"]["volume_percent"] | s.volume;
    s.supportsVolume = doc["device"]["supports_volume"] | true;
    s.fetchedAt = millis();
  } else if (rc == 204) {
    s.ok = true;
    s.active = s.playing = false;
    setStr(s.err, "");
  } else {
    s.ok = false;
    httpErr(s.err, sizeof(s.err), rc);
  }
  touch();
  UNLOCK();
}

static void spotifyCmd(Job j, int val) {
  int rc = 0;
  switch (j) {
    case Job::SpVolume: rc = spotify("PUT", "/volume?volume_percent=" + String(constrain(val, 0, 100))); break;
    case Job::SpPlay: rc = spotify("PUT", "/play"); break;
    case Job::SpPause: rc = spotify("PUT", "/pause"); break;
    case Job::SpNext: rc = spotify("POST", "/next"); break;
    case Job::SpPrev: rc = spotify("POST", "/previous"); break;
    default: return;
  }
  if (rc >= 300 || rc < 0) {
    LOCK();
    s_st.notifyId++;
    if (rc == 403) setStr(s_st.notifyText, "Spotify: Premium noetig / nicht erlaubt");
    else if (rc == 404) setStr(s_st.notifyText, "Spotify: kein aktives Geraet");
    else snprintf(s_st.notifyText, sizeof(s_st.notifyText), "Spotify: HTTP %d", rc);
    setStr(s_st.notifyLevel, "warn");
    touch();
    UNLOCK();
  }
}

// ----------------------------------------------------------- WLAN
static void startAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  LOCK();
  s_st.apMode = true;
  s_st.wifi = false;
  setStr(s_st.ip, WiFi.softAPIP().toString().c_str());
  touch();
  UNLOCK();
}

static bool connectSta(const Settings& c, uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(c.wifiSsid.c_str(), c.wifiPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) vTaskDelay(pdMS_TO_TICKS(200));
  return WiFi.status() == WL_CONNECTED;
}

static void onConnected(const Settings& c) {
  configTzTime(c.tz.c_str(), "pool.ntp.org", "time.cloudflare.com");
  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);
  LOCK();
  s_st.apMode = false;
  s_st.wifi = true;
  setStr(s_st.ip, WiFi.localIP().toString().c_str());
  touch();
  UNLOCK();
}

// ----------------------------------------------------------- Task
static void handleJob(const JobMsg& m) {
  switch (m.job) {
    case Job::ClaudeExchange: claudeExchange(m.sval); claudePoll(); break;
    case Job::ClaudeLogout:
      settingsLock();
      S.claudeAccess = S.claudeRefresh = "";
      S.claudeExp = 0;
      settingsUnlock();
      settingsSave();
      LOCK();
      s_st.claude = ClaudeUsage();
      touch();
      UNLOCK();
      break;
    case Job::MsStart: msStart(); break;
    case Job::MsLogout:
      settingsLock();
      S.msAccess = S.msRefresh = "";
      S.msExp = 0;
      settingsUnlock();
      settingsSave();
      LOCK();
      s_st.msLinked = false;
      s_st.teams = TeamsState();
      s_st.cal = CalendarState();
      touch();
      UNLOCK();
      break;
    case Job::SpExchange: spExchange(m.sval); spotifyPoll(); break;
    case Job::SpLogout:
      settingsLock();
      S.spAccess = S.spRefresh = "";
      S.spExp = 0;
      settingsUnlock();
      settingsSave();
      LOCK();
      s_st.sp = SpotifyState();
      touch();
      UNLOCK();
      break;
    case Job::SetPresence: setPresence(m.sval); break;
    case Job::SpVolume:
    case Job::SpPlay:
    case Job::SpPause:
    case Job::SpNext:
    case Job::SpPrev:
      spotifyCmd(m.job, m.ival);
      vTaskDelay(pdMS_TO_TICKS(m.job == Job::SpVolume ? 50 : 400));
      spotifyPoll();
      break;
    case Job::RefreshAll: break;  // Polls werden unten fällig gestellt
  }
}

static void netTask(void*) {
  Settings c = settingsCopy();
  bool up = c.wifiSsid.length() && connectSta(c, 25000);
  if (up) onConnected(c);
  else startAp();

  uint32_t tClaude = 0, tTeams = 0, tCal = 0, tSp = 0, tApRetry = millis();
  for (;;) {
    LOCK();
    bool ap = s_st.apMode;
    UNLOCK();

    if (ap) {
      // Im Setup-Modus regelmäßig das gespeicherte WLAN erneut probieren, wenn niemand verbunden ist
      if (millis() - tApRetry > 180000) c = settingsCopy();
      if (c.wifiSsid.length() && WiFi.softAPgetStationNum() == 0 && millis() - tApRetry > 180000) {
        if (connectSta(c, 20000)) onConnected(c);
        else startAp();
        tApRetry = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    bool wl = WiFi.status() == WL_CONNECTED;
    LOCK();
    if (wl != s_st.wifi) {
      s_st.wifi = wl;
      if (wl) setStr(s_st.ip, WiFi.localIP().toString().c_str());
      touch();
    }
    s_st.timeOk = timeValid();
    // Klingeln endet automatisch
    if (s_st.ringing && millis() - s_st.ringSince > RING_TIMEOUT_S * 1000UL) {
      s_st.ringing = false;
      touch();
    }
    UNLOCK();
    if (!wl) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    JobMsg m;
    if (xQueueReceive(s_jobs, &m, 0) == pdTRUE) {
      // Lautstärke: nur den letzten Wert senden
      JobMsg next;
      while (m.job == Job::SpVolume && xQueuePeek(s_jobs, &next, 0) == pdTRUE && next.job == Job::SpVolume)
        xQueueReceive(s_jobs, &m, 0);
      if (m.job == Job::RefreshAll) tClaude = tTeams = tCal = tSp = 0;
      handleJob(m);
      continue;
    }

    msDevicePoll();
    settingsLock();  // nur Flags lesen, keine Token-Kopien
    bool hasClaude = S.claudeAccess.length() > 0, hasMs = S.msAccess.length() > 0, hasSp = S.spAccess.length() > 0;
    settingsUnlock();
    Focus f = s_focus;
    uint32_t now = millis();
    auto due = [&](uint32_t& t, uint32_t every) {
      if (t == 0 || now - t >= every) {
        t = now ? now : 1;
        return true;
      }
      return false;
    };
    if (hasClaude && due(tClaude, f == Focus::Claude ? 180000 : 300000)) claudePoll();
    else if (hasMs && due(tTeams, f == Focus::Teams ? 8000 : 20000)) teamsPoll();
    else if (hasMs && timeValid() && due(tCal, f == Focus::Calendar ? 60000 : 180000)) calendarPoll();
    else if (hasSp && due(tSp, f == Focus::Spotify ? 3000 : 30000)) spotifyPoll();
    else xQueuePeek(s_jobs, &m, pdMS_TO_TICKS(100));  // warten, bis ein Auftrag kommt
  }
}

// ----------------------------------------------------------- öffentliche API
void netBegin() {
  s_mx = xSemaphoreCreateMutex();
  s_jobs = xQueueCreate(6, sizeof(JobMsg));
  Settings c = settingsCopy();
  s_st.claude.linked = c.claudeAccess.length() > 0;
  s_st.msLinked = c.msAccess.length() > 0;
  s_st.sp.linked = c.spAccess.length() > 0;
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

void netGetState(AppState& out) {
  LOCK();
  out = s_st;
  UNLOCK();
}

bool netJob(Job j, int ival, const char* sval) {
  static JobMsg m;  // groß -> nicht auf dem Stack
  m.job = j;
  m.ival = ival;
  strlcpy(m.sval, sval ? sval : "", sizeof(m.sval));
  return xQueueSend(s_jobs, &m, pdMS_TO_TICKS(50)) == pdTRUE;
}

void netSetFocus(Focus f) { s_focus = f; }

void netRing(const char* caller) {
  LOCK();
  s_st.ringing = true;
  s_st.callId++;
  s_st.ringSince = millis();
  setStr(s_st.caller, caller && *caller ? caller : "Unbekannt");
  touch();
  UNLOCK();
}

void netRingStop() {
  LOCK();
  s_st.ringing = false;
  touch();
  UNLOCK();
}

void netNotify(const char* text, const char* level) {
  LOCK();
  s_st.notifyId++;
  setStr(s_st.notifyText, text);
  setStr(s_st.notifyLevel, level && *level ? level : "info");
  touch();
  UNLOCK();
}
