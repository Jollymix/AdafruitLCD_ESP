// =====================================================================
//  Utetemperatur – Homey Pro  ->  ESP32 + Adafruit 2.2" TFT (ILI9340/41)
//
//  Stort, rolig tall (rødt i pluss, blått i minus) og en diskret
//  24-timers trend under. Mørkt design i Apple-stil med Airbus sin
//  cockpit-font B612, anti-aliasert tekst og grafikk.
//
//  Biblioteker (Arduino Library Manager):
//    - LovyanGFX   (lovyan03)
//    - ArduinoJson (Benoit Blanchon, v7)
//  Kort: "ESP32 Dev Module", Arduino-ESP32 core 3.x
// =====================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "fonts.h"
#include "secrets.h"

// Store glyfer rendres via stacken – gi loop-tasken god plass.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---------------------------------------------------------------------
//  KONFIG
// ---------------------------------------------------------------------
// Hemmeligheter ligger i secrets.h (ikke i git). Mal: secrets.example.h
const char* WIFI_SSID   = WIFI_SSID_SECRET;
const char* WIFI_PASS   = WIFI_PASS_SECRET;
const char* HOMEY_IP    = HOMEY_IP_SECRET;
const char* HOMEY_TOKEN = HOMEY_TOKEN_SECRET;
const char* DEVICE_ID   = DEVICE_ID_SECRET;

const char* LABEL       = "Ute";

const uint32_t POLL_MS        = 60UL * 1000;       // hent temperatur hvert minutt
const uint32_t STALE_MS       = 10UL * 60 * 1000;  // oransje prikk etter 10 min uten data
const uint8_t  BL_DAY         = 200;               // lysstyrke dag   (0-255)
const uint8_t  BL_NIGHT       = 30;                // lysstyrke natt
const int      NIGHT_FROM_H   = 23;                // natt fra kl. 23
const int      NIGHT_TO_H     = 6;                 // ... til kl. 06
const char*    TZ_INFO        = "CET-1CEST,M3.5.0,M10.5.0/3";  // Norge

// ---------------------------------------------------------------------
//  PINNER (VSPI)
// ---------------------------------------------------------------------
#define PIN_SCK   18
#define PIN_MOSI  23
#define PIN_CS     5
#define PIN_DC    16
#define PIN_RST   17
#define PIN_BL    21
// MISO og SD CS brukes ikke. Bind gjerne SD CS til 3V3.

// ---------------------------------------------------------------------
//  DISPLAY
// ---------------------------------------------------------------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host    = SPI3_HOST;          // = VSPI
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk    = PIN_SCK;
      cfg.pin_mosi    = PIN_MOSI;
      cfg.pin_miso    = -1;
      cfg.pin_dc      = PIN_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs          = PIN_CS;
      cfg.pin_rst         = PIN_RST;
      cfg.pin_busy        = -1;
      cfg.panel_width     = 240;
      cfg.panel_height    = 320;
      cfg.offset_rotation = 0;
      cfg.readable        = false;
      cfg.invert          = false;
      cfg.rgb_order       = false;  // Bytter rødt og blått plass? Sett til true.
      cfg.dlen_16bit      = false;
      cfg.bus_shared      = false;
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl      = PIN_BL;
      cfg.invert      = false;
      cfg.freq        = 12000;
      cfg.pwm_channel = 7;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
    setPanel(&_panel);
  }
};

LGFX tft;

// ---------------------------------------------------------------------
//  DESIGN  (Apple iOS dark mode-farger)
// ---------------------------------------------------------------------
static const int W = 320, H = 240, M = 20;

static const uint32_t C_BG       = 0x000000;
static const uint32_t C_RED      = 0xFF453A;  // systemRed (dark)
static const uint32_t C_BLUE     = 0x0A84FF;  // systemBlue (dark)
static const uint32_t C_WHITE    = 0xFFFFFF;
static const uint32_t C_GRAY     = 0x8E8E93;  // secondaryLabel
static const uint32_t C_GRAY_DIM = 0x48484A;  // tertiary
static const uint32_t C_ORANGE   = 0xFF9F0A;  // systemOrange
static const uint32_t C_NUMBER   = C_WHITE;   // farge på det store tallet

// Soner (y-område) – tegnes hver for seg i en sprite for flimmerfri oppdatering
static const int HDR_Y = 0,   HDR_H = 40;   // "Ute"  +  H / L
static const int NUM_Y = 40,  NUM_H = 132;  // stort tall
static const int CH_Y  = 172, CH_H  = 68;   // trend + akse-tekst

static const int HDR_BASE = 30;   // grunnlinjer i skjermkoordinater
static const int BIG_BASE = 136;  // 76 px-font (sifferhøyde 58) sentrert i tallsonen
static const int CH_TOP   = 176;
static const int CH_BOT   = 214;
static const int LBL_BASE = 232;

// ---------------------------------------------------------------------
//  HISTORIKK: 96 x 15 min = 24 t, lagres i NVS (overlever omstart)
// ---------------------------------------------------------------------
static const int      N_SLOTS  = 96;
static const uint32_t SLOT_SEC = 900;

struct History {
  uint32_t slot[N_SLOTS];   // epoch/900 for prøven i denne plassen
  int16_t  t10[N_SLOTS];    // temperatur * 10
} hist;

Preferences prefs;
uint32_t curSlot = 0;
float    slotSum = 0;
int      slotCnt = 0;

float    curTemp    = NAN;
uint32_t lastOkMs   = 0;
bool     everOk     = false;
uint32_t lastPollMs = 0;

// ---------------------------------------------------------------------
//  Hjelpere
// ---------------------------------------------------------------------
static bool timeValid() { return time(nullptr) > 1700000000; }

static uint16_t rgb565(uint32_t c) {
  return lgfx::color565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// Alfa-blanding direkte i sprite (sprites er alltid lesbare)
static void blendPx(LGFX_Sprite& s, int x, int y, uint32_t c, float a) {
  if (a <= 0.004f || x < 0 || y < 0 || x >= s.width() || y >= s.height()) return;
  if (a > 1.0f) a = 1.0f;
  auto p = s.readPixelRGB(x, y);
  int r = p.R8() + (int)((((c >> 16) & 0xFF) - p.R8()) * a + 0.5f);
  int g = p.G8() + (int)((((c >> 8) & 0xFF) - p.G8()) * a + 0.5f);
  int b = p.B8() + (int)(((c & 0xFF) - p.B8()) * a + 0.5f);
  s.drawPixel(x, y, lgfx::color565(r, g, b));
}

static String fmtTemp(float v) {
  long n = lroundf(v);
  String s = (n < 0) ? "−" : "";   // ekte minustegn
  s += String(labs(n));
  return s;
}

// ---------------------------------------------------------------------
//  Historikk-lagring
// ---------------------------------------------------------------------
static void saveHistory() { prefs.putBytes("h", &hist, sizeof(hist)); }

static void loadHistory() {
  memset(&hist, 0, sizeof(hist));
  if (prefs.getBytesLength("h") == sizeof(hist)) prefs.getBytes("h", &hist, sizeof(hist));
}

static void putSlot(uint32_t slot, float t) {
  int i = slot % N_SLOTS;
  hist.slot[i] = slot;
  hist.t10[i]  = (int16_t)lroundf(t * 10.0f);
}

static int validSlots(uint32_t now) {
  int n = 0;
  for (int i = 0; i < N_SLOTS; i++)
    if (hist.slot[i] && now - hist.slot[i] < N_SLOTS) n++;
  return n;
}

static void recordSample(float t) {
  if (!timeValid()) return;
  uint32_t s = time(nullptr) / SLOT_SEC;
  if (s != curSlot) {
    if (slotCnt) saveHistory();       // forrige kvarter er komplett
    curSlot = s; slotSum = 0; slotCnt = 0;
  }
  slotSum += t; slotCnt++;
  putSlot(curSlot, slotSum / slotCnt); // gjennomsnitt i kvarteret
}

// ---------------------------------------------------------------------
//  Homey: nåverdi
// ---------------------------------------------------------------------
static bool fetchTemp(float& out) {
  HTTPClient http;
  String url = String("http://") + HOMEY_IP + "/api/manager/devices/device/" + DEVICE_ID;
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + HOMEY_TOKEN);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[homey] HTTP %d\n", code);
    http.end();
    return false;
  }
  JsonDocument filter;
  filter["capabilitiesObj"]["measure_temperature"]["value"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[homey] JSON: %s\n", err.c_str()); return false; }
  JsonVariant v = doc["capabilitiesObj"]["measure_temperature"]["value"];
  if (v.isNull()) return false;
  out = v.as<float>();
  return true;
}

// ---------------------------------------------------------------------
//  Homey Insights: fyll historikken ved første oppstart (best effort).
//  Krever at API-nøkkelen har lesetilgang til Insights. Feiler den,
//  bygges grafen bare opp lokalt over tid.
// ---------------------------------------------------------------------
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {  // H. Hinnant
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static uint32_t parseIsoUtc(const char* s) {  // "2026-10-03T06:15:00.000Z"
  int Y, Mo, D, h, mi, se;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &h, &mi, &se) != 6) return 0;
  return (uint32_t)(daysFromCivil(Y, Mo, D) * 86400 + h * 3600 + mi * 60 + se);
}

static bool prefillFromInsights() {
  // GET /api/manager/insights/log/:uri/:id/entry  (uri = eier, id = full logg-ID)
  const String owner = String("homey:device:") + DEVICE_ID;
  const String url = String("http://") + HOMEY_IP + "/api/manager/insights/log/" +
                     owner + "/" + owner + ":measure_temperature/entry?resolution=last24Hours";
  JsonDocument filter;
  filter["values"][0]["t"] = true;
  filter["values"][0]["v"] = true;

  HTTPClient http;
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + HOMEY_TOKEN);
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { Serial.printf("[insights] HTTP %d\n", code); http.end(); return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[insights] JSON: %s\n", err.c_str()); return false; }

  uint32_t now = time(nullptr) / SLOT_SEC;
  int n = 0;
  for (JsonObject e : doc["values"].as<JsonArray>()) {
    if (e["v"].isNull()) continue;
    uint32_t slot = parseIsoUtc(e["t"]) / SLOT_SEC;
    if (slot == 0 || now - slot >= N_SLOTS) continue;
    putSlot(slot, e["v"].as<float>());
    n++;
  }
  Serial.printf("[insights] %d punkter hentet\n", n);
  if (n > 0) { saveHistory(); return true; }
  return false;
}

// ---------------------------------------------------------------------
//  Tegning
// ---------------------------------------------------------------------
static void drawHeader(bool stale, bool haveHL, float hi, float lo) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(W, HDR_H)) return;
  s.fillSprite(rgb565(C_BG));
  s.loadFont(FONT_SMALL);
  s.setTextDatum(textdatum_t::baseline_left);
  const int base = HDR_BASE - HDR_Y;

  s.setTextColor(rgb565(C_GRAY));
  s.drawString(LABEL, M, base);

  if (stale) {  // diskret oransje prikk etter etiketten
    float cx = M + s.textWidth(LABEL) + 8.5f, cy = base - 5.5f;
    for (int y = (int)cy - 5; y <= (int)cy + 5; y++)
      for (int x = (int)cx - 5; x <= (int)cx + 5; x++) {
        float d = hypotf(x + 0.5f - cx, y + 0.5f - cy);
        blendPx(s, x, y, C_ORANGE, 3.5f - d);
      }
  }

  if (haveHL) {
    String hl = "H " + fmtTemp(hi) + "°   L " + fmtTemp(lo) + "°";
    s.drawString(hl, W - M - s.textWidth(hl), base);
  }
  s.unloadFont();
  s.pushSprite(0, HDR_Y);
  s.deleteSprite();
}

static void drawNumber(float t) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(W, NUM_H)) { Serial.println("[gfx] lite minne (tall)"); return; }
  s.fillSprite(rgb565(C_BG));
  s.loadFont(FONT_BIG);
  s.setTextDatum(textdatum_t::baseline_left);
  const int base = BIG_BASE - NUM_Y;

  // Én tekst, én størrelse: "9,8°C" / "−12,3°C"
  String txt;
  if (isnan(t)) {                       // ingen data ennå
    txt = "−";
    s.setTextColor(rgb565(C_GRAY_DIM));
  } else {
    long n10 = lroundf(t * 10.0f);
    txt = String(n10 < 0 ? "−" : "") + String(labs(n10) / 10) + "," +
          String(labs(n10) % 10) + "°C";
    s.setTextColor(rgb565(C_NUMBER));
  }
  s.drawString(txt, (W - s.textWidth(txt)) / 2, base);

  s.unloadFont();
  s.pushSprite(0, NUM_Y);
  s.deleteSprite();
}

static void drawChart(uint32_t nowSlot) {
  LGFX_Sprite s(&tft);
  s.setColorDepth(16);
  if (!s.createSprite(W, CH_H)) { Serial.println("[gfx] lite minne (graf)"); return; }
  s.fillSprite(rgb565(C_BG));

  // Samle gyldige punkter i tidsrekkefølge (eldste -> nå)
  int   idx[N_SLOTS];
  float val[N_SLOTS];
  int   n = 0;
  for (int k = N_SLOTS - 1; k >= 0; k--) {
    uint32_t slot = nowSlot - k;
    int i = slot % N_SLOTS;
    if (hist.slot[i] == slot) { idx[n] = N_SLOTS - 1 - k; val[n] = hist.t10[i] / 10.0f; n++; }
  }

  // Akse-tekst
  s.loadFont(FONT_TINY);
  s.setTextDatum(textdatum_t::baseline_left);
  s.setTextColor(rgb565(C_GRAY_DIM));
  const int lb = LBL_BASE - CH_Y;
  if (n < 2) {
    const char* msg = "Samler temperaturdata…";
    s.drawString(msg, (W - s.textWidth(msg)) / 2, (CH_TOP + CH_BOT) / 2 - CH_Y + 4);
  } else {
    s.drawString("24 t siden", M, lb);
    s.drawString("nå", W - M - s.textWidth("nå"), lb);
  }
  s.unloadFont();

  if (n >= 2) {
    // Myk utjevning (1-2-1)
    float sm[N_SLOTS];
    for (int k = 0; k < n; k++) {
      float a = val[k > 0 ? k - 1 : k], b = val[k < n - 1 ? k + 1 : k];
      sm[k] = (a + 2 * val[k] + b) / 4.0f;
    }
    float lo = sm[0], hi = sm[0];
    for (int k = 1; k < n; k++) { lo = fminf(lo, sm[k]); hi = fmaxf(hi, sm[k]); }
    if (hi - lo < 4.0f) { float mid = (hi + lo) / 2; lo = mid - 2; hi = mid + 2; }
    float pad = (hi - lo) * 0.08f; lo -= pad; hi += pad;

    const float x0 = M, x1 = W - M - 1;
    const float top = CH_TOP - CH_Y, bot = CH_BOT - CH_Y;
    float xs[N_SLOTS], ys[N_SLOTS];
    for (int k = 0; k < n; k++) {
      xs[k] = x0 + idx[k] * (x1 - x0) / (N_SLOTS - 1);
      ys[k] = top + (hi - sm[k]) / (hi - lo) * (bot - top);
    }
    auto at = [&](float x, float& y, float& v) {
      if (x <= xs[0])     { y = ys[0];     v = sm[0];     return; }
      if (x >= xs[n - 1]) { y = ys[n - 1]; v = sm[n - 1]; return; }
      for (int k = 0; k < n - 1; k++)
        if (x <= xs[k + 1]) {
          float t = (x - xs[k]) / (xs[k + 1] - xs[k]);
          y = ys[k] + (ys[k + 1] - ys[k]) * t;
          v = sm[k] + (sm[k + 1] - sm[k]) * t;
          return;
        }
    };

    // Stiplet nullinje hvis 0 °C er innenfor skalaen
    if (lo < 0 && hi > 0) {
      int zy = (int)lroundf(top + hi / (hi - lo) * (bot - top));
      for (int x = (int)x0; x <= (int)x1; x += 4) {
        blendPx(s, x, zy, C_GRAY_DIM, 1);
        blendPx(s, x + 1, zy, C_GRAY_DIM, 1);
      }
    }

    // Kurve (anti-aliasert, kolonnevis) med gradient-fyll under
    const float T = 2.0f;
    for (int x = (int)floorf(xs[0]); x <= (int)ceilf(xs[n - 1]); x++) {
      float ya, yb, yc, v, dummy;
      at(x - 0.5f, ya, dummy);
      at((float)x, yb, v);
      at(x + 0.5f, yc, dummy);
      uint32_t col = v >= 0 ? C_RED : C_BLUE;

      for (int y = (int)yb; y <= (int)bot; y++) {
        float f = (y + 0.5f - yb) / fmaxf(1.0f, bot - yb);
        f = fminf(1.0f, fmaxf(0.0f, f));
        float cov = fminf(1.0f, fmaxf(0.0f, y + 1 - yb));
        blendPx(s, x, y, col, 0.30f * powf(1.0f - f, 1.6f) * cov);
      }
      float y0 = fminf(ya, fminf(yb, yc)) - T / 2, y1 = fmaxf(ya, fmaxf(yb, yc)) + T / 2;
      for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++)
        blendPx(s, x, y, col, fminf(y + 1.0f, y1) - fmaxf((float)y, y0));
    }

    // Prikk for "nå" med svart ring
    float ex = xs[n - 1], ey = ys[n - 1];
    uint32_t col = sm[n - 1] >= 0 ? C_RED : C_BLUE;
    for (int y = (int)ey - 7; y <= (int)ey + 7; y++)
      for (int x = (int)ex - 7; x <= (int)ex + 7; x++) {
        float d = hypotf(x + 0.5f - ex, y + 0.5f - ey);
        blendPx(s, x, y, C_BG, 6.0f - d);
        blendPx(s, x, y, col, 4.0f - d);
      }
  }

  s.pushSprite(0, CH_Y);
  s.deleteSprite();
}

static void redraw() {
  bool stale = !everOk || (millis() - lastOkMs > STALE_MS);

  float hi = -1000, lo = 1000;
  bool haveHL = false;
  if (timeValid()) {
    uint32_t now = time(nullptr) / SLOT_SEC;
    for (int i = 0; i < N_SLOTS; i++)
      if (hist.slot[i] && now - hist.slot[i] < N_SLOTS) {
        float v = hist.t10[i] / 10.0f;
        hi = fmaxf(hi, v); lo = fminf(lo, v); haveHL = true;
      }
  }
  if (!isnan(curTemp)) { hi = fmaxf(hi, curTemp); lo = fminf(lo, curTemp); haveHL = true; }

  drawHeader(stale && everOk, haveHL, hi, lo);
  drawNumber(curTemp);
  drawChart(timeValid() ? time(nullptr) / SLOT_SEC : 0);
}

static void updateBacklight() {
  if (!timeValid()) { tft.setBrightness(BL_DAY); return; }
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  bool night = (NIGHT_FROM_H > NIGHT_TO_H)
                 ? (lt.tm_hour >= NIGHT_FROM_H || lt.tm_hour < NIGHT_TO_H)
                 : (lt.tm_hour >= NIGHT_FROM_H && lt.tm_hour < NIGHT_TO_H);
  tft.setBrightness(night ? BL_NIGHT : BL_DAY);
}

// ---------------------------------------------------------------------
//  WiFi / tid
// ---------------------------------------------------------------------
static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  static bool started = false;
  if (!started) {
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
      Serial.printf("[wifi] frakoblet, grunn %d\n", info.wifi_sta_disconnected.reason);
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.mode(WIFI_STA);
    // Mesh-nett: velg sterkeste tilgangspunkt, ikke det første som svarer
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);  // billige devkits feiler ofte handshake på full effekt
    started = true;
  } else {
    WiFi.reconnect();
  }
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] %s  RSSI %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return;
  }
  Serial.printf("[wifi] ikke tilkoblet (status %d)\n", WiFi.status());
  WiFi.disconnect();
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++)
    if (WiFi.SSID(i) == WIFI_SSID)
      Serial.printf("[wifi]   ser %s  kanal %d  RSSI %d dBm\n", WIFI_SSID, WiFi.channel(i), WiFi.RSSI(i));
  Serial.printf("[wifi]   %d nettverk funnet\n", n);
  WiFi.scanDelete();
}

static void waitForTime(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (!timeValid() && millis() - t0 < timeoutMs) delay(200);
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);              // liggende 320x240
  tft.setBrightness(BL_DAY);
  tft.fillScreen(TFT_BLACK);

  prefs.begin("utetemp", false);
  loadHistory();

  redraw();                        // vis rammen med en gang

  connectWiFi();
  configTzTime(TZ_INFO, "no.pool.ntp.org", "pool.ntp.org");
  waitForTime(10000);

  if (timeValid() && validSlots(time(nullptr) / SLOT_SEC) < N_SLOTS / 2)
    prefillFromInsights();

  lastPollMs = millis() - POLL_MS; // hent med en gang
}

void loop() {
  if (millis() - lastPollMs >= POLL_MS) {
    lastPollMs = millis();
    connectWiFi();

    float t;
    if (WiFi.status() == WL_CONNECTED && fetchTemp(t)) {
      curTemp  = t;
      lastOkMs = millis();
      everOk   = true;
      recordSample(t);
      Serial.printf("[temp] %.1f\n", t);
    }
    updateBacklight();
    redraw();
    Serial.printf("[mem] fri heap %u, største blokk %u\n",
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
  delay(50);
}
