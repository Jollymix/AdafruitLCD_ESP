# Prosjekt: Utetemperatur-display (Homey Pro → ESP32 + Adafruit TFT)

## Maskinvare
- **MCU:** ESP-32D-devkit (klassisk ESP32-WROOM-32D, 4 MB flash, CH340C USB-serie, én USB-C). Kort i Arduino: `esp32:esp32:esp32` («ESP32 Dev Module»).
- **Skjerm:** Adafruit 2.2" SPI-TFT 240×320 med SD-spor (ILI9340C/ILI9341), brukt liggende (320×240).
- Temperaturen kommer fra en sensor i **Homey Pro** via det lokale HTTP-API-et.

## Kobling (fast, ikke endre uten å spørre)
| Skjerm | ESP32 |
|---|---|
| GND | GND |
| VIN | 3V3 |
| D/C | GPIO16 |
| RESET | GPIO17 |
| LCD CS | GPIO5 |
| MOSI | GPIO23 |
| SCK | GPIO18 |
| BACKLIGHT | GPIO21 (PWM) |
| MISO, SD CS | ikke koblet |

## Bygg
- Arduino-ESP32 core **3.x**
- Biblioteker: **LovyanGFX**, **ArduinoJson v7**
- **Partition scheme: Minimal SPIFFS** (`PartitionScheme=min_spiffs`). Skissen bruker ~96 % av standardpartisjonen.
- arduino-cli:
  ```
  arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs .
  arduino-cli upload  --fqbn esp32:esp32:esp32:PartitionScheme=min_spiffs -p <PORT> .
  arduino-cli monitor -p <PORT> -c baudrate=115200
  ```

## Filer
- `homeyDisplay.ino`: all logikk (WiFi, NTP, Homey-henting, 24 t historikk i NVS, tegning). Navnet må matche mappenavnet, ellers nekter arduino-cli å kompilere.
- `fonts.h`: generert. **Ikke rediger for hånd.** Regenereres med `python3 extras/make_fonts.py` (krever `freetype-py`). Fonten er B612 (Airbus, OFL).
- `extras/`: fontverktøy og lisens.

## Designkrav (avtalt med eieren, skal beholdes)
- Mørk bakgrunn, Apple-inspirert stil med iOS dark mode-farger og B612-font overalt.
- Stort **hvitt** tall med **én desimal**, norsk komma og «°C», alt i **samme fontstørrelse** som én enkel tekst uten mellomrom (f.eks. «9,8°C»). Ingen opphøyde/mindre deler. FONT_BIG er 76 px: «−30,0°C» er 271 px bred og får ≥ 20 px margin på 320 px (B612 har like brede tegn).
- Diskret 24-timers trend under tallet: tynn kantutjevnet kurve, **rød over 0°, blå under 0°**, svak gradient under, stiplet nullinje og prikk for «nå». Trenden er godkjent og skal ikke endres.
- Topplinje: «Ute» til venstre, «H x°  L y°» til høyre. En oransje prikk vises etter 10 min uten data.
- Dimming kl. 23–06.

## Kjente usikkerheter
- `cfg.rgb_order` kan måtte settes til `true` hvis rødt og blått er byttet om.
- Homey Insights-endepunktet er verifisert (Homey på 192.168.1.28, okt. 2026): `GET /api/manager/insights/log/homey:device:<ID>/homey:device:<ID>:measure_temperature/entry?resolution=last24Hours` (jf. `homey-api`-spesifikasjonen `/log/:uri/:id/entry`). API-nøkkelen må ha Insights-tilgang, ellers 403. Uten data faller koden tilbake til å bygge historikken lokalt.
- WiFi (mesh «HomerJay131», kanal 4, ca. −66 dBm): kortet feilet handshake (grunn 15/204) på full sendeeffekt. Løst med `WiFi.setTxPower(WIFI_POWER_8_5dBm)` og valg av sterkeste AP. Ikke fjern dette.
- Hemmeligheter (WiFi, Homey-IP, API-nøkkel, enhets-ID) ligger i `secrets.h` (git-ignorert). Mal: `secrets.example.h`.
