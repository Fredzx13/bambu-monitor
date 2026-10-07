// Bambu Monitor - Guition JC4827W543 (ESP32-S3, 480x272)
// Connexion au compte Bambu par email (+ code de vérification), configuration via
// une page web servie par l'écran lui-même (portail captif). Affichage température,
// avancement, temps restant. Veille automatique à la fin de l'impression.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include "config.h"

// ---------------- Matériel JC4827W543 ----------------
#define PIN_BL        1
#define PIN_TOUCH_SDA 8
#define PIN_TOUCH_SCL 4
#define PIN_TOUCH_INT 3
#define PIN_TOUCH_RST 38

Arduino_DataBus *bus   = new Arduino_ESP32QSPI(45 /*CS*/, 47 /*SCK*/, 21 /*D0*/, 48 /*D1*/, 40 /*D2*/, 39 /*D3*/);
Arduino_GFX     *panel = new Arduino_NV3041A(bus, GFX_NOT_DEFINED, 0, true);
Arduino_Canvas  *canvasObj = nullptr;   // double buffer en PSRAM (si disponible)
Arduino_GFX     *cv = nullptr;          // surface de dessin : canvas, ou écran direct en secours
static inline void flushFrame() { if (canvasObj) canvasObj->flush(); }

// ---------------- Couleurs (RGB888) ----------------
#define COL_BG     0x0B0F14
#define COL_PANEL  0x151B23
#define COL_TRACK  0x2A3441
#define COL_TEXT   0xE6EDF3
#define COL_MUTED  0x8B98A5
#define COL_GREEN  0x2ECC71
#define COL_CYAN   0x22D3EE
#define COL_ORANGE 0xFF9F43
#define COL_PINK   0xFF5C7A
#define COL_YELLOW 0xF5C542
#define COL_BLUE   0x4A90D9

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
static inline uint16_t C(uint32_t c) { return rgb565(c >> 16, (c >> 8) & 255, c & 255); }
static uint16_t mixc(uint32_t bg, uint32_t fg, float t) {
  float r = ((bg >> 16) & 255) * (1 - t) + ((fg >> 16) & 255) * t;
  float g = ((bg >> 8) & 255) * (1 - t) + ((fg >> 8) & 255) * t;
  float b = (bg & 255) * (1 - t) + (fg & 255) * t;
  return rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// ---------------- Données imprimante ----------------
struct PrinterData {
  String state = "UNKNOWN";
  int pct = 0;
  int remMin = -1;
  float nozzle = 0, nozzleT = 0, bed = 0, bedT = 0, chamber = NAN;
  int layer = 0, layerTotal = 0;
  String job = "";
} P;

String printerName = "Bambu Lab";
String gUid = "";
String gSerial = "";

volatile bool dirty = true;
bool haveData = false;
unsigned long lastMsg = 0;

// ---------------- Configuration enregistrée (NVS) ----------------
Preferences prefs;
String cfgSsid, cfgWpass, cfgEmail, cfgPass, cfgToken;
bool cfgChina = false;

String apiBase = "https://api.bambulab.com";
String mqttHost = "us.mqtt.bambulab.com";

void applyRegion() {
  if (cfgChina) { apiBase = "https://api.bambulab.cn";  mqttHost = "cn.mqtt.bambulab.com"; }
  else          { apiBase = "https://api.bambulab.com"; mqttHost = "us.mqtt.bambulab.com"; }
}
void loadPrefs() {
  prefs.begin("bbl", false);
  cfgSsid  = prefs.getString("ssid", "");
  cfgWpass = prefs.getString("wpass", "");
  cfgEmail = prefs.getString("email", "");
  cfgPass  = prefs.getString("pass", "");
  cfgToken = prefs.getString("token", "");
  cfgChina = prefs.getBool("china", false);
  applyRegion();
}
void savePrefs() {
  prefs.putString("ssid", cfgSsid);
  prefs.putString("wpass", cfgWpass);
  prefs.putString("email", cfgEmail);
  prefs.putString("pass", cfgPass);
  prefs.putString("token", cfgToken);
  prefs.putBool("china", cfgChina);
}
void clearToken() { cfgToken = ""; prefs.remove("token"); }

WiFiClientSecure tlsMqtt;
PubSubClient mqtt(tlsMqtt);
JsonDocument gFilter;
bool accountOk = false;
String lastError = "";
int lastHttp = 0;

// ---------------- Veille / tactile ----------------
bool sleeping = false;
unsigned long lastActivity = 0;
uint8_t gt911Addr = 0x5D;

// ============================================================
//  Tactile GT911 (minimal : réveil de l'écran / entrée en configuration)
// ============================================================
bool probe(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }

void touchInit() {
  pinMode(PIN_TOUCH_INT, OUTPUT);
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_INT, LOW);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(11);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(6);
  pinMode(PIN_TOUCH_INT, INPUT);
  delay(60);
  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  Wire.setClock(400000);
  if (probe(0x5D)) gt911Addr = 0x5D;
  else if (probe(0x14)) gt911Addr = 0x14;
}

bool touchPressed() {
  Wire.beginTransmission(gt911Addr);
  Wire.write(0x81); Wire.write(0x4E);
  if (Wire.endTransmission() != 0) return false;
  Wire.requestFrom(gt911Addr, (uint8_t)1);
  if (!Wire.available()) return false;
  uint8_t s = Wire.read();
  bool touched = (s & 0x80) && (s & 0x0F) > 0;
  if (s & 0x80) {
    Wire.beginTransmission(gt911Addr);
    Wire.write(0x81); Wire.write(0x4E); Wire.write(0x00);
    Wire.endTransmission();
  }
  return touched;
}

// ============================================================
//  Dessin
// ============================================================
#define F_B24 u8g2_font_helvB24_tf
#define F_B18 u8g2_font_helvB18_tf
#define F_B14 u8g2_font_helvB14_tf
#define F_B10 u8g2_font_helvB10_tf
#define F_R10 u8g2_font_helvR10_tf
#define F_R12 u8g2_font_helvR12_tf

int textW(const char *s) {
  int16_t x1, y1; uint16_t w, h;
  cv->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}
void textL(const char *s, int x, int y, const uint8_t *f, uint32_t col) {
  cv->setFont(f); cv->setTextColor(C(col)); cv->setCursor(x, y); cv->print(s);
}
void textC(const char *s, int cx, int y, const uint8_t *f, uint32_t col) {
  cv->setFont(f); cv->setTextColor(C(col)); cv->setCursor(cx - textW(s) / 2, y); cv->print(s);
}
void textR(const char *s, int xr, int y, const uint8_t *f, uint32_t col) {
  cv->setFont(f); cv->setTextColor(C(col)); cv->setCursor(xr - textW(s), y); cv->print(s);
}
String fitText(String s, int maxW, const uint8_t *f) {
  cv->setFont(f);
  if (textW(s.c_str()) <= maxW) return s;
  while (s.length() > 1) {
    int i = s.length() - 1;
    while (i > 0 && (((uint8_t)s[i]) & 0xC0) == 0x80) i--;  // ne pas couper un caractère UTF-8
    s.remove(i);
    if (textW((s + "...").c_str()) <= maxW) break;
  }
  return s + "...";
}

// Camembert en anneau (donut) antialiasé, départ à midi, sens horaire
void donut(int cx, int cy, float rO, float rI, float frac, uint32_t fg, uint32_t track, uint32_t bg) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  float lim = frac * TWO_PI;
  int R = (int)rO + 2;
  for (int y = -R; y <= R; y++) {
    for (int x = -R; x <= R; x++) {
      float d = sqrtf((float)(x * x + y * y));
      float cov = fminf(d - rI + 0.5f, rO - d + 0.5f);
      if (cov <= 0) continue;
      if (cov > 1) cov = 1;
      float a = atan2f((float)x, (float)-y);
      if (a < 0) a += TWO_PI;
      cv->drawPixel(cx + x, cy + y, mixc(bg, (a < lim) ? fg : track, cov));
    }
  }
}

String fmtDur(int m) {
  if (m < 0) return "--";
  if (m == 0) return "< 1 min";
  char b[24];
  if (m >= 1440) snprintf(b, sizeof(b), "%d j %d h", m / 1440, (m % 1440) / 60);
  else if (m >= 60) snprintf(b, sizeof(b), "%d h %02d min", m / 60, m % 60);
  else snprintf(b, sizeof(b), "%d min", m);
  return String(b);
}

bool isPrinting() { return P.state == "RUNNING" || P.state == "PREPARE" || P.state == "PAUSE"; }
bool isOnline() { return haveData && (millis() - lastMsg < 120000UL); }

void stateStyle(const String &s, const char *&label, uint32_t &col) {
  if (s == "RUNNING")      { label = "Impression"; col = COL_CYAN; }
  else if (s == "PREPARE") { label = "Préparation"; col = COL_YELLOW; }
  else if (s == "PAUSE")   { label = "En pause"; col = COL_YELLOW; }
  else if (s == "FINISH")  { label = "Terminé"; col = COL_GREEN; }
  else if (s == "FAILED")  { label = "Échec"; col = COL_PINK; }
  else if (s == "IDLE")    { label = "Inactif"; col = COL_MUTED; }
  else                     { label = "..."; col = COL_MUTED; }
}

void tempDonut(int cx, int cy, const char *name, float t, float target, float maxScale, uint32_t hot) {
  uint32_t col; float frac;
  if (target > 0) { frac = t / target; col = hot; }
  else            { frac = t / maxScale; col = COL_BLUE; }
  donut(cx, cy, 38, 28, frac, col, COL_TRACK, COL_PANEL);
  char b[12]; snprintf(b, sizeof(b), "%d°", (int)roundf(t));
  textC(b, cx, cy + 6, F_B14, COL_TEXT);
  textC(name, cx, cy + 56, F_B10, col);
  char s[24];
  if (target > 0) snprintf(s, sizeof(s), "cible %d°", (int)roundf(target));
  else snprintf(s, sizeof(s), "repos");
  textC(s, cx, cy + 69, F_R10, COL_MUTED);
}

void render() {
  cv->fillScreen(C(COL_BG));
  bool online = isOnline();
  const char *lbl; uint32_t scol;
  stateStyle(P.state, lbl, scol);
  if (!online) { lbl = "Hors ligne"; scol = COL_MUTED; }

  // --- En-tête ---
  textL(fitText(printerName, 250, F_B14).c_str(), 12, 23, F_B14, COL_TEXT);
  cv->setFont(F_B10);
  int pw = textW(lbl) + 26;
  cv->fillRoundRect(468 - pw, 7, pw, 22, 11, C(scol));
  textC(lbl, 468 - pw / 2, 23, F_B10, COL_BG);
  cv->fillCircle(468 - pw - 14, 18, 5, C(mqtt.connected() ? COL_GREEN : COL_PINK));

  // --- Panneau gauche : avancement ---
  cv->fillRoundRect(8, 36, 224, 200, 14, C(COL_PANEL));
  int pct = constrain(P.pct, 0, 100);
  donut(120, 126, 76, 56, pct / 100.0f, scol == COL_MUTED ? COL_BLUE : scol, COL_TRACK, COL_PANEL);
  char b[32];
  snprintf(b, sizeof(b), "%d%%", pct);
  textC(b, 120, 137, F_B24, COL_TEXT);
  if (P.layerTotal > 0) snprintf(b, sizeof(b), "Couche %d / %d", P.layer, P.layerTotal);
  else snprintf(b, sizeof(b), "Avancement");
  textC(b, 120, 226, F_R12, COL_MUTED);

  // --- Panneau températures ---
  cv->fillRoundRect(240, 36, 232, 122, 14, C(COL_PANEL));
  tempDonut(298, 82, "Buse", P.nozzle, P.nozzleT, 300, COL_ORANGE);
  tempDonut(414, 82, "Plateau", P.bed, P.bedT, 120, COL_PINK);

  // --- Panneau temps restant ---
  cv->fillRoundRect(240, 164, 232, 72, 14, C(COL_PANEL));
  bool showRem = online && isPrinting() && P.remMin >= 0;
  textL("Temps restant", 254, 184, F_R10, COL_MUTED);
  textL(showRem ? fmtDur(P.remMin).c_str() : "--", 254, 214, F_B18, COL_TEXT);
  textR("Fin prévue", 460, 184, F_R10, COL_MUTED);
  String eta = "--:--";
  time_t now = time(nullptr);
  if (showRem && now > 1700000000) {
    time_t e = now + (time_t)P.remMin * 60;
    struct tm te, tn;
    localtime_r(&e, &te); localtime_r(&now, &tn);
    char t[16];
    snprintf(t, sizeof(t), "%02d:%02d", te.tm_hour, te.tm_min);
    eta = String(t);
    if (te.tm_yday != tn.tm_yday) eta += " +1j";
  }
  textR(eta.c_str(), 460, 214, F_B18, COL_TEXT);

  // --- Pied de page ---
  String job = P.job.length() ? P.job : String("Aucun travail");
  textL(fitText(job, 340, F_R10).c_str(), 12, 262, F_R10, COL_MUTED);
  if (!isnan(P.chamber)) {
    snprintf(b, sizeof(b), "Caisson %d°", (int)roundf(P.chamber));
    textR(b, 468, 262, F_R10, COL_MUTED);
  }
  flushFrame();
}

void statusScreen(const char *l1, const char *l2 = "", const char *l3 = "") {
  if (sleeping) return;
  cv->fillScreen(C(COL_BG));
  donut(240, 90, 34, 26, 0.72f, COL_CYAN, COL_TRACK, COL_BG);
  textC(l1, 240, 160, F_B14, COL_TEXT);
  textC(fitText(String(l2), 450, F_R12).c_str(), 240, 188, F_R12, COL_MUTED);
  textC(fitText(String(l3), 450, F_R12).c_str(), 240, 212, F_R12, COL_MUTED);
  flushFrame();
}

// ============================================================
//  Veille
// ============================================================
void enterSleep() {
  cv->fillScreen(0x0000);
  flushFrame();
  digitalWrite(PIN_BL, LOW);
  setCpuFrequencyMhz(80);
  sleeping = true;
  Serial.println("[sleep] écran éteint");
}
void wakeUp() {
  setCpuFrequencyMhz(240);
  sleeping = false;
  lastActivity = millis();
  render();
  digitalWrite(PIN_BL, HIGH);
  Serial.println("[sleep] réveil");
}

// ============================================================
//  API cloud Bambu (login email, UID, imprimante)
// ============================================================
static const char *UA = "bambu_network_agent/01.09.05.01";

int apiPost(const String &path, const String &body, String &resp) {
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;
  h.setTimeout(15000);
  resp = "";
  if (!h.begin(c, apiBase + path)) { lastHttp = 0; return -1; }
  h.addHeader("Content-Type", "application/json");
  h.addHeader("User-Agent", UA);
  int code = h.POST(body);
  if (code > 0) resp = h.getString();
  h.end();
  lastHttp = code;
  return code;
}

bool apiGet(const String &path, String &body) {
  WiFiClientSecure c;
  c.setInsecure();
  HTTPClient h;
  h.setTimeout(15000);
  if (!h.begin(c, apiBase + path)) { lastError = "API: begin"; return false; }
  h.addHeader("Authorization", String("Bearer ") + cfgToken);
  h.addHeader("User-Agent", UA);
  h.addHeader("Accept", "application/json");
  int code = h.GET();
  lastHttp = code;
  if (code != 200) {
    lastError = "API HTTP " + String(code);
    if (code == 401) { lastError += " (session expirée)"; clearToken(); }
    h.end();
    return false;
  }
  body = h.getString();
  h.end();
  return true;
}

// 0 = token obtenu, 1 = code email requis, -1 = erreur (lastError renseigné)
int bambuLogin(const String &email, const String &pass, const String &code, String &tok) {
  JsonDocument b;
  b["account"] = email;
  if (code.length()) b["code"] = code;
  else b["password"] = pass;
  String body, resp;
  serializeJson(b, body);
  int hc = apiPost("/v1/user-service/user/login", body, resp);
  if (hc <= 0) { lastError = "API Bambu injoignable"; return -1; }
  JsonDocument d;
  DeserializationError e = deserializeJson(d, resp);
  if (hc != 200) {
    String m = e ? String("") : String(d["message"] | "");
    lastError = "HTTP " + String(hc) + (hc == 403 ? " (bloqué)" : "") + (m.length() ? " : " + m : "");
    return -1;
  }
  if (e) { lastError = "Réponse invalide"; return -1; }
  String t = String(d["accessToken"] | "");
  if (t.length() > 20) { tok = t; return 0; }
  String lt = String(d["loginType"] | "");
  if (lt == "verifyCode") return 1;
  if (String(d["tfaKey"] | "").length()) { lastError = "Double authentification par appli non supportée"; return -1; }
  String m = String(d["message"] | "");
  lastError = m.length() ? m : "Identifiants refusés";
  return -1;
}

bool bambuSendCode(const String &email) {
  JsonDocument b;
  b["email"] = email;
  b["type"] = "codeLogin";
  String body, resp;
  serializeJson(b, body);
  int hc = apiPost("/v1/user-service/user/sendemail/code", body, resp);
  if (hc != 200) { lastError = "Envoi du code impossible (HTTP " + String(hc) + ")"; return false; }
  return true;
}

bool resolveAccount() {
  String body;
  if (gUid.length() == 0) {
    if (!apiGet("/v1/design-user-service/my/preference", body)) return false;
    JsonDocument d;
    if (deserializeJson(d, body)) { lastError = "API: JSON uid"; return false; }
    unsigned long long u = d["uid"].as<unsigned long long>();
    if (u == 0) { lastError = "API: uid introuvable"; return false; }
    gUid = String(u);
  }
  if (gSerial.length() == 0) {
    if (!apiGet("/v1/iot-service/api/user/bind", body)) return false;
    JsonDocument f;
    f["devices"][0]["dev_id"] = true;
    f["devices"][0]["name"] = true;
    JsonDocument d;
    if (deserializeJson(d, body, DeserializationOption::Filter(f))) { lastError = "API: JSON devices"; return false; }
    JsonArray arr = d["devices"].as<JsonArray>();
    if (arr.size() == 0) { lastError = "Aucune imprimante liée au compte"; return false; }
    JsonObject dev = arr[0];
    gSerial = dev["dev_id"].as<String>();
    String n = dev["name"].as<String>();
    if (n.length()) printerName = n;
  }
  Serial.printf("[api] uid=%s serial=%s nom=%s\n", gUid.c_str(), gSerial.c_str(), printerName.c_str());
  return true;
}

// ============================================================
//  Portail de configuration (Wi-Fi + compte Bambu) : ne retourne jamais, redémarre à la fin
// ============================================================
WebServer web(80);
DNSServer dns;
String ssidOptions;
bool portalCodeStage = false;
bool portalDone = false;
unsigned long portalDoneAt = 0;

String esc(String s) {
  s.replace("&", "&amp;"); s.replace("\"", "&quot;"); s.replace("<", "&lt;"); s.replace(">", "&gt;");
  return s;
}

String pageShell(const String &body) {
  return String("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
                "<title>Bambu Monitor</title><style>body{font-family:system-ui,sans-serif;background:#0b0f14;color:#e6edf3;margin:0 auto;padding:16px;max-width:460px}"
                "h1{font-size:21px}label{display:block;margin-top:14px;font-size:14px;color:#8b98a5}"
                "input,select{width:100%;box-sizing:border-box;padding:12px;border-radius:8px;border:1px solid #2a3441;background:#151b23;color:#e6edf3;font-size:16px}"
                "button{margin-top:20px;width:100%;padding:14px;border:0;border-radius:8px;background:#22d3ee;color:#0b0f14;font-size:16px;font-weight:700}"
                ".err{background:#4a1d28;padding:10px;border-radius:8px;margin:12px 0}.ok{background:#12372a;padding:10px;border-radius:8px;margin:12px 0}"
                "a{color:#22d3ee}details{margin-top:16px;color:#8b98a5}p{color:#8b98a5;font-size:14px}</style></head><body>") +
         body + "</body></html>";
}

String formPage(const String &msg = "") {
  String h = "<h1>Bambu Monitor</h1>";
  if (msg.length()) h += "<div class=err>" + esc(msg) + "</div>";
  h += "<form method=post action=/save>";
  h += "<label>Réseau Wi-Fi (2,4 GHz)</label><input name=ssid list=nets value=\"" + esc(cfgSsid) + "\" required autocapitalize=none>";
  h += "<datalist id=nets>" + ssidOptions + "</datalist>";
  h += "<label>Mot de passe Wi-Fi</label><input name=wp type=password placeholder=\"" + String(cfgWpass.length() ? "inchangé" : "") + "\">";
  h += "<label>Email du compte Bambu Lab</label><input name=email type=email value=\"" + esc(cfgEmail) + "\" required autocapitalize=none>";
  h += "<label>Mot de passe Bambu Lab</label><input name=pw type=password placeholder=\"" + String(cfgPass.length() ? "inchangé" : "") + "\">";
  h += "<label>Région du compte</label><select name=region><option value=world" + String(cfgChina ? "" : " selected") +
       ">Monde / Europe</option><option value=cn" + String(cfgChina ? " selected" : "") + ">Chine</option></select>";
  h += "<details><summary>Avancé : token d'accès</summary><label>Token (seulement si la connexion par email est refusée)</label><input name=tk autocapitalize=none></details>";
  h += "<button>Enregistrer et se connecter</button>";
  h += "<p>Un code de vérification pourra être envoyé par email à l'étape suivante.</p></form>";
  return pageShell(h);
}

String codePage(const String &msg = "") {
  String h = "<h1>Code de vérification</h1>";
  if (msg.length()) h += "<div class=err>" + esc(msg) + "</div>";
  h += "<p>Bambu Lab a envoyé un code à <b>" + esc(cfgEmail) + "</b>. Saisis-le ci-dessous.</p>";
  h += "<form method=post action=/code><label>Code reçu par email</label><input name=code inputmode=numeric autocomplete=one-time-code required>";
  h += "<button>Valider</button></form><p><a href=/resend>Renvoyer le code</a> &middot; <a href=/restart>Modifier mes informations</a></p>";
  return pageShell(h);
}

String donePage() {
  return pageShell("<h1>C'est fait ✔</h1><div class=ok>Compte enregistré. L'écran redémarre et affiche ton imprimante dans quelques secondes. "
                   "Tu peux fermer cette page.</div>");
}

void portalFinish() {
  portalDone = true;
  portalDoneAt = millis() + 3000;
  web.send(200, "text/html; charset=utf-8", donePage());
}

void runPortal(bool startWithCode) {
  Serial.println("[portail] démarrage");
  portalDone = false;
  portalCodeStage = startWithCode;
  ssidOptions = "";
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_NAME);
  IPAddress ip = WiFi.softAPIP();
  dns.start(53, "*", ip);
  if (!startWithCode) {
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && i < 20; i++) ssidOptions += "<option value=\"" + esc(WiFi.SSID(i)) + "\">";
  }

  web.on("/", HTTP_GET, []() {
    web.send(200, "text/html; charset=utf-8", portalCodeStage ? codePage() : formPage());
  });

  web.on("/save", HTTP_POST, []() {
    String ssid = web.arg("ssid"), wp = web.arg("wp"), em = web.arg("email"), pw = web.arg("pw"), tk = web.arg("tk");
    ssid.trim(); em.trim(); tk.trim();
    if (ssid.isEmpty() || em.isEmpty()) { web.send(200, "text/html; charset=utf-8", formPage("Wi-Fi et email obligatoires.")); return; }
    cfgSsid = ssid;
    if (wp.length()) cfgWpass = wp;
    cfgEmail = em;
    if (pw.length()) cfgPass = pw;
    cfgChina = (web.arg("region") == "cn");
    applyRegion();

    WiFi.begin(cfgSsid.c_str(), cfgWpass.c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { dns.processNextRequest(); delay(100); }
    if (WiFi.status() != WL_CONNECTED) {
      web.send(200, "text/html; charset=utf-8", formPage("Connexion Wi-Fi impossible : vérifie le nom et le mot de passe."));
      return;
    }
    if (tk.length() > 20) { cfgToken = tk; savePrefs(); portalFinish(); return; }
    if (cfgPass.isEmpty()) { web.send(200, "text/html; charset=utf-8", formPage("Mot de passe Bambu Lab manquant.")); return; }

    String tok;
    int r = bambuLogin(cfgEmail, cfgPass, "", tok);
    if (r == 0) { cfgToken = tok; savePrefs(); portalFinish(); }
    else if (r == 1) {
      savePrefs();
      if (!bambuSendCode(cfgEmail)) { web.send(200, "text/html; charset=utf-8", formPage(lastError)); return; }
      portalCodeStage = true;
      web.send(200, "text/html; charset=utf-8", codePage());
    } else {
      web.send(200, "text/html; charset=utf-8", formPage("Connexion Bambu refusée : " + lastError));
    }
  });

  web.on("/code", HTTP_POST, []() {
    String code = web.arg("code");
    code.trim();
    String tok;
    int r = bambuLogin(cfgEmail, cfgPass, code, tok);
    if (r == 0) { cfgToken = tok; savePrefs(); portalFinish(); }
    else web.send(200, "text/html; charset=utf-8", codePage(r == 1 ? "Code non accepté." : lastError));
  });

  web.on("/resend", HTTP_GET, []() {
    bool ok = bambuSendCode(cfgEmail);
    web.send(200, "text/html; charset=utf-8", codePage(ok ? "" : lastError));
  });

  web.on("/restart", HTTP_GET, []() {
    portalCodeStage = false;
    web.sendHeader("Location", "/", true);
    web.send(302, "text/plain", "");
  });

  web.onNotFound([]() {  // portail captif : tout redirige vers la page de config
    web.sendHeader("Location", "http://192.168.4.1/", true);
    web.send(302, "text/plain", "");
  });

  web.begin();
  statusScreen("Configuration", "1. Connecte ton téléphone au Wi-Fi : " SETUP_AP_NAME, "2. Ouvre http://192.168.4.1 si la page ne s'affiche pas seule");

  unsigned long start = millis();
  while (true) {
    dns.processNextRequest();
    web.handleClient();
    if (portalDone && millis() > portalDoneAt) { delay(200); ESP.restart(); }
    if (millis() - start > 900000UL) ESP.restart();  // 15 min sans action : on retente la connexion normale
    delay(2);
  }
}

// ============================================================
//  MQTT
// ============================================================
void onMessage(char *topic, byte *payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len, DeserializationOption::Filter(gFilter))) return;
  JsonObject p = doc["print"];
  if (p.isNull()) return;
  lastMsg = millis();
  haveData = true;

  if (p["gcode_state"].is<const char *>()) P.state = p["gcode_state"].as<String>();
  if (p["mc_percent"].is<int>()) P.pct = p["mc_percent"];
  if (p["mc_remaining_time"].is<int>()) P.remMin = p["mc_remaining_time"];
  if (p["nozzle_temper"].is<float>()) P.nozzle = p["nozzle_temper"];
  if (p["nozzle_target_temper"].is<float>()) P.nozzleT = p["nozzle_target_temper"];
  if (p["bed_temper"].is<float>()) P.bed = p["bed_temper"];
  if (p["bed_target_temper"].is<float>()) P.bedT = p["bed_target_temper"];
  if (p["chamber_temper"].is<float>()) P.chamber = p["chamber_temper"];
  if (p["layer_num"].is<int>()) P.layer = p["layer_num"];
  if (p["total_layer_num"].is<int>()) P.layerTotal = p["total_layer_num"];
  if (p["subtask_name"].is<const char *>()) {
    String j = p["subtask_name"].as<String>();
    if (j.length()) P.job = j;
  } else if (P.job.length() == 0 && p["gcode_file"].is<const char *>()) {
    P.job = p["gcode_file"].as<String>();
  }
  dirty = true;
}

void requestPushAll() {
  String t = "device/" + gSerial + "/request";
  mqtt.publish(t.c_str(), "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}");
}

int authFails = 0;

bool connectMqtt() {
  String cid = "bblmon_" + String((uint32_t)esp_random(), HEX);
  String user = "u_" + gUid;
  mqtt.setServer(mqttHost.c_str(), 8883);
  Serial.println("[mqtt] connexion...");
  if (mqtt.connect(cid.c_str(), user.c_str(), cfgToken.c_str())) {
    String t = "device/" + gSerial + "/report";
    mqtt.subscribe(t.c_str());
    requestPushAll();
    lastError = "";
    authFails = 0;
    dirty = true;
    Serial.println("[mqtt] connecté");
    return true;
  }
  int rc = mqtt.state();
  lastError = "MQTT erreur " + String(rc);
  if (rc == 4 || rc == 5) {
    lastError += " (session refusée)";
    if (++authFails >= 2) {  // token périmé : nouvelle connexion automatique
      authFails = 0;
      clearToken();
      gUid = "";
      accountOk = false;
    }
  }
  Serial.println("[mqtt] " + lastError);
  return false;
}

// ============================================================
//  setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BL, OUTPUT);
  digitalWrite(PIN_BL, HIGH);   // rétroéclairage allumé tout de suite : on voit que le programme tourne
  delay(300);
  Serial.printf("[boot] PSRAM: %s (%u octets)\n", psramFound() ? "oui" : "NON", (unsigned)ESP.getPsramSize());

  bool panelBegun = false;
  if (psramFound()) {
    canvasObj = new Arduino_Canvas(480, 272, panel);
    panelBegun = true;                       // Arduino_Canvas::begin() initialise aussi l'écran
    if (canvasObj->begin()) cv = canvasObj;
    else { Serial.println("[boot] canvas impossible"); canvasObj = nullptr; }
  }
  if (!panelBegun) panel->begin();
  if (!cv) { cv = panel; Serial.println("[boot] mode sans double buffer (secours)"); }

  // Test visuel : écran rouge 0,5 s = l'écran fonctionne
  panel->fillScreen(0xF800);
  delay(500);
  panel->fillScreen(0x0000);

  cv->setUTF8Print(true);
  cv->fillScreen(0x0000);
  flushFrame();
  if (!canvasObj) statusScreen("Mode secours", "Mémoire PSRAM indisponible", "Affichage possible mais plus lent");
  delay(canvasObj ? 0 : 1500);

  touchInit();
  loadPrefs();

  JsonObject f = gFilter["print"].to<JsonObject>();
  const char *keys[] = {"gcode_state", "mc_percent", "mc_remaining_time", "nozzle_temper", "nozzle_target_temper",
                        "bed_temper", "bed_target_temper", "chamber_temper", "layer_num", "total_layer_num",
                        "subtask_name", "gcode_file"};
  for (auto k : keys) f[k] = true;

  // Toucher l'écran au démarrage = (re)configuration
  bool force = false;
  statusScreen("Bambu Monitor", "Touche l'écran maintenant pour reconfigurer");
  unsigned long t0 = millis();
  while (millis() - t0 < 2500) {
    if (touchPressed()) { force = true; break; }
    delay(30);
  }
  if (force || cfgSsid.isEmpty() || cfgEmail.isEmpty()) runPortal(false);

  statusScreen("Connexion Wi-Fi", cfgSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfgSsid.c_str(), cfgWpass.c_str());
  t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    if (millis() - t0 > 30000) runPortal(false);  // Wi-Fi introuvable : page de configuration
  }
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  tlsMqtt.setInsecure();
  mqtt.setBufferSize(40000);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);
  mqtt.setCallback(onMessage);

  lastActivity = millis();
}

void loop() {
  static unsigned long lastApiTry = 0, lastMqttTry = 0, lastRender = 0, lastTouch = 0, lastPush = 0;
  static bool prevOnline = false;
  static int loginFails = 0;
  unsigned long now = millis();

  // --- Wi-Fi ---
  if (WiFi.status() != WL_CONNECTED) {
    statusScreen("Wi-Fi perdu", "Reconnexion...");
    delay(500);
    return;
  }

  // --- Compte : connexion par email si besoin, puis UID + imprimante ---
  if (!accountOk) {
    if (lastApiTry == 0 || now - lastApiTry > 10000) {
      lastApiTry = now;
      if (cfgToken.isEmpty()) {
        statusScreen("Connexion au compte Bambu", cfgEmail.c_str());
        String tok;
        int r = bambuLogin(cfgEmail, cfgPass, "", tok);
        if (r == 0) {
          cfgToken = tok;
          prefs.putString("token", tok);
          loginFails = 0;
        } else if (r == 1) {                      // code email demandé
          bambuSendCode(cfgEmail);
          runPortal(true);
        } else {
          statusScreen("Connexion refusée", lastError.c_str());
          if (lastHttp > 0 && ++loginFails >= 3) runPortal(false);  // identifiants à corriger
        }
      }
      if (cfgToken.length()) {
        statusScreen("Connexion au cloud Bambu", cfgEmail.c_str());
        accountOk = resolveAccount();
        if (!accountOk) statusScreen("Erreur compte Bambu", lastError.c_str());
      }
    }
    delay(50);
    return;
  }

  // --- MQTT ---
  if (!mqtt.connected()) {
    if (lastMqttTry == 0 || now - lastMqttTry > 5000) {
      lastMqttTry = now;
      statusScreen("Connexion à l'imprimante", lastError.c_str());
      if (!connectMqtt()) statusScreen("Cloud MQTT indisponible", lastError.c_str());
    }
  } else {
    mqtt.loop();
    if (now - lastMsg > 30000 && now - lastPush > 120000) { lastPush = now; requestPushAll(); }
  }

  // --- Tactile : réveil ---
  if (now - lastTouch > 60) {
    lastTouch = now;
    if (touchPressed()) {
      lastActivity = now;
      if (sleeping) wakeUp();
    }
  }

  // --- Logique de veille ---
  bool online = isOnline();
  if (online != prevOnline) { prevOnline = online; dirty = true; }
  bool active = online && isPrinting();
  if (active) {
    lastActivity = now;
    if (sleeping) wakeUp();
  }
  if (!sleeping && !active && mqtt.connected() && (now - lastActivity > (unsigned long)SLEEP_AFTER_S * 1000UL)) {
    enterSleep();
  }

  // --- Affichage ---
  if (!sleeping && mqtt.connected() && (dirty || now - lastRender > 30000)) {
    dirty = false;
    lastRender = now;
    render();
  }

  delay(sleeping ? 40 : 10);
}
