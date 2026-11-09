#pragma once
#include <M5Dial.h>

// Bildschirm: 240x240, rund (GC9A01). Mittelpunkt 120/120.
constexpr int CX = 120, CY = 120, R = 120;

// Farben (RGB565)
constexpr uint16_t c565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
namespace col {
constexpr uint16_t bg = c565(0, 0, 0);
constexpr uint16_t text = c565(255, 255, 255);
constexpr uint16_t dim = c565(140, 140, 140);
constexpr uint16_t faint = c565(45, 45, 45);
constexpr uint16_t track = c565(30, 30, 30);
constexpr uint16_t claude = c565(217, 119, 87);     // Claude-Orange #D97757
constexpr uint16_t claudeDim = c565(90, 45, 30);
constexpr uint16_t green = c565(46, 204, 113);
constexpr uint16_t yellow = c565(250, 200, 30);
constexpr uint16_t orange = c565(255, 140, 0);
constexpr uint16_t red = c565(235, 60, 50);
constexpr uint16_t darkred = c565(160, 0, 0);
constexpr uint16_t blue = c565(70, 140, 255);
constexpr uint16_t violet = c565(140, 100, 255);
}  // namespace col

extern M5Canvas canvas;

// Text
void txt(const char* s, int x, int y, const lgfx::IFont* font, uint16_t color,
         uint8_t datum = middle_center);
void txtf(int x, int y, const lgfx::IFont* font, uint16_t color, const char* fmt, ...);

// Formatierung
void fmtMoney(char* out, size_t n, float usd);
void fmtTokens(char* out, size_t n, double tok);
void fmtMinutes(char* out, size_t n, int min);
void fmtClock(char* out, size_t n, time_t t, bool withDay = false);  // lokale Uhrzeit

// Grafik-Bausteine
// Gauge über 270° (unten offen). pct 0..1 (wird begrenzt), marker <0 = keiner
void gauge(int r0, int r1, float pct, uint16_t color, float marker = -1);
// Ring-Auswahl: n Segmente rund um den Rand, Segment sel hervorgehoben.
// colors == nullptr -> Standardfarben.
void ringSelector(int n, int sel, const uint16_t* colors = nullptr, int rOuter = 118, int width = 8);
// Seitenpunkte (unten) für Datenseiten
void pageDots(int n, int sel, int y = 214);
// Statusring außen in Teams-Präsenzfarbe
void presenceRim(uint16_t color);
uint16_t pctColor(float pct);

// Icons (Mittelpunkt x/y, Größe ~ 2*s)
enum class Icon { Claude, Calendar, Music, Presence, Settings, Bell, Play, Pause, Next, Prev, Wifi };
void drawIcon(Icon ic, int x, int y, int s, uint16_t color);

// Teams
uint16_t presenceColor(const char* availability);
const char* presenceLabel(const char* availability);
