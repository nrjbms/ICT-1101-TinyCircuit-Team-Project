/*
  Study Box v2 - TinyScreen+ sketch
  Hall Wireling (lid magnet) + Audio Amp Wireling + WiFi TinyShield + Telegram (via bot.py)

  Boot self-test shows: hall baseline -> WiFi result -> IP address -> server result.
  While READY, the bottom line shows:  <last server reply> h<hall value> <C/O>
      C = lid closed (magnet seen), O = lid open

  Buttons
    READY   : upper-left = +5 min, lower-left = -5 min (lowest step is the 30 s test)
    RUNNING : any button = pause
    PAUSED  : any button = resume (lid must be closed)
    DONE    : any button = back to READY
*/

#include <SPI.h>
#include <Wire.h>
#include <WiFi101.h>
#include <TinyScreen.h>
#include <Wireling.h>

// ===================== USER SETTINGS =====================
const char*    WIFI_SSID   = "<TO BE ADDED>";
const char*    WIFI_PASS   = "<TO BE ADDED>";
const char*    SERVER_HOST = "<TO BE ADDED>";   // IPv4 of the PC running box.py
const uint16_t SERVER_PORT = 8080;

// !! COPY THESE FROM YOUR WORKING HALL/AMP SKETCH !!
#define HALL_PORT 2
#define AMP_PORT  1
#define HALL_PIN  (A0 + HALL_PORT)  // Hall sensor on Wireling Port 2 = A2
#define AMP_PIN   (A0 + AMP_PORT)   // Audio amplifier on Wireling Port 1 = A1

// !! Use the exact WiFi.setPins(...) line from the ASD2123 product page / your WiFi example !!
#define WIFI_SETPINS()  WiFi.setPins(8, 2, A3, -1)
#define DEBUG_FOOTER  1     // 1 = show "<reply> h<hall> C/O" on the READY screen
#define DEBUG_SERIAL  0     // 1 = also print hall values to SerialUSB
#define POLL_MS       3000UL
// =========================================================

TinyScreen display = TinyScreen(TinyScreenPlus);
IPAddress serverIP;

enum State { READY, RUNNING, PAUSED, ALARM, COMPLETE };
State state = READY;

// ---- timer ----
uint8_t  durIdx = 5;            // 0 = 30 s test, k = k*5 min (max 24 = 2 h)
uint32_t totalSecs = 0;
int32_t  remainingMs = 0;       // valid while PAUSED or ALARM
uint32_t endMs = 0;             // valid while RUNNING
uint16_t distractions = 0;
uint32_t totalDistractionMs = 0;  // accumulated time with the lid open during a running session
uint32_t distractionStartMs = 0;
bool distractionActive = false;
uint32_t sessionId = 0;

// ---- lid ----
bool lidClosed = false;         // debounced: LOW means magnet detected
bool lidRaw = false;
uint32_t lidChangeMs = 0;
bool armed = false;             // lid must be seen OPEN in READY before closing starts a session

// ---- misc ----
uint8_t  lastButtons = 0;
uint32_t lastKey = 0xFFFFFFFF;
uint32_t lastPoll = 0;
uint32_t lastWifiTry = 0;
String   lastReply = "-";

// ---- completion ----
bool     needSend = false;
uint8_t  sendTries = 0;
uint32_t nextSendMs = 0;
uint32_t jingleStart = 0;
bool     jingleActive = false;

// ===================== SOUND =====================
void soundOn(unsigned f) { tone(AMP_PIN, f); }
void soundOff()          { noTone(AMP_PIN); }

void updateAlarmSound() {                    // beep-beep while lid is open
  static int8_t last = -1;
  int8_t on = ((millis() / 250) % 2) == 0;
  if (on != last) { last = on; if (on) soundOn(2000); else soundOff(); }
}

void updateJingle() {                        // ~6 s happy tune
  static int16_t lastNote = -1;
  const unsigned notes[8] = {523, 659, 784, 1047, 0, 1047, 784, 659};
  uint32_t el = millis() - jingleStart;
  if (el > 6000) { soundOff(); jingleActive = false; lastNote = -1; return; }
  int16_t n = (el / 180) % 8;
  if (n != lastNote) {
    lastNote = n;
    if (notes[n]) soundOn(notes[n]); else soundOff();
  }
}

// ===================== HALL / LID =====================
bool readHall() {
  // AST1006-D digital Hall sensor: LOW when magnet is detected.
  return digitalRead(HALL_PIN) == LOW;
}

void updateLid() {
  bool raw = readHall();
#if DEBUG_SERIAL
  static uint32_t t = 0;
  if (millis() - t > 500) {
    t = millis();
    SerialUSB.print("Hall pin=");
    SerialUSB.print(digitalRead(HALL_PIN));
    SerialUSB.print(" lid=");
    SerialUSB.println(raw ? "CLOSED" : "OPEN");
  }
#endif
  if (raw != lidRaw) {
    lidRaw = raw;
    lidChangeMs = millis();
  }
  if (millis() - lidChangeMs > 150) {
    lidClosed = lidRaw;
  }
}

void initWirelings() {
  Wireling.begin();
  pinMode(HALL_PIN, INPUT);
  pinMode(AMP_PIN, OUTPUT);
}

// ===================== DISPLAY =====================
#define TIMER_Y 15
const uint8_t SEG[10] = {0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
                         0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111};

void clearArea(int x, int y, int w, int h) { display.drawRect(x, y, w, h, 1, TS_8b_Black); }

void drawText(const char* s, int y, uint8_t color) {
  clearArea(0, y, 96, 11);
  display.setFont(thinPixel7_10ptFontInfo);
  display.fontColor(color, TS_8b_Black);
  int w = display.getPrintWidth((char*)s);
  display.setCursor((96 - w) / 2, y);
  display.print(s);
}

void showMessage(const char* a, const char* b = "") {
  display.clearScreen();
  drawText(a, 22, TS_8b_White);
  if (b[0]) drawText(b, 38, TS_8b_Gray);
  lastKey = 0xFFFFFFFF;
}

// 7-segment digit: 12 wide, 35 tall, 3 thick
void drawDigit(int x, int y, int d, uint8_t c) {
  const int W = 12, H = 35, T = 3, M = 16;
  uint8_t s = SEG[d];
  if (s & 1)  display.drawRect(x,         y,         W, T,     1, c);  // a
  if (s & 2)  display.drawRect(x + W - T, y,         T, M + T, 1, c);  // b
  if (s & 4)  display.drawRect(x + W - T, y + M,     T, H - M, 1, c);  // c
  if (s & 8)  display.drawRect(x,         y + H - T, W, T,     1, c);  // d
  if (s & 16) display.drawRect(x,         y + M,     T, H - M, 1, c);  // e
  if (s & 32) display.drawRect(x,         y,         T, M + T, 1, c);  // f
  if (s & 64) display.drawRect(x,         y + M,     W, T,     1, c);  // g
}

void drawColon(int x, int y, uint8_t c) {
  display.drawRect(x + 1, y + 10, 3, 3, 1, c);
  display.drawRect(x + 1, y + 22, 3, 3, 1, c);
}

void drawTimer(uint32_t secs, uint8_t color) {
  char buf[12];
  unsigned long h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
  if (h > 0) snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", h, m, s);
  else       snprintf(buf, sizeof(buf), "%02lu:%02lu", m, s);

  int total = 0;
  for (char* p = buf; *p; p++) { total += (*p == ':') ? 4 : 12; if (*(p + 1)) total += 3; }
  int x = (96 - total) / 2;

  clearArea(0, TIMER_Y, 96, 36);
  for (char* p = buf; *p; p++) {
    if (*p == ':') { drawColon(x, TIMER_Y, color); x += 4; }
    else           { drawDigit(x, TIMER_Y, *p - '0', color); x += 12; }
    x += 3;
  }
}

uint32_t secsForIdx(uint8_t i) { return i == 0 ? 30 : (uint32_t)i * 300; }

int32_t remainingNow() { return (int32_t)(endMs - millis()); }

uint32_t displaySecs() {
  switch (state) {
    case READY:   return secsForIdx(durIdx);
    case PAUSED:
    case ALARM:   return (remainingMs > 0) ? (remainingMs + 999) / 1000 : 0;
    case RUNNING: {
      int32_t r = remainingNow();
      return r > 0 ? (r + 999) / 1000 : 0;
    }
    default:      return 0;
  }
}

void render() {
  uint32_t secs = displaySecs();
  uint32_t key = (secs << 8) | ((uint32_t)state << 4) | (lidClosed << 1) | armed;
  if (key == lastKey) return;
  lastKey = key;

  if (state == COMPLETE) {
    display.clearScreen();
    drawText("Congratulations!", 6, TS_8b_Green);
    drawText("Session complete", 22, TS_8b_White);
    char buf[24];
    snprintf(buf, sizeof(buf), "Distracted: %u", distractions);
    drawText(buf, 38, TS_8b_Yellow);
    drawText("Press any button", 53, TS_8b_Gray);
    return;
  }

  const char* top; const char* bottom = nullptr; uint8_t color;
  switch (state) {
    case READY:
      color = TS_8b_White;
#if DEBUG_FOOTER
      top = armed ? "CLOSE LID=START" : "OPEN LID FIRST";   // bottom line is owned by the footer
#else
      top = "READY";
      bottom = armed ? "Close lid to start" : "Open the lid first";
#endif
      break;
    case RUNNING:
      top = "STUDYING";         color = TS_8b_Green;
      bottom = "Button = pause";
      break;
    case PAUSED:
      top = "|| PAUSED";        color = TS_8b_Yellow;
      bottom = lidClosed ? "Press to resume" : "Close lid, press";
      break;
    default: // ALARM
      top = "LID OPEN!";        color = TS_8b_Red;
      bottom = "Close lid / pause";
      break;
  }
  drawText(top, 2, color);
  drawTimer(secs, color);
  if (bottom) drawText(bottom, 53, TS_8b_Gray);
}

void drawFooter() {                     // READY-screen diagnostics
  static uint32_t t = 0;
  if (millis() - t < 300) return;
  t = millis();
  char b[28];
  snprintf(b, sizeof(b), "%.8s Hall:%s", lastReply.c_str(), lidClosed ? "C" : "O");
  drawText(b, 53, TS_8b_Gray);
}

// ===================== NETWORK =====================
String httpGet(const String& path, uint16_t timeoutMs = 2500) {
  WiFiClient client;
  if (!client.connect(serverIP, SERVER_PORT)) return "";
  client.print(String("GET ") + path + " HTTP/1.0\r\nHost: " + SERVER_HOST + "\r\nConnection: close\r\n\r\n");
  String resp;
  uint32_t t = millis();
  while ((client.connected() || client.available()) && millis() - t < timeoutMs) {
    while (client.available()) resp += (char)client.read();
  }
  client.stop();
  int i = resp.indexOf("\r\n\r\n");
  return i >= 0 ? resp.substring(i + 4) : "";
}

bool connectWifi(bool force = false) {
  int st = WiFi.status();
  if (st == WL_CONNECTED) return true;
  if (st == WL_NO_SHIELD) return false;                                   // board not detected
  if (!force && lastWifiTry && millis() - lastWifiTry < 20000) return false;  // cool-down between tries
  lastWifiTry = millis();

  showMessage("WiFi...", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(250);

  if (WiFi.status() != WL_CONNECTED) {
    char buf[20];
    snprintf(buf, sizeof(buf), "status %d", WiFi.status());
    showMessage("WiFi failed", buf);
    delay(2500);
  }
  lastKey = 0xFFFFFFFF;
  return WiFi.status() == WL_CONNECTED;
}

void applySeconds(long s) {
  if (s == 30) durIdx = 0;
  else if (s >= 300 && s <= 7200 && s % 300 == 0) durIdx = s / 300;
}

void pollServer() {
  bool ok = connectWifi();
  String r = ok ? httpGet("/poll") : "";
  r.trim();
  lastReply = !ok ? "no wifi" : (r.length() ? r : "no reply");
  if (r.startsWith("SET ")) applySeconds(r.substring(4).toInt());
}

// Boot-time check: shows exactly which link is broken.
void bootSelfTest() {
  if (WiFi.status() == WL_NO_SHIELD) {
    showMessage("No WiFi board!", "check setPins");
    delay(5000);
    return;
  }
  if (!connectWifi(true)) return;               // already shows "WiFi failed / status N"

  IPAddress ip = WiFi.localIP();
  char buf[24];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  showMessage("WiFi OK", buf);
  delay(1500);

  showMessage("Asking server...", SERVER_HOST);
  String r = httpGet("/poll", 4000);
  r.trim();
  if (r.length() == 0) showMessage("Server FAIL", SERVER_HOST);
  else                 showMessage("Server OK", r.c_str());
  if (r.startsWith("SET ")) applySeconds(r.substring(4).toInt());
  delay(2000);
}

// ===================== SESSION FLOW =====================
void enterReady() {
  state = READY;
  armed = false;
  soundOff();
  lastKey = 0xFFFFFFFF;
}

void startSession() {
  totalSecs = secsForIdx(durIdx);
  distractions = 0;
  totalDistractionMs = 0;
  distractionStartMs = 0;
  distractionActive = false;
  sessionId = millis();
  connectWifi();
  httpGet(String("/event?type=start&secs=") + totalSecs + "&sid=" + sessionId);  // best effort
  endMs = millis() + totalSecs * 1000UL;
  state = RUNNING;
  lastKey = 0xFFFFFFFF;
}

void finishSession() {
  soundOff();
  state = COMPLETE;
  needSend = true;
  sendTries = 0;
  nextSendMs = 0;
  jingleStart = millis();
  jingleActive = true;
  lastKey = 0xFFFFFFFF;
}

void trySendComplete() {
  connectWifi();
  uint32_t lostMs = totalDistractionMs;
  if (distractionActive) lostMs += millis() - distractionStartMs;
  String r = httpGet(String("/event?type=complete&secs=") + totalSecs +
                     "&distractions=" + distractions + "&lost_ms=" + lostMs +
                     "&sid=" + sessionId, 3000);
  if (r.startsWith("OK") || ++sendTries >= 5) needSend = false;
  else nextSendMs = millis() + 2000;
}

// ===================== ARDUINO =====================
void setup() {
#if DEBUG_SERIAL
  SerialUSB.begin(115200);
#endif
  Wire.begin();
  display.begin();
  display.setBrightness(10);

  initWirelings();

  // Read the actual lid position before arming the timer.
  lidClosed = readHall();
  lidRaw = lidClosed;
  lidChangeMs = millis();
  armed = false;

  serverIP.fromString(SERVER_HOST);

  WIFI_SETPINS();          // must run before the first WiFi call
  bootSelfTest();

  enterReady();
}

void loop() {
  updateLid();

  // Stop the distraction timer when the lid closes. This is independent
  // of RUNNING/ALARM/PAUSED so a button can silence the alarm without
  // stopping the lost-time count while the lid is still open.
  if (distractionActive && lidClosed) {
    totalDistractionMs += millis() - distractionStartMs;
    distractionActive = false;
  }

  uint8_t b = display.getButtons();
  uint8_t pressed = b & ~lastButtons;
  lastButtons = b;

  switch (state) {
    case READY:
      if (!lidClosed) armed = true;
      if ((pressed & TSButtonUpperLeft) && durIdx < 24) durIdx++;
      if ((pressed & TSButtonLowerLeft) && durIdx > 0)  durIdx--;
      if (millis() - lastPoll > POLL_MS) { lastPoll = millis(); pollServer(); }
      if (armed && lidClosed) { startSession(); break; }
      render();
#if DEBUG_FOOTER
      drawFooter();
#endif
      break;

    case RUNNING: {
      int32_t rem = remainingNow();
      if (rem <= 0) { finishSession(); break; }

      if (pressed) {
        remainingMs = rem;
        state = PAUSED;
        lastKey = 0xFFFFFFFF;
      }
      else if (!lidClosed) {
        // Freeze the countdown and start measuring lost time.
        remainingMs = rem;
        distractions++;
        distractionStartMs = millis();
        distractionActive = true;
        state = ALARM;
        soundOff();
        lastKey = 0xFFFFFFFF;
      }

      render();
      break;
    }

    case ALARM: {
      // remainingMs is frozen while the lid is open; endMs is not used here.
      if (remainingMs <= 0) {
        finishSession();
        break;
      }

      if (pressed) {
        // A button press silences the alarm and leaves the timer paused.
        state = PAUSED;
        soundOff();
        lastKey = 0xFFFFFFFF;
      }
      else if (lidClosed) {
        // Resume the countdown from the exact remaining time.
        endMs = millis() + (uint32_t)remainingMs;
        state = RUNNING;
        soundOff();
        lastKey = 0xFFFFFFFF;
      }
      else {
        updateAlarmSound();
      }

      render();
      break;
    }

    case PAUSED:
      if (pressed && lidClosed) { endMs = millis() + remainingMs; state = RUNNING; lastKey = 0xFFFFFFFF; }
      render();
      break;

    case COMPLETE:
      if (jingleActive) updateJingle();
      if (needSend && millis() >= nextSendMs) trySendComplete();
      if (pressed && !needSend) { soundOff(); jingleActive = false; enterReady(); break; }
      render();
      break;
  }
  delay(10);
}
