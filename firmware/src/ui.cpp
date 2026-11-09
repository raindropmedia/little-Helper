#include "ui.h"

#include <stdarg.h>

M5Canvas canvas(&M5.Display);  // M5.Display: statisch adressierbar (M5Dial.Display ist eine Referenz)

// ---------------------------------------------------------------- Text
void txt(const char* s, int x, int y, const lgfx::IFont* font, uint16_t color, uint8_t datum) {
  canvas.setFont(font);
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  canvas.drawString(s, x, y);
}

void txtf(int x, int y, const lgfx::IFont* font, uint16_t color, const char* fmt, ...) {
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  txt(buf, x, y, font, color);
}

// ---------------------------------------------------------------- Formatierung
void fmtMoney(char* out, size_t n, float usd) {
  if (usd >= 10000.f)
    snprintf(out, n, "$%.1fk", usd / 1000.f);
  else if (usd >= 100.f)
    snprintf(out, n, "$%.0f", usd);
  else
    snprintf(out, n, "$%.2f", usd);
}

void fmtTokens(char* out, size_t n, double t) {
  if (t >= 1e9)
    snprintf(out, n, "%.2fB", t / 1e9);
  else if (t >= 1e6)
    snprintf(out, n, "%.1fM", t / 1e6);
  else if (t >= 1e3)
    snprintf(out, n, "%.0fk", t / 1e3);
  else
    snprintf(out, n, "%.0f", t);
}

void fmtClock(char* out, size_t n, time_t t, bool withDay) {
  static const char* WD[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
  struct tm tm;
  localtime_r(&t, &tm);
  if (withDay) snprintf(out, n, "%s %02d:%02d", WD[tm.tm_wday], tm.tm_hour, tm.tm_min);
  else snprintf(out, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void fmtMinutes(char* out, size_t n, int m) {
  if (m < 0) m = 0;
  snprintf(out, n, "%d:%02dh", m / 60, m % 60);
}

// ---------------------------------------------------------------- Bögen
// LovyanGFX: 0° = 3 Uhr, im Uhrzeigersinn. Bereiche > 360° werden aufgeteilt.
static void arc(int r0, int r1, float a0, float a1, uint16_t color) {
  if (a1 <= a0) return;
  while (a0 >= 360.f) { a0 -= 360.f; a1 -= 360.f; }
  if (a1 > 360.f) {
    canvas.fillArc(CX, CY, r0, r1, a0, 360.f, color);
    canvas.fillArc(CX, CY, r0, r1, 0.f, a1 - 360.f, color);
  } else {
    canvas.fillArc(CX, CY, r0, r1, a0, a1, color);
  }
}

uint16_t pctColor(float pct) {
  if (pct >= 0.9f) return col::red;
  if (pct >= 0.7f) return col::orange;
  return col::green;
}

void gauge(int r0, int r1, float pct, uint16_t color, float marker) {
  const float A0 = 135.f, SPAN = 270.f;
  if (pct < 0) pct = 0;
  arc(r0, r1, A0, A0 + SPAN, col::track);
  float p = pct > 1.f ? 1.f : pct;
  if (p > 0.003f) arc(r0, r1, A0, A0 + SPAN * p, color);
  // abgerundete Enden
  if (p > 0.003f) {
    float rm = (r0 + r1) / 2.f, w = (r1 - r0) / 2.f;
    float a = (A0 + SPAN * p) * DEG_TO_RAD;
    canvas.fillCircle(CX + cosf(A0 * DEG_TO_RAD) * rm, CY + sinf(A0 * DEG_TO_RAD) * rm, w, color);
    canvas.fillCircle(CX + cosf(a) * rm, CY + sinf(a) * rm, w, color);
  }
  if (marker >= 0) {
    float m = marker > 1.f ? 1.f : marker;
    float a = A0 + SPAN * m;
    arc(r0 - 5, r1 + 3, a - 1.2f, a + 1.2f, col::text);
  }
  // Überschreitung: zweite Runde dünn innen
  if (pct > 1.f) {
    float over = pct - 1.f;
    if (over > 1.f) over = 1.f;
    arc(r0 - 6, r0 - 3, A0, A0 + SPAN * over, col::red);
  }
}

void ringSelector(int n, int sel, const uint16_t* colors, int rOuter, int width) {
  if (n <= 0) return;
  float step = 360.f / n;
  float gap = n > 12 ? 2.f : 4.f;
  for (int i = 0; i < n; i++) {
    float c = 270.f + i * step;
    float a0 = c - step / 2 + gap / 2, a1 = c + step / 2 - gap / 2;
    uint16_t cl = colors ? colors[i] : col::claude;
    if (i == sel) {
      arc(rOuter - width - 4, rOuter, a0, a1, cl);
    } else {
      uint16_t d = colors ? cl : col::claudeDim;
      arc(rOuter - width / 2 - 1, rOuter - 2, a0, a1, colors ? col::faint : d);
      if (colors) arc(rOuter - 4, rOuter - 2, a0, a1, d);
    }
  }
}

void pageDots(int n, int sel, int y) {
  int w = (n - 1) * 10;
  for (int i = 0; i < n; i++) {
    int x = CX - w / 2 + i * 10;
    if (i == sel)
      canvas.fillCircle(x, y, 3, col::claude);
    else
      canvas.fillCircle(x, y, 2, col::dim);
  }
}

void presenceRim(uint16_t color) {
  canvas.drawCircle(CX, CY, 119, color);
  canvas.drawCircle(CX, CY, 118, color);
}

// ---------------------------------------------------------------- Icons
static void gear(int x, int y, int s, uint16_t c) {
  for (int i = 0; i < 8; i++) {
    float a = 22.5f + i * 45.f;
    canvas.fillArc(x, y, s * 0.62f, s, a - 11, a + 11, c);
  }
  canvas.fillCircle(x, y, s * 0.72f, c);
  canvas.fillCircle(x, y, s * 0.30f, col::bg);
}

static void tri(int x, int y, int s, bool right, uint16_t c) {
  if (right) canvas.fillTriangle(x - s / 2, y - s, x - s / 2, y + s, x + s, y, c);
  else canvas.fillTriangle(x + s / 2, y - s, x + s / 2, y + s, x - s, y, c);
}

void drawIcon(Icon ic, int x, int y, int s, uint16_t c) {
  switch (ic) {
    case Icon::Claude:  // Gauge mit Zeiger
      canvas.fillArc(x, y, s * 0.7f, s, 135, 315, c);
      canvas.fillArc(x, y, s * 0.7f, s, 315, 360, col::faint);
      canvas.fillArc(x, y, s * 0.7f, s, 0, 45, col::faint);
      canvas.fillTriangle(x, y - 3, x, y + 3, x + s * 0.55f, y - s * 0.55f, c);
      canvas.fillCircle(x, y, 4, c);
      break;
    case Icon::Calendar:
      canvas.fillRoundRect(x - s, y - s * 0.75f, s * 2, s * 1.75f, 5, c);
      canvas.fillRect(x - s + 3, y - s * 0.3f, s * 2 - 6, s * 1.2f, col::bg);
      canvas.fillRect(x - s * 0.55f, y - s, 4, s * 0.5f, c);
      canvas.fillRect(x + s * 0.55f - 4, y - s, 4, s * 0.5f, c);
      for (int i = 0; i < 3; i++)
        for (int j = 0; j < 2; j++) canvas.fillRect(x - s * 0.7f + i * s * 0.55f, y - s * 0.1f + j * s * 0.45f, s * 0.3f, s * 0.28f, c);
      break;
    case Icon::Music:
      canvas.fillEllipse(x - s * 0.55f, y + s * 0.6f, s * 0.35f, s * 0.27f, c);
      canvas.fillEllipse(x + s * 0.55f, y + s * 0.4f, s * 0.35f, s * 0.27f, c);
      canvas.fillRect(x - s * 0.27f, y - s * 0.8f, 4, s * 1.4f, c);
      canvas.fillRect(x + s * 0.83f, y - s, 4, s * 1.4f, c);
      canvas.fillTriangle(x - s * 0.27f, y - s * 0.8f, x + s * 0.87f, y - s, x + s * 0.87f, y - s * 0.6f, c);
      canvas.fillTriangle(x - s * 0.27f, y - s * 0.8f, x - s * 0.27f, y - s * 0.4f, x + s * 0.87f, y - s * 0.6f, c);
      break;
    case Icon::Presence:
      canvas.fillCircle(x, y - s * 0.35f, s * 0.38f, c);
      canvas.fillArc(x, y + s, s * 0.25f, s * 0.95f, 180, 360, c);
      break;
    case Icon::Bell:
      canvas.fillArc(x, y, 0, s * 0.75f, 180, 360, c);
      canvas.fillRect(x - s * 0.75f, y, s * 1.5f, s * 0.5f, c);
      canvas.fillRoundRect(x - s, y + s * 0.45f, s * 2, s * 0.25f, 2, c);
      canvas.fillCircle(x, y + s * 0.9f, s * 0.2f, c);
      break;
    case Icon::Play: tri(x, y, s, true, c); break;
    case Icon::Pause:
      canvas.fillRoundRect(x - s * 0.7f, y - s, s * 0.5f, s * 2, 2, c);
      canvas.fillRoundRect(x + s * 0.2f, y - s, s * 0.5f, s * 2, 2, c);
      break;
    case Icon::Next:
      tri(x - s * 0.3f, y, s * 0.8f, true, c);
      canvas.fillRect(x + s * 0.6f, y - s * 0.8f, 3, s * 1.6f, c);
      break;
    case Icon::Prev:
      tri(x + s * 0.3f, y, s * 0.8f, false, c);
      canvas.fillRect(x - s * 0.6f - 3, y - s * 0.8f, 3, s * 1.6f, c);
      break;
    case Icon::Wifi:
      for (int i = 0; i < 3; i++) canvas.fillArc(x, y + s * 0.6f, s * (0.45f + i * 0.4f), s * (0.6f + i * 0.4f), 225, 315, c);
      canvas.fillCircle(x, y + s * 0.6f, s * 0.18f, c);
      break;
    case Icon::Settings: gear(x, y, s, c); break;
  }
}

// ---------------------------------------------------------------- Teams
uint16_t presenceColor(const char* a) {
  if (!strncmp(a, "Available", 9)) return col::green;
  if (!strcmp(a, "DoNotDisturb")) return col::darkred;
  if (!strncmp(a, "Busy", 4)) return col::red;
  if (!strcmp(a, "BeRightBack") || !strcmp(a, "Away")) return col::yellow;
  if (!strcmp(a, "Offline")) return col::dim;
  return col::faint;
}

// GFX-Schriften enthalten nur ASCII -> Umlaute umschrieben
const char* presenceLabel(const char* a) {
  if (!strcmp(a, "Available")) return "Verfuegbar";
  if (!strcmp(a, "AvailableIdle")) return "Verfuegbar (inaktiv)";
  if (!strcmp(a, "Busy")) return "Beschaeftigt";
  if (!strcmp(a, "BusyIdle")) return "Beschaeftigt";
  if (!strcmp(a, "DoNotDisturb")) return "Nicht stoeren";
  if (!strcmp(a, "BeRightBack")) return "Gleich zurueck";
  if (!strcmp(a, "Away")) return "Abwesend";
  if (!strcmp(a, "Offline")) return "Offline";
  return "Unbekannt";
}
