# Claude Dial – Claude-Nutzung, Kalender, Spotify & Teams am M5Stack Dial

Eigenständige Firmware für den **M5Stack Dial v1.1** (ESP32-S3, 1,28" rundes Touch-Display, Drehrad).
Kein PC und kein Server nötig. Der Dial verbindet sich selbst per HTTPS mit Claude, Microsoft 365 und Spotify. Eingerichtet wird alles im Browser unter **http://claude-dial.local**.

## Funktionen

| Menü | Anzeige | Drehen | Drücken / Tippen |
|---|---|---|---|
| **Claude** (Startseite) | 5-Stunden-Fenster und Wochenlimit als Doppelring, Reset-Zeiten. 2. Seite: Opus-/Sonnet-Wochenlimit, Zusatzkontingent | Übersicht ↔ Details | Menü |
| **Kalender** | Termine der nächsten 36 h (Outlook/Teams), laufende Termine grün, „in 12 Min“ | Liste scrollen | Menü |
| **Spotify** | Titel, Interpret, Gerät, Fortschritt, Lautstärke als Ring | **Lautstärke** | Play/Pause (Tippen links = zurück, rechts = weiter) |
| **Teams Status** | Verfügbar, Beschäftigt, Nicht stören, Gleich zurück, Abwesend, Offline, Zurücksetzen | Auswahl | Status setzen |
| **Einstellungen** | Helligkeit, Ton, Lautstärke, Auto-Startseite, Termin-Erinnerung, Testanruf, Aktualisieren, Info | Auswahl/Wert | bearbeiten |

**Lang drücken** führt immer zurück zum Menü.

Zusätzlich gibt es diese Einblendungen:
- **Termin-Erinnerung** (Standard 5 Min vorher, einstellbar): Vollbild mit Titel, Uhrzeit, Ort und Dreiklang.
- **Eingehender Anruf** über den Webhook `POST /api/ring`: Vollbild-Alarm mit Klingelton. Drehen = stumm, Drücken = OK.
- **Teams-Status-Ring** am Displayrand in der Farbe deiner Präsenz und der Hinweis „Teams: Anruf läuft“.
- **Hinweise** von außen über `POST /api/notify`, z. B. „Build fertig“.

## Einrichtung

### 1. Flashen

Mit [PlatformIO](https://platformio.org/) (VS Code-Erweiterung oder CLI). Den Dial per USB-C anschließen:

```bash
cd firmware
```

```bash
pio run -t upload
```

### 2. WLAN

Beim ersten Start (oder wenn das WLAN nicht erreichbar ist) öffnet der Dial den Hotspot **Claude-Dial-Setup** (Passwort `claudedial`).
Verbinde dich damit, die Setup-Seite öffnet sich automatisch (sonst `http://192.168.4.1`). Gib dort dein WLAN ein, der Dial startet neu.
Danach ist er unter **http://claude-dial.local** erreichbar. Die IP steht auch unter *Einstellungen → Info*.
In der Webkonfiguration solltest du unter *Allgemein* ein Passwort setzen (Benutzer `admin`).

### 3. Claude-Nutzung

1. In der Webkonfiguration **„Bei Claude anmelden“** klicken. claude.ai öffnet sich, dort anmelden und zustimmen.
2. Claude zeigt einen Code an (`…#…`). Diesen kopieren und ins Feld **„Code übernehmen“** einfügen.

Der Dial bekommt eine **eigene** Sitzung und erneuert sein Token selbst. Dein Claude Code am PC bleibt angemeldet.

> ⚠️ Die Nutzungsdaten kommen von der Schnittstelle, die Claude Code für `/usage` verwendet (`/api/oauth/usage`). Sie ist nicht offiziell dokumentiert und kann sich ändern. Login-Parameter: öffentlicher Client und manueller Code-Flow von Claude Code, Scope nur `user:profile`.

### 4. Microsoft 365 (Teams-Status + Kalender)

1. Im [Entra-Portal](https://entra.microsoft.com) unter *App-Registrierungen → Neue Registrierung* eine App anlegen (Konten in diesem Organisationsverzeichnis).
2. Unter *Authentifizierung* die Option **„Öffentliche Clientflows zulassen“** auf *Ja* stellen.
3. Unter *API-Berechtigungen* die delegierten Microsoft-Graph-Berechtigungen `Presence.ReadWrite`, `Calendars.Read`, `User.Read` und `offline_access` hinzufügen.
4. Die *Anwendungs-ID (Client-ID)* in der Webkonfiguration eintragen und **„Anmelden (Gerätecode)“** klicken.
5. Auf dem Dial und auf der Webseite erscheint ein Code. Diesen unter <https://microsoft.com/devicelogin> eingeben.

An Hochschulen und in Firmen kann es sein, dass ein Administrator der App zustimmen muss. Wenn die Registrierung gesperrt ist, an die IT wenden.

### 5. Spotify (Lautstärke & Wiedergabe)

1. Unter <https://developer.spotify.com/dashboard> eine App anlegen. Als **Redirect URI** `http://127.0.0.1:8888/callback` eintragen und *Web API* auswählen.
2. Die *Client ID* in der Webkonfiguration speichern und **„Bei Spotify anmelden“** klicken.
3. Nach der Zustimmung landet der Browser auf `http://127.0.0.1:8888/callback?code=…`. Die Seite lädt nicht, das ist normal.
4. Die **komplette Adresse** aus der Adresszeile kopieren und ins Feld einfügen.

Die Steuerung (Lautstärke, Play/Pause, Titelwechsel) setzt **Spotify Premium** voraus. Manche Geräte (z. B. Smartphones) erlauben keine Fernsteuerung der Lautstärke. Der Dial zeigt das dann an.

### 6. Rufsignalisierung (Webhook)

Teams gibt eingehende Anrufe über keine öffentliche Schnittstelle heraus. Der Dial klingelt, wenn ihm jemand einen Anruf meldet, z. B. Power Automate, eine Telefonanlage oder ein Skript:

```bash
curl -X POST http://claude-dial.local/api/ring -H "X-Dial-Token: GEHEIM" -d '{"caller":"Erika Mustermann"}'
```

Weitere Endpunkte: `POST /api/ring/stop` und `POST /api/notify` mit `{"text":"Build fertig","level":"info|warn|call"}`.
Das Token legst du in der Webkonfiguration fest. Ohne Token darf jeder im Netz klingeln lassen.

## Projektstruktur

```
firmware/
  platformio.ini
  include/config.h         Hostname, Setup-Hotspot, Encoder, Zeiten
  src/main.cpp             Screens, Navigation, Erinnerungen, Klingeln
  src/net.cpp              WLAN, NTP, OAuth (Claude/Microsoft/Spotify), HTTPS-Abfragen (Core 0)
  src/web.cpp              Webkonfiguration, Captive Portal, Webhooks
  src/settings.cpp/.h      Konfiguration im Flash (NVS)
  src/ui.cpp/.h            Gauges, Ring-Auswahl, Icons, Farben
  src/state.h              Datenmodell
  data/x509_crt_bundle.bin Root-Zertifikate (Mozilla) für TLS-Prüfung
  tools/gen_crt_bundle.py  erzeugt das Zertifikats-Bundle neu
  tools/extra_roots/       Roots, die Mozilla entfernt hat, die große Dienste aber noch nutzen
```

## Hinweise

* **Sicherheit:** Alle Verbindungen nach außen laufen über HTTPS mit Zertifikatsprüfung. Die Tokens liegen im Flash des Dials. Die Webkonfiguration läuft im LAN unverschlüsselt über HTTP, deshalb ein Passwort setzen.
* **Zertifikate erneuern** (selten nötig). Das Bundle enthält zusätzlich *GlobalSign Root CA* (für `api.anthropic.com`) und *DigiCert Global Root CA* aus `tools/extra_roots/`:

  ```bash
  curl -o cacert.pem https://curl.se/ca/cacert.pem
  ```

  ```bash
  python tools/gen_crt_bundle.py cacert.pem data/x509_crt_bundle.bin
  ```

* **Schrift:** Die Display-Schriften enthalten nur ASCII, Umlaute werden auf dem Gerät umschrieben („Verfuegbar“).
* **Drehrad:** Springt ein Klick zwei Einträge weiter, `ENC_STEPS_PER_DETENT` in `include/config.h` anpassen.
* **Speicher:** Der Dial hat kein PSRAM. Der Bildpuffer (115 kB) und eine TLS-Verbindung passen gleichzeitig in den RAM. Abfragen laufen deshalb nacheinander. Falls der Bildpuffer nicht angelegt werden kann, fällt die Firmware auf 8-Bit-Farbe zurück.
* **Status:** Die Firmware kompiliert mit PlatformIO (espressif32 6.9.0, Arduino 2.0.17), wurde aber noch nicht auf echter Hardware getestet.
