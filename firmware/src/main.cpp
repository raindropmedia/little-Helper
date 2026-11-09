// Claude Dial – Claude-Nutzung, Kalender, Spotify und Teams-Status am M5Stack Dial v1.1
// Läuft komplett eigenständig; Einrichtung über http://claude-dial.local
//
// Bedienung
//   Drehen        : Menü / Auswahl / Spotify-Lautstärke
//   Drücken (Rad) : Öffnen / Ausführen
//   Lang drücken  : Zurück zum Menü
//   Tippen        : wie Drücken (Spotify: links = zurück, rechts = weiter)
#include <M5Dial.h>
#include <WiFi.h>

#include "config.h"
#include "settings.h"
#include "state.h"
#include "ui.h"

void webLoop(bool apMode, bool netUp);

// ============================================================ Zustand
enum Screen : uint8_t { MENU, CLAUDE, CALENDAR, SPOTIFY, PRESENCE, SETTINGS };

struct MenuItem {
  Screen scr;
  const char* title;
  Icon icon;
};
static const MenuItem MENU_ITEMS[] = {
    {CLAUDE, "Claude", Icon::Claude},       {CALENDAR, "Kalender", Icon::Calendar},
    {SPOTIFY, "Spotify", Icon::Music},      {PRESENCE, "Teams Status", Icon::Presence},
    {SETTINGS, "Einstellungen", Icon::Settings},
};
constexpr int MENU_N = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

static const char* PRESENCE_KEYS[] = {"Available", "Busy", "DoNotDisturb", "BeRightBack", "Away", "Offline", "Reset"};
constexpr int PRESENCE_N = 7;

enum SetItem : uint8_t { S_BRIGHT, S_SOUND, S_VOL, S_HOME, S_REMIND, S_TESTRING, S_REFRESH, S_INFO };
constexpr int SET_N = 8;

static AppState st;
static Screen scr = MENU;
static int menuSel = 0;
static int sel = 0;           // Auswahl/Scroll innerhalb eines Screens
static bool editing = false;  // Einstellungen: Wert bearbeiten
static uint32_t lastInput = 0;
static bool dimmed = false;
static bool dirty = true;
static uint32_t lastSeq = 0;

static char toastText[64] = "";
static uint16_t toastColor = col::claude;
static uint32_t toastUntil = 0;

static uint32_t lastNotifyId = 0;
static bool notifyInit = false;
static uint32_t dismissedCall = 0, silencedCall = 0;
static uint32_t nextRingMs = 0;
static uint8_t ringStep = 0;

// Spotify-Lautstärke: lokal sofort anzeigen, verzögert senden
static int spVol = -1;
static uint32_t spVolInputAt = 0;
static bool spVolPending = false;

// Kalender-Erinnerung
static uint32_t remindedKeys[12];
static uint8_t remindedPos = 0;
static int reminderIdx = -1;  // Index in st.cal.ev oder -1
static uint32_t reminderKey = 0;

// ============================================================ Helfer
static void beep(uint16_t f, uint16_t ms) {
  if (S.sound) M5Dial.Speaker.tone(f, ms);
}

static void toast(const char* text, uint16_t color = col::claude, uint32_t ms = 2500) {
  strlcpy(toastText, text, sizeof(toastText));
  toastColor = color;
  toastUntil = millis() + ms;
  dirty = true;
}

static void applyOutput() {
  M5Dial.Display.setBrightness(dimmed ? S.bright / 5 + 2 : S.bright);
  M5Dial.Speaker.setVolume(S.vol);
}

static void wake() {
  lastInput = millis();
  if (dimmed) {
    dimmed = false;
    applyOutput();
  }
}

static int wrap(int v, int n) { return ((v % n) + n) % n; }

static void fit(char* buf, size_t n, const char* src, const lgfx::IFont* font, int maxW) {
  strlcpy(buf, src, n);
  canvas.setFont(font);
  size_t len = strlen(buf);
  if (canvas.textWidth(buf) <= maxW) return;
  while (len > 2) {
    buf[--len] = 0;
    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s..", buf);
    if (canvas.textWidth(tmp) <= maxW) {
      strlcpy(buf, tmp, n);
      return;
    }
  }
}

static time_t now() { return time(nullptr); }
static bool ringingActive() { return st.ringing && st.callId != dismissedCall; }

static Focus focusFor(Screen s) {
  switch (s) {
    case CLAUDE: return Focus::Claude;
    case CALENDAR: return Focus::Calendar;
    case SPOTIFY: return Focus::Spotify;
    case PRESENCE: return Focus::Teams;
    default: return Focus::None;
  }
}

static void setScreen(Screen s) {
  scr = s;
  editing = false;
  sel = 0;
  if (s == PRESENCE)
    for (int i = 0; i < PRESENCE_N; i++)
      if (!strcmp(PRESENCE_KEYS[i], st.teams.availability)) sel = i;
  if (s == MENU) {
    for (int i = 0; i < MENU_N; i++)
      if (MENU_ITEMS[i].scr == scr) menuSel = i;
  }
  netSetFocus(focusFor(s));
  dirty = true;
}

static void backToMenu() {
  for (int i = 0; i < MENU_N; i++)
    if (MENU_ITEMS[i].scr == scr) menuSel = i;
  setScreen(MENU);
}

static void drawHeader(const char* label, int y = 52) { txt(label, CX, y, &fonts::FreeSans9pt7b, col::dim); }

static void drawHint(const char* title, const char* l1, const char* l2 = nullptr) {
  drawHeader(title);
  txt(l1, CX, 104, &fonts::FreeSansBold9pt7b, col::text);
  if (l2) txt(l2, CX, 128, &fonts::FreeSans9pt7b, col::dim);
  char b[48];
  snprintf(b, sizeof(b), "%s.local", HOSTNAME);
  txt(b, CX, 160, &fonts::Font2, col::claude);
  txt(st.ip, CX, 180, &fonts::Font2, col::dim);
}

// Uhrzeit bis Reset als "2:48h" bzw. "Mo 09:00"
static void fmtReset(char* out, size_t n, time_t t) {
  if (!t) {
    strlcpy(out, "-", n);
    return;
  }
  long d = (long)(t - now());
  if (d < 0) d = 0;
  if (d < 6 * 3600) fmtMinutes(out, n, d / 60);
  else fmtClock(out, n, t, d > 20 * 3600);
}

// ============================================================ Screens
static void menuSubtitle(int idx, char* out, size_t n, uint16_t& color) {
  color = col::dim;
  switch (MENU_ITEMS[idx].scr) {
    case CLAUDE:
      if (!st.claude.linked) strlcpy(out, "nicht angemeldet", n);
      else if (!st.claude.ok) strlcpy(out, st.claude.err[0] ? st.claude.err : "lade...", n);
      else {
        snprintf(out, n, "5h %.0f%%  |  Woche %.0f%%", st.claude.fiveH, st.claude.week);
        color = pctColor(max(st.claude.fiveH, st.claude.week) / 100.f);
      }
      return;
    case CALENDAR: {
      if (!st.msLinked) { strlcpy(out, "nicht verbunden", n); return; }
      for (int i = 0; i < st.cal.n; i++) {
        const CalEvent& e = st.cal.ev[i];
        if (e.allDay) continue;
        char t[8];
        fmtClock(t, sizeof(t), e.start);
        snprintf(out, n, "%s %s", t, e.subject);
        color = e.start <= now() ? col::green : col::claude;
        return;
      }
      strlcpy(out, st.cal.ok ? "keine Termine" : "lade...", n);
      return;
    }
    case SPOTIFY:
      if (!st.sp.linked) strlcpy(out, "nicht verbunden", n);
      else if (!st.sp.active) strlcpy(out, "keine Wiedergabe", n);
      else {
        strlcpy(out, st.sp.title, n);
        color = st.sp.playing ? col::green : col::dim;
      }
      return;
    case PRESENCE:
      if (!st.msLinked) { strlcpy(out, "nicht verbunden", n); return; }
      strlcpy(out, presenceLabel(st.teams.availability), n);
      color = presenceColor(st.teams.availability);
      return;
    case SETTINGS: snprintf(out, n, "%s.local", HOSTNAME); return;
    default: out[0] = 0;
  }
}

static void drawMenu() {
  ringSelector(MENU_N, menuSel, nullptr, 113, 8);
  const MenuItem& it = MENU_ITEMS[menuSel];
  uint16_t ic = col::claude;
  if (it.scr == PRESENCE) ic = presenceColor(st.teams.availability);
  if (it.scr == SPOTIFY) ic = col::green;
  drawIcon(it.icon, CX, 82, 24, ic);
  txt(it.title, CX, 134, &fonts::FreeSansBold12pt7b, col::text);
  char sub[64], f[48];
  uint16_t sc;
  menuSubtitle(menuSel, sub, sizeof(sub), sc);
  fit(f, sizeof(f), sub, &fonts::FreeSans9pt7b, 170);
  txt(f, CX, 162, &fonts::FreeSans9pt7b, sc);

  // Uhrzeit + Verbindungsstatus
  char b[24];
  if (st.timeOk) {
    fmtClock(b, sizeof(b), now());
  } else {
    strlcpy(b, st.wifi ? "online" : "WLAN...", sizeof(b));
  }
  uint16_t c = st.wifi ? col::green : col::orange;
  canvas.setFont(&fonts::FreeSans9pt7b);
  canvas.fillCircle(CX - canvas.textWidth(b) / 2 - 8, 192, 3, c);
  txt(b, CX + 4, 192, &fonts::FreeSans9pt7b, col::dim);
}

static void drawClaudeOverview() {
  const ClaudeUsage& u = st.claude;
  float p5 = u.fiveH / 100.f, pw = u.week / 100.f;
  gauge(102, 114, p5, pctColor(p5));
  gauge(86, 94, pw, pctColor(pw));
  drawHeader("5H-FENSTER", 62);
  char a[24], b[40];
  snprintf(a, sizeof(a), "%.0f%%", u.fiveH);
  txt(a, CX, 100, &fonts::FreeSansBold24pt7b, col::text);
  fmtReset(a, sizeof(a), u.fiveReset);
  snprintf(b, sizeof(b), "Reset %s", a);
  txt(b, CX, 134, &fonts::FreeSans9pt7b, col::dim);
  fmtReset(a, sizeof(a), u.weekReset);
  snprintf(b, sizeof(b), "Woche %.0f%%", u.week);
  txt(b, CX, 158, &fonts::FreeSansBold9pt7b, pctColor(pw));
  if (u.err[0]) txt(u.err, CX, 180, &fonts::Font0, col::orange);
}

static void drawClaudeDetails() {
  const ClaudeUsage& u = st.claude;
  drawHeader("CLAUDE LIMITS", 40);
  struct Row {
    const char* name;
    float v;
    time_t r;
  } rows[] = {{"5 Std.", u.fiveH, u.fiveReset}, {"Woche", u.week, u.weekReset},
              {"Opus", u.opus, u.opusReset},    {"Sonnet", u.sonnet, u.sonnetReset},
              {"Extra", u.extraEnabled ? u.extraUtil : -1, 0}};
  int y = 66;
  for (const Row& r : rows) {
    if (r.v < 0) continue;
    char a[16], b[24];
    txt(r.name, 36, y, &fonts::FreeSans9pt7b, col::text, middle_left);
    snprintf(a, sizeof(a), "%.0f%%", r.v);
    txt(a, 204, y, &fonts::FreeSansBold9pt7b, pctColor(r.v / 100.f), middle_right);
    if (r.r) {
      fmtReset(b, sizeof(b), r.r);
      txt(b, 150, y, &fonts::Font0, col::dim, middle_right);
    }
    canvas.fillRoundRect(36, y + 10, 168, 3, 1, col::track);
    canvas.fillRoundRect(36, y + 10, max(3, (int)(168 * min(r.v, 100.f) / 100.f)), 3, 1, pctColor(r.v / 100.f));
    y += 28;
    if (y > 186) break;
  }
  if (u.updated) {
    char t[24], b[32];
    fmtClock(t, sizeof(t), u.updated);
    snprintf(b, sizeof(b), "Stand %s", t);
    txt(b, CX, 206, &fonts::Font0, col::dim);
  }
}

static void drawClaude() {
  if (!st.claude.linked) return drawHint("CLAUDE", "Nicht angemeldet", "Im Browser einrichten:");
  if (!st.claude.ok) return drawHint("CLAUDE", st.claude.err[0] ? st.claude.err : "Lade Nutzung...");
  if (sel == 0) drawClaudeOverview();
  else drawClaudeDetails();
  pageDots(2, sel, 216);
}

static void drawCalendar() {
  if (!st.msLinked) return drawHint("KALENDER", "Microsoft nicht verbunden", "Im Browser einrichten:");
  if (!st.cal.ok) return drawHint("KALENDER", st.cal.err[0] ? st.cal.err : "Lade Termine...");
  const CalendarState& c = st.cal;
  time_t t = now();
  struct tm tm;
  localtime_r(&t, &tm);
  char head[32];
  static const char* WD[] = {"SO", "MO", "DI", "MI", "DO", "FR", "SA"};
  snprintf(head, sizeof(head), "%s %02d.%02d.", WD[tm.tm_wday], tm.tm_mday, tm.tm_mon + 1);
  drawHeader(head, 38);
  if (!c.n) {
    txt("Keine Termine", CX, 110, &fonts::FreeSansBold12pt7b, col::text);
    txt("in den naechsten 36 h", CX, 136, &fonts::FreeSans9pt7b, col::dim);
    return;
  }
  int first = constrain(sel, 0, max(0, c.n - 1));
  int y = 66;
  for (int i = first; i < c.n && y < 190; i++) {
    const CalEvent& e = c.ev[i];
    bool running = !e.allDay && e.start <= t && t < e.end;
    char tb[16], sb[48], lb[40];
    if (e.allDay) strlcpy(tb, "ganzt.", sizeof(tb));
    else fmtClock(tb, sizeof(tb), e.start, localtime_r(&e.start, &tm)->tm_mday != localtime_r(&t, &tm)->tm_mday);
    uint16_t bar = running ? col::green : !strcmp(e.showAs, "free") ? col::dim : col::claude;
    canvas.fillRoundRect(28, y - 9, 4, 34, 2, bar);
    txt(tb, 38, y, &fonts::FreeSansBold9pt7b, running ? col::green : col::text, middle_left);
    fit(sb, sizeof(sb), e.subject, &fonts::FreeSans9pt7b, 196 - 38 - canvas.textWidth(tb) - 8);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    int tw = canvas.textWidth(tb);
    fit(sb, sizeof(sb), e.subject, &fonts::FreeSans9pt7b, 204 - 46 - tw);
    txt(sb, 46 + tw, y, &fonts::FreeSans9pt7b, col::text, middle_left);
    char sub[48];
    if (running) {
      snprintf(sub, sizeof(sub), "laeuft, noch %ld Min", (long)((e.end - t) / 60));
    } else if (!e.allDay && e.start > t && e.start - t < 3 * 3600) {
      snprintf(sub, sizeof(sub), "in %ld Min", (long)((e.start - t + 59) / 60));
    } else {
      sub[0] = 0;
    }
    if (e.location[0]) {
      if (sub[0]) strlcat(sub, "  ", sizeof(sub));
      strlcat(sub, e.location, sizeof(sub));
    }
    fit(lb, sizeof(lb), sub, &fonts::Font2, 166);
    txt(lb, 38, y + 17, &fonts::Font2, running ? col::green : col::dim, middle_left);
    y += 44;
  }
  if (c.n > 1) pageDots(min<int>(c.n, 8), first, 214);
}

static int spDisplayVol() { return (spVol >= 0 && millis() - spVolInputAt < 4000) ? spVol : st.sp.volume; }

static void drawSpotify() {
  if (!st.sp.linked) return drawHint("SPOTIFY", "Nicht verbunden", "Im Browser einrichten:");
  const SpotifyState& s = st.sp;
  int vol = spDisplayVol();
  gauge(104, 114, vol / 100.f, s.supportsVolume ? col::green : col::dim);
  if (!s.active) {
    drawHeader("SPOTIFY", 56);
    drawIcon(Icon::Music, CX, 100, 22, col::dim);
    txt(s.ok ? "Keine Wiedergabe" : s.err, CX, 146, &fonts::FreeSansBold9pt7b, col::text);
    txt("Spotify auf einem Geraet starten", CX, 170, &fonts::Font0, col::dim);
    return;
  }
  char b[64];
  fit(b, sizeof(b), s.device, &fonts::Font2, 140);
  txt(b, CX, 50, &fonts::Font2, col::green);
  fit(b, sizeof(b), s.title, &fonts::FreeSansBold9pt7b, 168);
  txt(b, CX, 80, &fonts::FreeSansBold9pt7b, col::text);
  fit(b, sizeof(b), s.artist, &fonts::FreeSans9pt7b, 160);
  txt(b, CX, 102, &fonts::FreeSans9pt7b, col::dim);

  drawIcon(Icon::Prev, CX - 52, 140, 9, col::dim);
  canvas.fillCircle(CX, 140, 22, col::green);
  drawIcon(s.playing ? Icon::Pause : Icon::Play, CX + (s.playing ? 0 : 2), 140, 9, col::bg);
  drawIcon(Icon::Next, CX + 52, 140, 9, col::dim);

  // Fortschritt (zwischen den Abfragen hochgerechnet)
  uint32_t prog = s.progressMs + (s.playing ? millis() - s.fetchedAt : 0);
  if (s.durationMs) {
    float p = min(1.f, prog / (float)s.durationMs);
    canvas.fillRoundRect(CX - 60, 176, 120, 3, 1, col::track);
    canvas.fillRoundRect(CX - 60, 176, max(3, (int)(120 * p)), 3, 1, col::text);
  }
  snprintf(b, sizeof(b), s.supportsVolume ? "Lautstaerke %d%%" : "Lautstaerke nicht steuerbar", vol);
  txt(b, CX, 196, &fonts::Font0, s.supportsVolume ? col::green : col::orange);
}

static uint16_t presenceOptColor(int i) { return i == PRESENCE_N - 1 ? col::blue : presenceColor(PRESENCE_KEYS[i]); }

static void drawPresence() {
  if (!st.msLinked) return drawHint("TEAMS STATUS", "Microsoft nicht verbunden", "Im Browser einrichten:");
  uint16_t colors[PRESENCE_N];
  for (int i = 0; i < PRESENCE_N; i++) colors[i] = presenceOptColor(i);
  ringSelector(PRESENCE_N, sel, colors, 113, 8);
  drawHeader("TEAMS STATUS", 50);
  canvas.fillCircle(CX, 96, 22, colors[sel]);
  if (!strcmp(PRESENCE_KEYS[sel], "DoNotDisturb")) canvas.fillRect(CX - 12, 93, 24, 6, col::text);
  if (!strcmp(PRESENCE_KEYS[sel], "Reset")) canvas.fillCircle(CX, 96, 12, col::bg);
  txt(sel == PRESENCE_N - 1 ? "Zuruecksetzen" : presenceLabel(PRESENCE_KEYS[sel]), CX, 140, &fonts::FreeSansBold12pt7b,
      col::text);
  char line[48];
  snprintf(line, sizeof(line), "Aktuell: %s", presenceLabel(st.teams.availability));
  txt(st.teams.err[0] ? st.teams.err : line, CX, 168, &fonts::FreeSans9pt7b,
      st.teams.err[0] ? col::orange : presenceColor(st.teams.availability));
  txt("Druecken = setzen", CX, 190, &fonts::Font0, col::dim);
}

static void settingLabel(int i, const char*& label, char* val, size_t n) {
  switch (i) {
    case S_BRIGHT: label = "Helligkeit"; snprintf(val, n, "%d%%", S.bright * 100 / 255); break;
    case S_SOUND: label = "Ton"; strlcpy(val, S.sound ? "an" : "aus", n); break;
    case S_VOL: label = "Lautstaerke"; snprintf(val, n, "%d%%", S.vol * 100 / 255); break;
    case S_HOME:
      label = "Auto-Startseite";
      if (HOME_OPTS[S.homeIdx]) snprintf(val, n, "nach %ds", HOME_OPTS[S.homeIdx]);
      else strlcpy(val, "aus", n);
      break;
    case S_REMIND:
      label = "Termin-Erinnerung";
      if (REMIND_OPTS[S.remindIdx]) snprintf(val, n, "%d Min vorher", REMIND_OPTS[S.remindIdx]);
      else strlcpy(val, "aus", n);
      break;
    case S_TESTRING: label = "Testanruf"; strlcpy(val, "Druecken", n); break;
    case S_REFRESH: label = "Aktualisieren"; strlcpy(val, "Druecken", n); break;
    default: label = "Info"; val[0] = 0;
  }
}

static void drawSettings() {
  ringSelector(SET_N, sel, nullptr, 113, 8);
  drawHeader("EINSTELLUNGEN", 50);
  const char* label;
  char val[32];
  settingLabel(sel, label, val, sizeof(val));
  if (sel == S_INFO) {
    char line[48];
    txt("Info", CX, 78, &fonts::FreeSansBold12pt7b, col::text);
    snprintf(line, sizeof(line), "http://%s.local", HOSTNAME);
    txt(line, CX, 106, &fonts::Font2, col::claude);
    snprintf(line, sizeof(line), "IP %s", st.ip);
    txt(line, CX, 124, &fonts::Font2, col::dim);
    snprintf(line, sizeof(line), "WLAN %d dBm  Zeit %s", st.wifi ? WiFi.RSSI() : 0, st.timeOk ? "ok" : "-");
    txt(line, CX, 142, &fonts::Font2, col::dim);
    snprintf(line, sizeof(line), "Heap %u kB", (unsigned)(ESP.getFreeHeap() / 1024));
    txt(line, CX, 160, &fonts::Font2, col::dim);
    return;
  }
  txt(label, CX, 100, &fonts::FreeSansBold12pt7b, col::text);
  if (editing) canvas.fillRoundRect(CX - 70, 122, 140, 32, 16, col::claude);
  txt(val, CX, 138, &fonts::FreeSansBold9pt7b, editing ? col::bg : col::claude);
  if (sel <= S_REMIND) txt(editing ? "Drehen = aendern" : "Druecken = bearbeiten", CX, 176, &fonts::Font0, col::dim);
}

// ------------------------------------------------------------ Einblendungen
static void drawSetup() {
  drawIcon(Icon::Wifi, CX, 58, 20, col::claude);
  txt("WLAN einrichten", CX, 96, &fonts::FreeSansBold12pt7b, col::text);
  txt("Mit diesem WLAN verbinden:", CX, 124, &fonts::Font0, col::dim);
  txt(AP_SSID, CX, 142, &fonts::FreeSansBold9pt7b, col::claude);
  char b[40];
  snprintf(b, sizeof(b), "Passwort: %s", AP_PASS);
  txt(b, CX, 164, &fonts::Font2, col::text);
  snprintf(b, sizeof(b), "dann http://%s", st.ip);
  txt(b, CX, 186, &fonts::Font2, col::dim);
}

static void drawMsLogin() {
  drawHeader("MICROSOFT-LOGIN", 50);
  txt("Im Browser oeffnen:", CX, 80, &fonts::Font2, col::dim);
  txt("microsoft.com/devicelogin", CX, 100, &fonts::FreeSansBold9pt7b, col::blue);
  txt("Code eingeben:", CX, 128, &fonts::Font2, col::dim);
  txt(st.ms.userCode, CX, 158, &fonts::FreeSansBold18pt7b, col::text);
  txt("Drehen = ausblenden", CX, 196, &fonts::Font0, col::dim);
}

static void drawRinging() {
  canvas.fillScreen(c565(0, 40, 10));
  uint32_t t = millis() % 1200;
  int r = 60 + t * 60 / 1200;
  uint16_t pc = t < 600 ? col::green : c565(20, 120, 50);
  canvas.drawCircle(CX, CY, r, pc);
  canvas.drawCircle(CX, CY, r + 1, pc);
  presenceRim(col::green);
  drawIcon(Icon::Bell, CX, 70, 20, col::text);
  txt("Eingehender Anruf", CX, 112, &fonts::FreeSansBold9pt7b, col::green);
  char who[48];
  fit(who, sizeof(who), st.caller, &fonts::FreeSansBold12pt7b, 190);
  txt(who, CX, 140, &fonts::FreeSansBold12pt7b, col::text);
  txt(silencedCall == st.callId ? "stumm" : "Drehen = stumm", CX, 172, &fonts::Font0, col::dim);
  txt("Druecken = OK", CX, 186, &fonts::Font0, col::dim);
}

static void drawReminder() {
  const CalEvent& e = st.cal.ev[reminderIdx];
  time_t t = now();
  canvas.fillScreen(c565(35, 20, 10));
  presenceRim(col::claude);
  drawIcon(Icon::Calendar, CX, 52, 16, col::claude);
  char b[48], c[16], d[16];
  long mins = (long)((e.start - t + 59) / 60);
  if (mins > 0) snprintf(b, sizeof(b), "in %ld Min", mins);
  else strlcpy(b, "jetzt", sizeof(b));
  txt(b, CX, 90, &fonts::FreeSansBold18pt7b, col::claude);
  fit(b, sizeof(b), e.subject, &fonts::FreeSansBold12pt7b, 196);
  txt(b, CX, 126, &fonts::FreeSansBold12pt7b, col::text);
  fmtClock(c, sizeof(c), e.start);
  fmtClock(d, sizeof(d), e.end);
  snprintf(b, sizeof(b), "%s - %s", c, d);
  txt(b, CX, 154, &fonts::FreeSans9pt7b, col::dim);
  if (e.location[0]) {
    fit(b, sizeof(b), e.location, &fonts::Font2, 170);
    txt(b, CX, 176, &fonts::Font2, col::dim);
  }
  txt("Druecken = OK", CX, 198, &fonts::Font0, col::dim);
}

static void drawToast() {
  if (millis() >= toastUntil) return;
  char b[64];
  fit(b, sizeof(b), toastText, &fonts::FreeSansBold9pt7b, 176);
  int w = canvas.textWidth(b) + 28;
  canvas.fillRoundRect(CX - w / 2, 196, w, 28, 14, c565(25, 25, 25));
  canvas.drawRoundRect(CX - w / 2, 196, w, 28, 14, toastColor);
  txt(b, CX, 210, &fonts::FreeSansBold9pt7b, col::text);
}

static bool msOverlay = false;  // Microsoft-Code anzeigen

static void render() {
  canvas.fillScreen(col::bg);
  if (st.apMode) {
    drawSetup();
  } else if (ringingActive()) {
    drawRinging();
  } else if (reminderIdx >= 0) {
    drawReminder();
  } else if (msOverlay && st.ms.pending) {
    drawMsLogin();
  } else {
    if (st.teams.ok) presenceRim(presenceColor(st.teams.availability));
    switch (scr) {
      case MENU: drawMenu(); break;
      case CLAUDE: drawClaude(); break;
      case CALENDAR: drawCalendar(); break;
      case SPOTIFY: drawSpotify(); break;
      case PRESENCE: drawPresence(); break;
      case SETTINGS: drawSettings(); break;
    }
  }
  drawToast();
  canvas.pushSprite(0, 0);
}

// ============================================================ Eingaben
static void onRotate(int d) {
  switch (scr) {
    case MENU: menuSel = wrap(menuSel + d, MENU_N); break;
    case CLAUDE: sel = wrap(sel + d, 2); break;
    case CALENDAR: sel = constrain(sel + d, 0, max(0, st.cal.n - 1)); break;
    case SPOTIFY:
      if (!st.sp.active) break;
      if (!st.sp.supportsVolume) {
        toast("Geraet ohne Lautstaerke", col::orange);
        break;
      }
      if (spVol < 0 || millis() - spVolInputAt > 4000) spVol = st.sp.volume;
      spVol = constrain(spVol + d * SP_VOL_STEP, 0, 100);
      spVolInputAt = millis();
      spVolPending = true;
      return;  // kein Klick-Ton beim Drehen der Lautstärke
    case PRESENCE: sel = wrap(sel + d, PRESENCE_N); break;
    case SETTINGS:
      if (!editing) {
        sel = wrap(sel + d, SET_N);
        break;
      }
      switch (sel) {
        case S_BRIGHT: S.bright = constrain(S.bright + d * 15, 10, 255); break;
        case S_SOUND: S.sound = !S.sound; break;
        case S_VOL: S.vol = constrain(S.vol + d * 25, 0, 255); break;
        case S_HOME: S.homeIdx = wrap(S.homeIdx + d, HOME_N); break;
        case S_REMIND: S.remindIdx = wrap(S.remindIdx + d, REMIND_N); break;
      }
      applyOutput();
      break;
  }
  beep(6000, 4);
}

static void onClick(int touchX) {
  switch (scr) {
    case MENU: setScreen(MENU_ITEMS[menuSel].scr); break;
    case SPOTIFY:
      if (!st.sp.active) {
        backToMenu();
        break;
      }
      if (touchX >= 0 && touchX < 85) netJob(Job::SpPrev);
      else if (touchX > 155) netJob(Job::SpNext);
      else netJob(st.sp.playing ? Job::SpPause : Job::SpPlay);
      break;
    case PRESENCE: {
      if (!st.msLinked) {
        backToMenu();
        break;
      }
      netJob(Job::SetPresence, 0, PRESENCE_KEYS[sel]);
      char t[48];
      snprintf(t, sizeof(t), "-> %s", sel == PRESENCE_N - 1 ? "Zuruecksetzen" : presenceLabel(PRESENCE_KEYS[sel]));
      toast(t, presenceOptColor(sel));
      break;
    }
    case SETTINGS:
      if (sel <= S_REMIND) {
        if (editing) settingsSave();
        editing = !editing;
      } else if (sel == S_TESTRING) {
        netRing("Testanruf");
      } else if (sel == S_REFRESH) {
        netJob(Job::RefreshAll);
        toast("aktualisiere...");
      }
      break;
    default: backToMenu();
  }
  beep(2500, 12);
}

static void onLongPress() {
  if (scr == SETTINGS && editing) {
    settingsSave();
    editing = false;
  } else if (scr != MENU) {
    backToMenu();
  }
  beep(1500, 30);
}

static void handleInput() {
  static long encLast = 0;
  long enc = M5Dial.Encoder.read();
  int detents = (enc - encLast) / ENC_STEPS_PER_DETENT;
  if (detents) encLast += (long)detents * ENC_STEPS_PER_DETENT;
  if (ENC_INVERT) detents = -detents;

  bool click = M5Dial.BtnA.wasClicked();
  bool hold = M5Dial.BtnA.wasHold();
  int touchX = -1;
  auto t = M5Dial.Touch.getDetail();
  if (t.wasClicked()) {
    click = true;
    touchX = t.x;
  }
  if (!detents && !click && !hold) return;
  dirty = true;
  bool wasDim = dimmed;
  wake();
  if (wasDim) return;  // erste Eingabe weckt nur auf

  if (st.apMode) return;
  if (ringingActive()) {
    if (click || hold) {
      dismissedCall = st.callId;
      netRingStop();
    } else {
      silencedCall = st.callId;
    }
    M5Dial.Speaker.stop();
    return;
  }
  if (reminderIdx >= 0) {
    if (click || hold) reminderIdx = -1;
    return;
  }
  if (msOverlay && st.ms.pending) {
    msOverlay = false;
    return;
  }
  if (detents) onRotate(detents > 0 ? 1 : -1);
  if (hold) onLongPress();
  else if (click) onClick(touchX);
}

// ============================================================ Ereignisse
static uint32_t eventKey(const CalEvent& e) {
  uint32_t h = (uint32_t)e.start * 2654435761u;
  for (const char* p = e.subject; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  return h;
}

static void checkReminders() {
  uint8_t lead = REMIND_OPTS[S.remindIdx];
  time_t t = now();
  if (reminderIdx >= 0) {  // offene Erinnerung: Index nachführen / beenden
    int found = -1;
    for (int i = 0; i < st.cal.n; i++)
      if (eventKey(st.cal.ev[i]) == reminderKey) found = i;
    reminderIdx = found;
    if (found >= 0 && t > st.cal.ev[found].start + 300) reminderIdx = -1;
    return;
  }
  if (!lead || !st.timeOk) return;
  for (int i = 0; i < st.cal.n; i++) {
    const CalEvent& e = st.cal.ev[i];
    if (e.allDay || !strcmp(e.showAs, "free")) continue;
    if (t < e.start - lead * 60 || t > e.start + 60) continue;
    uint32_t k = eventKey(e);
    bool done = false;
    for (uint32_t r : remindedKeys) done |= r == k;
    if (done) continue;
    remindedKeys[remindedPos++ % 12] = k;
    reminderIdx = i;
    reminderKey = k;
    wake();
    dirty = true;
    if (S.sound) {
      M5Dial.Speaker.tone(1046, 120);
      M5Dial.Speaker.tone(1318, 120, -1, false);
      M5Dial.Speaker.tone(1568, 200, -1, false);
    }
    return;
  }
}

static void handleEvents() {
  AppState n;
  netGetState(n);
  bool newData = n.seq != lastSeq;
  bool msWasPending = st.ms.pending;
  if (newData) {
    lastSeq = n.seq;
    st = n;
    dirty = true;
  }
  if (st.ms.pending && !msWasPending) {
    msOverlay = true;
    wake();
  }

  // Hinweise (Webhook, Teams-Anruf, Fehler)
  if (st.notifyId != lastNotifyId) {
    if (notifyInit && st.notifyText[0]) {
      uint16_t c = !strcmp(st.notifyLevel, "warn") ? col::orange : !strcmp(st.notifyLevel, "call") ? col::green : col::blue;
      toast(st.notifyText, c, 5000);
      if (S.sound) {
        M5Dial.Speaker.tone(1800, 80);
        if (!strcmp(st.notifyLevel, "warn")) M5Dial.Speaker.tone(1200, 160, -1, false);
      }
      wake();
    }
    lastNotifyId = st.notifyId;
    notifyInit = true;
  }

  // Klingeln
  if (ringingActive()) {
    wake();
    dirty = true;
    if (silencedCall != st.callId && millis() >= nextRingMs) {
      beep(ringStep % 2 ? 1320 : 1760, 180);
      ringStep++;
      nextRingMs = millis() + (ringStep % 4 == 0 ? 1700 : 220);
    }
  }

  // Termin-Erinnerung (einmal pro Sekunde reicht)
  static uint32_t lastRemCheck = 0;
  if (millis() - lastRemCheck > 1000 || newData) {
    lastRemCheck = millis();
    checkReminders();
  }

  // Spotify-Lautstärke verzögert senden
  if (spVolPending && millis() - spVolInputAt > 250) {
    spVolPending = false;
    netJob(Job::SpVolume, spVol);
  }

  // Leerlauf: Startseite / dimmen
  uint32_t idle = millis() - lastInput;
  uint16_t home = HOME_OPTS[S.homeIdx];
  if (home && idle > home * 1000UL && !(scr == CLAUDE && sel == 0) && !(scr == SETTINGS && editing)) {
    setScreen(CLAUDE);
  }
  if (!dimmed && DIM_AFTER_S > 0 && idle > DIM_AFTER_S * 1000UL) {
    dimmed = true;
    applyOutput();
  }
}

// ============================================================ Arduino
static void splash() {
  canvas.fillScreen(col::bg);
  gauge(102, 114, 0.66f, col::claude);
  txt("Claude Dial", CX, 104, &fonts::FreeSansBold18pt7b, col::text);
  txt("Nutzung  Kalender  Musik", CX, 138, &fonts::FreeSans9pt7b, col::dim);
  txt("verbinde WLAN...", CX, 170, &fonts::Font0, col::dim);
  canvas.pushSprite(0, 0);
}

void setup() {
  auto m5cfg = M5.config();
  M5Dial.begin(m5cfg, true, false);  // Encoder an, RFID aus
  M5Dial.BtnA.setHoldThresh(600);
  settingsLoad();
  applyOutput();
  canvas.setColorDepth(16);
  if (!canvas.createSprite(240, 240)) {  // zu wenig Speicher -> 8 Bit Farbtiefe
    canvas.setColorDepth(8);
    canvas.createSprite(240, 240);
  }
  log_i("Heap nach Sprite: %u, groesster Block %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  splash();
  netBegin();
  lastInput = millis();
  scr = CLAUDE;
  netSetFocus(Focus::Claude);
}

void loop() {
  M5Dial.update();
  handleInput();
  handleEvents();
  webLoop(st.apMode, st.wifi);

  static uint32_t lastRender = 0;
  bool anim = ringingActive() || millis() < toastUntil + 50 || (scr == SPOTIFY && st.sp.playing);
  uint32_t period = ringingActive() ? 40 : anim ? 250 : 1000;
  if (dirty || millis() - lastRender >= period) {
    render();
    lastRender = millis();
    dirty = false;
  }
  delay(5);
}
