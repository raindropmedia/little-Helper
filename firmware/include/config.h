#pragma once
// ---------------------------------------------------------------------------
// Claude Dial – Kompilier-Konstanten. WLAN, Logins usw. werden NICHT hier,
// sondern über die Webkonfiguration (http://claude-dial.local) eingestellt.
// ---------------------------------------------------------------------------

// Hostname im Netz (mDNS: http://claude-dial.local)
#define HOSTNAME "claude-dial"

// Setup-Hotspot, wenn kein WLAN eingerichtet oder erreichbar ist
#define AP_SSID "Claude-Dial-Setup"
#define AP_PASS "claudedial"  // mind. 8 Zeichen

// Klingeln endet nach so vielen Sekunden automatisch
#define RING_TIMEOUT_S 45

// Encoder-Schritte pro Rastung (M5Dial: 4). Springt ein Klick zwei Einträge: anpassen.
#define ENC_STEPS_PER_DETENT 4
#define ENC_INVERT 0

// Nach so vielen Sekunden ohne Eingabe wird das Display gedimmt
#define DIM_AFTER_S 120

// Spotify: Lautstärkeschritt pro Rastung (Prozent)
#define SP_VOL_STEP 3
