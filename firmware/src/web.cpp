// Webkonfiguration (http://claude-dial.local) + Captive Portal im Setup-Modus
// + Webhook-Endpunkte für Rufsignalisierung und Hinweise.
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "settings.h"
#include "state.h"

static WebServer server(80);
static DNSServer dns;
static bool s_started = false, s_dns = false;
static String s_flash;  // Hinweis für die nächste Seitenanzeige

static String esc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static bool auth() {
  Settings c = settingsCopy();
  if (!c.webPass.length()) return true;
  if (server.authenticate("admin", c.webPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Claude Dial");
  return false;
}

static void back(const String& msg) {
  s_flash = msg;
  server.sendHeader("Location", "/");
  server.send(303);
}

static const char CSS[] PROGMEM = R"CSS(
body{font-family:system-ui,sans-serif;background:#141413;color:#eee;margin:0;padding:16px;max-width:720px;margin:auto}
h1{color:#d97757;font-size:1.5em}h2{font-size:1.1em;margin:0 0 8px}
section{background:#1f1f1d;border-radius:12px;padding:16px;margin:12px 0}
label{display:block;margin:8px 0 2px;color:#aaa;font-size:.9em}
input{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #444;background:#111;color:#eee}
button,.btn{background:#d97757;color:#111;border:0;border-radius:6px;padding:8px 14px;margin:8px 8px 0 0;font-weight:600;cursor:pointer;text-decoration:none;display:inline-block}
.sec{background:#444;color:#eee}.ok{color:#2ecc71}.bad{color:#eb3c32}.muted{color:#888;font-size:.85em}
.flash{background:#2a3b2a;padding:10px;border-radius:8px}code{background:#000;padding:2px 5px;border-radius:4px}
.code{font-size:1.6em;letter-spacing:3px;color:#d97757}
)CSS";

static String stat(bool ok, const char* yes, const char* no) {
  return String("<span class='") + (ok ? "ok'>" : "bad'>") + (ok ? yes : no) + "</span>";
}

static void handleRoot() {
  if (!auth()) return;
  Settings c = settingsCopy();
  AppState st;
  netGetState(st);
  String h;
  h.reserve(9000);
  h += F("<!doctype html><html lang=de><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Claude Dial</title><style>");
  h += FPSTR(CSS);
  h += F("</style></head><body><h1>Claude Dial</h1>");
  if (s_flash.length()) {
    h += "<p class=flash>" + esc(s_flash) + "</p>";
    s_flash = "";
  }

  // WLAN
  h += F("<section><h2>WLAN</h2>");
  h += st.apMode ? String("<p>Setup-Modus. Nach dem Speichern startet der Dial neu.</p>")
                 : "<p>Verbunden: " + stat(st.wifi, "ja", "nein") + " &middot; IP " + st.ip + "</p>";
  h += F("<form method=post action=/wifi><label>SSID</label><input name=ssid value='");
  h += esc(c.wifiSsid);
  h += F("'><label>Passwort (leer = unverändert)</label><input name=pass type=password><button>Speichern &amp; Neustart</button></form></section>");

  if (!st.apMode) {
    // Claude
    h += F("<section><h2>Claude-Nutzung</h2><p>Status: ");
    h += stat(st.claude.linked, "angemeldet", "nicht angemeldet");
    if (st.claude.err[0]) h += " &middot; <span class=bad>" + esc(st.claude.err) + "</span>";
    if (st.claude.ok)
      h += "<br>5h: " + String(st.claude.fiveH, 0) + "% &middot; Woche: " + String(st.claude.week, 0) + "%";
    h += F("</p><ol class=muted><li>„Bei Claude anmelden“ öffnet claude.ai in einem neuen Tab.</li>"
           "<li>Nach dem Login zeigt Claude einen Code an. Diesen kopieren und unten einfügen.</li></ol>"
           "<a class=btn target=_blank href=/claude/login>Bei Claude anmelden</a>"
           "<form method=post action=/claude/code><label>Code (Format code#state)</label><input name=code autocomplete=off>"
           "<button>Code übernehmen</button></form>"
           "<form method=post action=/claude/logout><button class=sec>Abmelden</button></form>"
           "<p class=muted>Nutzt die inoffizielle Nutzungs-Schnittstelle von Claude Code (5h-Fenster, Wochenlimit). "
           "Der Dial erhält eine eigene Sitzung; dein Claude Code am PC bleibt angemeldet.</p></section>");

    // Microsoft
    h += F("<section><h2>Microsoft 365 – Teams-Status &amp; Kalender</h2><p>Status: ");
    h += stat(st.msLinked, "verbunden", "nicht verbunden");
    if (st.teams.err[0]) h += " &middot; <span class=bad>" + esc(st.teams.err) + "</span>";
    h += "</p>";
    if (st.ms.pending)
      h += "<p>Öffne <a target=_blank href='" + esc(st.ms.url) + "'>" + esc(st.ms.url) + "</a> und gib ein:<br><span class=code>" +
           esc(st.ms.userCode) + "</span><br><span class=muted>Seite danach neu laden.</span></p>";
    else if (st.ms.msg[0])
      h += "<p class=muted>" + esc(st.ms.msg) + "</p>";
    h += F("<form method=post action=/ms><label>Anwendungs-(Client-)ID der App-Registrierung</label><input name=client value='");
    h += esc(c.msClient);
    h += F("'><label>Tenant (organizations, common oder Tenant-ID)</label><input name=tenant value='");
    h += esc(c.msTenant);
    h += F("'><button>Speichern</button></form><form method=post action=/ms/login><button>Anmelden (Gerätecode)</button></form>"
           "<form method=post action=/ms/logout><button class=sec>Abmelden</button></form>"
           "<p class=muted>Berechtigungen: Presence.ReadWrite, Calendars.Read, User.Read, offline_access. "
           "„Öffentliche Clientflows zulassen“ muss in der App-Registrierung aktiv sein.</p></section>");

    // Spotify
    h += F("<section><h2>Spotify</h2><p>Status: ");
    h += stat(st.sp.linked, "verbunden", "nicht verbunden");
    if (st.sp.err[0]) h += " &middot; <span class=bad>" + esc(st.sp.err) + "</span>";
    h += F("</p><form method=post action=/spotify><label>Client-ID (developer.spotify.com)</label><input name=client value='");
    h += esc(c.spClient);
    h += F("'><label>Redirect-URI (genau so in der Spotify-App eintragen)</label><input name=redirect value='");
    h += esc(c.spRedirect);
    h += F("'><button>Speichern</button></form><a class=btn target=_blank href=/spotify/login>Bei Spotify anmelden</a>"
           "<form method=post action=/spotify/code><label>Nach dem Login: komplette Adresse aus der Adresszeile einfügen "
           "(die Seite selbst lädt nicht – das ist normal)</label><input name=url autocomplete=off><button>Übernehmen</button></form>"
           "<form method=post action=/spotify/logout><button class=sec>Abmelden</button></form>"
           "<p class=muted>Steuerung (Lautstärke, Play/Pause) erfordert Spotify Premium.</p></section>");

    // Allgemein
    h += F("<section><h2>Allgemein</h2><form method=post action=/settings><label>Zeitzone (POSIX)</label><input name=tz value='");
    h += esc(c.tz);
    h += F("'><label>Passwort der Webkonfiguration (Benutzer admin, leer = unverändert, „-“ = entfernen)</label><input name=webpw type=password>"
           "<label>Webhook-Token für /api/ring und /api/notify (leer = unverändert, „-“ = entfernen)</label><input name=hook>"
           "<label>Claude-OAuth-Scope (nur ändern, wenn nötig)</label><input name=scope value='");
    h += esc(c.claudeScope);
    h += F("'><button>Speichern</button></form>");
    h += "<p class=muted>Webhook: <code>curl -X POST http://" + String(st.ip) +
         "/api/ring -H 'X-Dial-Token: …' -d '{\"caller\":\"Erika\"}'</code><br>Token gesetzt: " +
         (c.hookToken.length() ? "ja" : "nein (jeder im Netz kann klingeln lassen)") + "</p>";
    h += F("<form method=post action=/reboot><button class=sec>Neustart</button></form></section>");
  }
  h += F("</body></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

static void handleWifi() {
  if (!auth()) return;
  settingsLock();
  S.wifiSsid = server.arg("ssid");
  if (server.arg("pass").length()) S.wifiPass = server.arg("pass");
  settingsUnlock();
  settingsSave();
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><p style='font-family:sans-serif'>Gespeichert. Der Dial startet neu und verbindet sich "
              "mit dem WLAN. Danach erreichbar unter <b>http://" HOSTNAME ".local</b> (IP steht unter Einstellungen &gt; Info).</p>");
  delay(800);
  ESP.restart();
}

static void setOrClear(String& field, const String& v) {
  if (v == "-") field = "";
  else if (v.length()) field = v;
}

static void handleSettings() {
  if (!auth()) return;
  settingsLock();
  if (server.arg("tz").length()) S.tz = server.arg("tz");
  setOrClear(S.webPass, server.arg("webpw"));
  setOrClear(S.hookToken, server.arg("hook"));
  if (server.arg("scope").length()) S.claudeScope = server.arg("scope");
  settingsUnlock();
  settingsSave();
  back("Gespeichert. Zeitzonen-Änderung wirkt nach Neustart.");
}

static void redirectTo(const String& url) {
  server.sendHeader("Location", url);
  server.send(302);
}

// Webhook-Token prüfen (Header X-Dial-Token oder ?token=)
static bool hookAuth() {
  Settings c = settingsCopy();
  if (!c.hookToken.length()) return true;
  if (server.header("X-Dial-Token") == c.hookToken || server.arg("token") == c.hookToken) return true;
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

static void handleRing() {
  if (!hookAuth()) return;
  JsonDocument d;
  deserializeJson(d, server.arg("plain"));
  String caller = d["caller"] | server.arg("caller");
  netRing(caller.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleNotify() {
  if (!hookAuth()) return;
  JsonDocument d;
  deserializeJson(d, server.arg("plain"));
  String text = d["text"] | server.arg("text");
  String level = d["level"] | server.arg("level");
  netNotify(text.c_str(), level.c_str());
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleStatus() {
  if (!auth()) return;
  AppState st;
  netGetState(st);
  JsonDocument d;
  d["wifi"] = st.wifi;
  d["ip"] = st.ip;
  d["time_ok"] = st.timeOk;
  d["heap"] = ESP.getFreeHeap();
  JsonObject cl = d["claude"].to<JsonObject>();
  cl["linked"] = st.claude.linked;
  cl["five_hour"] = st.claude.fiveH;
  cl["seven_day"] = st.claude.week;
  cl["error"] = st.claude.err;
  d["teams"]["availability"] = st.teams.availability;
  d["calendar"]["events"] = st.cal.n;
  d["spotify"]["title"] = st.sp.title;
  d["spotify"]["volume"] = st.sp.volume;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

void webBegin() {
  static const char* hdrs[] = {"X-Dial-Token"};
  server.collectHeaders(hdrs, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/reboot", HTTP_POST, [] {
    if (!auth()) return;
    server.send(200, "text/plain", "Neustart...");
    delay(500);
    ESP.restart();
  });

  server.on("/claude/login", HTTP_GET, [] {
    if (auth()) redirectTo(netClaudeLoginUrl());
  });
  server.on("/claude/code", HTTP_POST, [] {
    if (!auth()) return;
    netJob(Job::ClaudeExchange, 0, server.arg("code").c_str());
    back("Code übernommen – Anmeldung läuft. Seite in ein paar Sekunden neu laden.");
  });
  server.on("/claude/logout", HTTP_POST, [] {
    if (!auth()) return;
    netJob(Job::ClaudeLogout);
    back("Claude abgemeldet.");
  });

  server.on("/ms", HTTP_POST, [] {
    if (!auth()) return;
    settingsLock();
    S.msClient = server.arg("client");
    S.msTenant = server.arg("tenant").length() ? server.arg("tenant") : String("organizations");
    settingsUnlock();
    settingsSave();
    back("Microsoft-Einstellungen gespeichert.");
  });
  server.on("/ms/login", HTTP_POST, [] {
    if (!auth()) return;
    netJob(Job::MsStart);
    back("Anmeldung gestartet – Code erscheint gleich hier und auf dem Dial. Seite neu laden.");
  });
  server.on("/ms/logout", HTTP_POST, [] {
    if (!auth()) return;
    netJob(Job::MsLogout);
    back("Microsoft abgemeldet.");
  });

  server.on("/spotify", HTTP_POST, [] {
    if (!auth()) return;
    settingsLock();
    S.spClient = server.arg("client");
    if (server.arg("redirect").length()) S.spRedirect = server.arg("redirect");
    settingsUnlock();
    settingsSave();
    back("Spotify-Einstellungen gespeichert.");
  });
  server.on("/spotify/login", HTTP_GET, [] {
    if (!auth()) return;
    if (!settingsCopy().spClient.length()) return back("Zuerst die Spotify-Client-ID speichern.");
    redirectTo(netSpotifyLoginUrl());
  });
  server.on("/spotify/code", HTTP_POST, [] {
    if (!auth()) return;
    netJob(Job::SpExchange, 0, server.arg("url").c_str());
    back("Übernommen – Anmeldung läuft. Seite in ein paar Sekunden neu laden.");
  });
  server.on("/spotify/logout", HTTP_POST, [] {
    if (!auth()) return;
    netJob(Job::SpLogout);
    back("Spotify abgemeldet.");
  });

  server.on("/api/ring", HTTP_POST, handleRing);
  server.on("/api/ring/stop", HTTP_POST, [] {
    if (!hookAuth()) return;
    netRingStop();
    server.send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/notify", HTTP_POST, handleNotify);
  server.on("/api/status", HTTP_GET, handleStatus);

  server.onNotFound([] {
    AppState st;
    netGetState(st);
    if (st.apMode) {  // Captive Portal: alles auf die Setup-Seite
      server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
      server.send(302);
    } else {
      server.send(404, "text/plain", "nicht gefunden");
    }
  });
  server.begin();
  s_started = true;
}

void webLoop(bool apMode, bool netUp) {
  if (!s_started && (apMode || netUp)) webBegin();
  if (apMode && !s_dns) {
    dns.start(53, "*", WiFi.softAPIP());
    s_dns = true;
  } else if (!apMode && s_dns) {
    dns.stop();
    s_dns = false;
  }
  if (s_dns) dns.processNextRequest();
  if (s_started) server.handleClient();
}
