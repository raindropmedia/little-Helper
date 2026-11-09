#pragma once
#include <Arduino.h>
#include <time.h>

// Laufzeitzustand, den der Netzwerk-Task füllt und die Oberfläche anzeigt.

struct ClaudeUsage {
  bool linked = false, ok = false;
  char err[40] = "";
  float fiveH = -1, week = -1, opus = -1, sonnet = -1;  // Prozent, -1 = unbekannt
  time_t fiveReset = 0, weekReset = 0, opusReset = 0, sonnetReset = 0;
  bool extraEnabled = false;
  float extraUsed = 0, extraLimit = 0, extraUtil = -1;
  time_t updated = 0;
};

struct CalEvent {
  char subject[48];
  char location[32];
  time_t start, end;
  bool allDay;
  char showAs[12];
};

struct CalendarState {
  bool ok = false;
  char err[40] = "";
  CalEvent ev[8];
  uint8_t n = 0;
  time_t updated = 0;
};

struct SpotifyState {
  bool linked = false, ok = false, active = false, playing = false, supportsVolume = true;
  char err[40] = "";
  char title[48] = "", artist[40] = "", device[24] = "";
  int volume = 0;
  uint32_t progressMs = 0, durationMs = 0, fetchedAt = 0;  // fetchedAt = millis()
};

struct TeamsState {
  bool ok = false;
  char err[40] = "";
  char availability[20] = "PresenceUnknown";
  char activity[24] = "";
};

struct MsLogin {  // laufende Microsoft-Geräteanmeldung
  bool pending = false;
  char userCode[16] = "";
  char url[48] = "";
  char msg[60] = "";
};

struct AppState {
  bool wifi = false, apMode = false, timeOk = false;
  char ip[16] = "";
  bool msLinked = false;
  ClaudeUsage claude;
  CalendarState cal;
  SpotifyState sp;
  TeamsState teams;
  MsLogin ms;
  // Rufsignalisierung / Hinweise (Webhook)
  bool ringing = false;
  char caller[41] = "";
  uint32_t callId = 0, ringSince = 0;
  uint32_t notifyId = 0;
  char notifyText[61] = "";
  char notifyLevel[8] = "";
  uint32_t seq = 0;
};

// Aufträge an den Netzwerk-Task
enum class Job : uint8_t {
  ClaudeExchange, ClaudeLogout,
  MsStart, MsLogout,
  SpExchange, SpLogout,
  SetPresence,
  SpVolume, SpPlay, SpPause, SpNext, SpPrev,
  RefreshAll,
};

struct JobMsg {
  Job job;
  int ival;
  char sval[480];
};

// Fokus der Oberfläche -> schnellere Abfrage der sichtbaren Daten
enum class Focus : uint8_t { None, Claude, Calendar, Spotify, Teams };

// net.cpp
void netBegin();
void netGetState(AppState& out);
bool netJob(Job j, int ival = 0, const char* sval = "");
void netSetFocus(Focus f);
void netRing(const char* caller);
void netRingStop();
void netNotify(const char* text, const char* level);
String netClaudeLoginUrl();   // erzeugt PKCE + URL
String netSpotifyLoginUrl();
