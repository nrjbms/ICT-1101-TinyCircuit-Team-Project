/*
 * Study Box - phone lock-away timer for TinyScreen+
 * Built on your working Hall-sensor version, with session logging added
 * and (optionally) Wi-Fi time + Telegram commands/summary.
 *
 * Hardware:
 *   TinyScreen+ + Wireling Adapter TinyShield (+ WiFi TinyShield when USE_WIFI_TELEGRAM is 1)
 *   Port 1: Audio Amplifier Wireling   (alarm tone driven from A0 / DAC)
 *   Port 2: Digital Hall Wireling AST1006-D (magnet on the box lid)
 *   Port 3: leave EMPTY (WiFi shield uses A3 as its reset line)
 *
 * Buttons:
 *   Upper-left  : +5 min   (SETUP)
 *   Lower-left  : -5 min   (SETUP)
 *   Upper-right : start / pause / resume
 *   Lower-right : stop early (sends a summary) / back
 *
 * Telegram (only checked while idle in SETUP):
 *   /start 25  -> 25 minutes      /start 30s -> 30 seconds
 *
 * !! SKELETON - items marked VERIFY must be checked against the TinyCircuits
 *    example sketches / library docs for your board/library versions. !!
 */

// ---- 1 = Wi-Fi time + Telegram; 0 = logging only (summary printed to Serial Monitor)
#define USE_WIFI_TELEGRAM   1

#include <Wire.h>
#include <SPI.h>
#include <TinyScreen.h>
#include <Wireling.h>
#if USE_WIFI_TELEGRAM
  #include <WiFi101.h>
  #include <ArduinoJson.h>              // UniversalTelegramBot needs ArduinoJson (v6.x) - VERIFY
  #include <UniversalTelegramBot.h>
  #include "secrets.h"                  // WIFI_SSID, WIFI_PASS, BOT_TOKEN, CHAT_ID
#endif

// ---------------------------------------------------------------- config
#define AMP_PORT            1                // Wireling Adapter port for amplifier
#define HALL_PORT           2                // Wireling Adapter port for hall sensor
#define AUDIO_PIN           A0               // A0 = SAMD21 DAC pin; confirmed working on your board even with amp on port 1
#define HALL_PIN            (A0 + HALL_PORT) // VERIFY: port N maps to A<N> on the adapter
#define HALL_CLOSED_LEVEL   LOW              // VERIFY: LOW = magnet detected = lid closed

#define LID_DEBOUNCE_MS     50               // ignore hall flicker shorter than this
#define MIN_SECONDS         15               // shortest session (handy for testing)
#define MIN_MINUTES         5                // next step up after MIN_SECONDS
#define MAX_MINUTES         180
#define STEP_MINUTES        5
#define DEFAULT_MINUTES     25

#define MIN_DISTRACTION_MS  2000             // lid openings shorter than this are NOT logged
#define TZ_OFFSET_HOURS     8                // local time = UTC + this (change if needed)
#define TELEGRAM_POLL_MS    3000
#define TIME_RESYNC_MS      600000UL         // re-read the clock every 10 min while idle

#define ALARM_FREQ_HZ       2700
#define ALARM_BEEP_MS       250              // beep on/off period
#define DIM_AFTER_MS        10000
#define BRIGHT              10
#define DIM                 1

// ---------------------------------------------------------------- globals
TinyScreen display = TinyScreen(TinyScreenPlus);

#if USE_WIFI_TELEGRAM
WiFiSSLClient wifiClient;
UniversalTelegramBot bot(BOT_TOKEN, wifiClient);
unsigned long lastPollMs = 0;
#endif

enum State { SETUP, WAIT_FOR_BOX, RUNNING, VIOLATION, PAUSED, DONE };
State state = SETUP;

long setSeconds = DEFAULT_MINUTES * 60L;   // chosen session length in seconds
long remainingMs = 0;
unsigned long lastTickMs = 0;

// session log
struct SessionStats {
  unsigned long startEpoch;      // 0 = time of day unknown
  unsigned long startMs, endMs;
  long          distractCount;
  unsigned long distractMs;
  long          pauseCount;
  unsigned long pauseMs;
  bool          early;
} stats;
unsigned long segStartMs = 0;    // start of the current pause or distraction

// clock (set from Wi-Fi when available)
unsigned long epochAtSync = 0, msAtSync = 0, lastSyncTryMs = 0;

// buttons
uint8_t prevButtons = 0;

// alarm
bool alarmOn = false;
bool toneLevel = false;
unsigned long lastToggleUs = 0;

// display
bool needRedraw = true;
long lastDrawnSec = -1;
unsigned long lastActivityMs = 0;
bool dimmed = false;

// ---------------------------------------------------------------- lid
bool lidState = false;                   // debounced: true = magnet detected = lid closed
bool lastRawLid = false;
unsigned long rawLidChangeMs = 0;

bool readLidRaw() {
  return digitalRead(HALL_PIN) == HALL_CLOSED_LEVEL;
}

void updateLid() {                       // call once per loop()
  bool raw = readLidRaw();
  if (raw != lastRawLid) { lastRawLid = raw; rawLidChangeMs = millis(); }
  if (raw != lidState && millis() - rawLidChangeMs >= LID_DEBOUNCE_MS) {
    lidState = raw;
    SerialUSB.println(lidState ? "Lid CLOSED (magnet detected)" : "Lid OPEN (no magnet)");
  }
}

bool lidClosed() { return lidState; }

// ---------------------------------------------------------------- alarm
void setAlarm(bool on) {
  alarmOn = on;
  if (!on) analogWrite(AUDIO_PIN, 0);
}

// call every loop() - generates a beeping square wave on the DAC
void alarmUpdate() {
  if (!alarmOn) return;
  bool beepWindow = ((millis() / ALARM_BEEP_MS) % 2) == 0;
  if (!beepWindow) { analogWrite(AUDIO_PIN, 0); return; }

  unsigned long nowUs = micros();
  if (nowUs - lastToggleUs >= (500000UL / ALARM_FREQ_HZ)) {
    lastToggleUs = nowUs;
    toneLevel = !toneLevel;
    analogWrite(AUDIO_PIN, toneLevel ? 1023 : 0);    // full-scale 10-bit DAC swing = max volume
  }
}

// ---------------------------------------------------------------- display
void wake() {
  lastActivityMs = millis();
  if (dimmed) { display.setBrightness(BRIGHT); dimmed = false; }
}

void changeState(State s) {
  state = s;
  needRedraw = true;
  lastDrawnSec = -1;
  wake();
}

void drawText(const char *l1, const char *l2, const char *l3) {
  display.clearScreen();
  display.setFont(thinPixel7_10ptFontInfo);
  display.fontColor(TS_8b_White, TS_8b_Black);
  display.setCursor(2, 4);  display.print(l1);
  display.setCursor(2, 24); display.print(l2);
  display.setCursor(2, 44); display.print(l3);
}

void fmtTime(long ms, char *out) {
  long s = (ms + 999) / 1000;           // round up so it shows 00:01 until truly done
  snprintf(out, 12, "%02ld:%02ld", s / 60, s % 60);
}

void redraw() {
  char buf[16], buf2[24];
  switch (state) {
    case SETUP:
      if (setSeconds < 60) snprintf(buf, sizeof(buf), "%ld sec", setSeconds);
      else                 snprintf(buf, sizeof(buf), "%ld min", setSeconds / 60);
#if USE_WIFI_TELEGRAM
      drawText("Set study time", buf, "TG: /start N");
#else
      drawText("Set study time", buf, "+/-  then START");
#endif
      break;
    case WAIT_FOR_BOX:
      drawText("Put phone in box", "and CLOSE the lid", "(Stop = cancel)");
      break;
    case RUNNING:
      fmtTime(remainingMs, buf);
      drawText("Studying...", buf, "Start = pause");
      break;
    case VIOLATION:
      fmtTime(remainingMs, buf);
      snprintf(buf2, sizeof(buf2), "Left: %s", buf);
      drawText("Close the box", "and continue", buf2);
      break;
    case PAUSED:
      fmtTime(remainingMs, buf);
      snprintf(buf2, sizeof(buf2), "Left: %s", buf);
      drawText("PAUSED", buf2, "Close box+Start");
      break;
    case DONE:
      drawText("Study session", "over", "Press any button");
      break;
  }
  needRedraw = false;
}

// ---------------------------------------------------------------- clock
unsigned long nowEpoch() {                 // UTC epoch seconds, 0 if never synced
  if (!epochAtSync) return 0;
  return epochAtSync + (millis() - msAtSync) / 1000;
}

#if USE_WIFI_TELEGRAM
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
  return WiFi.status() == WL_CONNECTED;
}

bool syncTime() {                          // reads UTC epoch from the WiFi module
  lastSyncTryMs = millis();
  unsigned long t = WiFi.getTime();        // VERIFY: needs a recent WINC1500 firmware
  if (t > 1600000000UL) { epochAtSync = t; msAtSync = millis(); return true; }
  return false;
}
#endif

// ---------------------------------------------------------------- session log
void startSession(unsigned long now) {
  stats.startEpoch = nowEpoch();
  stats.startMs = now;
  stats.endMs = now;
  stats.distractCount = 0;
  stats.distractMs = 0;
  stats.pauseCount = 0;
  stats.pauseMs = 0;
  stats.early = false;
}

void endDistraction(unsigned long now) {   // only logs openings of MIN_DISTRACTION_MS or more
  unsigned long dur = now - segStartMs;
  if (dur >= MIN_DISTRACTION_MS) { stats.distractCount++; stats.distractMs += dur; }
}

void endPause(unsigned long now) {
  stats.pauseCount++;
  stats.pauseMs += now - segStartMs;
}

void fmtClock(unsigned long epoch, char *out) {      // "HH:MM" local time
  if (!epoch) { strcpy(out, "--:--"); return; }
  unsigned long s = (epoch + TZ_OFFSET_HOURS * 3600UL) % 86400UL;
  snprintf(out, 8, "%02lu:%02lu", s / 3600, (s % 3600) / 60);
}

void fmtDur(unsigned long ms, char *out, size_t n) { // "1 h 5 min" / "5 min 3 s" / "12 s"
  unsigned long s = (ms + 500) / 1000;
  unsigned long h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
  if (h > 0)      snprintf(out, n, "%lu h %lu min", h, m);
  else if (m > 0) snprintf(out, n, "%lu min %lu s", m, sec);
  else            snprintf(out, n, "%lu s", sec);
}

void buildSummary(char *buf, size_t n) {
  unsigned long totalMs = stats.endMs - stats.startMs;
  unsigned long lost = stats.distractMs + stats.pauseMs;
  unsigned long effMs = totalMs > lost ? totalMs - lost : 0;   // effective = total - distractions - pauses
  unsigned long endEpoch = stats.startEpoch ? stats.startEpoch + totalMs / 1000 : 0;

  char c1[8], c2[8], dTot[24], dPause[24], dDist[24], dEff[24];
  fmtClock(stats.startEpoch, c1);
  fmtClock(endEpoch, c2);
  fmtDur(totalMs, dTot, sizeof(dTot));
  fmtDur(stats.pauseMs, dPause, sizeof(dPause));
  fmtDur(stats.distractMs, dDist, sizeof(dDist));
  fmtDur(effMs, dEff, sizeof(dEff));

  snprintf(buf, n,
    "Study session summary (%s)\n"
    "Studied from: %s to %s\n"
    "Total time: %s\n"
    "Pauses taken: %ld (%s)\n"
    "Total distractions: %ld\n"
    "Total distraction duration: %s\n"
    "Effective study time: %s",
    stats.early ? "stopped early" : "completed",
    c1, c2, dTot,
    stats.pauseCount, dPause,
    stats.distractCount, dDist, dEff);
}

void deliverSummary() {
  char msg[420];
  buildSummary(msg, sizeof(msg));
  SerialUSB.println(msg);
#if USE_WIFI_TELEGRAM
  drawText("Sending", "summary...", "");
  bool ok = ensureWiFi() && bot.sendMessage(CHAT_ID, String(msg), "");
  drawText(ok ? "Summary sent" : "Send failed", ok ? "to Telegram" : "(check WiFi)", "");
#else
  drawText("Summary", "printed to", "Serial Monitor");
#endif
  delay(2000);
}

void finishSession(bool early) {           // stop pressed: log, send, back to SETUP
  stats.early = early;
  stats.endMs = millis();
  deliverSummary();
  changeState(SETUP);
}

// ---------------------------------------------------------------- telegram commands
#if USE_WIFI_TELEGRAM
void handleTelegramMessage(const String &chatId, const String &text) {
  if (chatId != String(CHAT_ID)) {
    SerialUSB.print("Ignored message from chat "); SerialUSB.println(chatId);
    return;
  }
  String t = text; t.trim();
  if (t.startsWith("/start")) {
    String arg = t.substring(6); arg.trim();
    long v = arg.toInt();
    long sec = arg.endsWith("s") ? v : v * 60;
    if (v <= 0 || sec < MIN_SECONDS || sec > MAX_MINUTES * 60L) {
      bot.sendMessage(CHAT_ID, "Send /start 25 (minutes) or /start 30s (seconds). Range: 15 s to 180 min.", "");
      return;
    }
    setSeconds = sec;
    char reply[80];
    if (sec < 60) snprintf(reply, sizeof(reply), "Got it: %ld sec. Put your phone in the box and close the lid.", sec);
    else          snprintf(reply, sizeof(reply), "Got it: %ld min. Put your phone in the box and close the lid.", sec / 60);
    bot.sendMessage(CHAT_ID, String(reply), "");
    changeState(WAIT_FOR_BOX);
  }
}

void pollTelegram() {                      // blocking (a second or two) - only call while idle
  if (millis() - lastPollMs < TELEGRAM_POLL_MS) return;
  lastPollMs = millis();
  if (!ensureWiFi()) return;
  int n = bot.getUpdates(bot.last_message_received + 1);
  while (n > 0) {
    for (int i = 0; i < n; i++) {
      handleTelegramMessage(bot.messages[i].chat_id, bot.messages[i].text);
      if (state != SETUP) return;          // command accepted
    }
    n = bot.getUpdates(bot.last_message_received + 1);
  }
}
#endif

// ---------------------------------------------------------------- setup
void setup() {
  SerialUSB.begin(115200);               // debug only; do not wait for a PC

  Wire.begin();
  Wireling.begin();                      // power the Wireling ports
  Wireling.selectPort(AMP_PORT);
  Wireling.selectPort(HALL_PORT);

  pinMode(HALL_PIN, INPUT_PULLUP);       // hall switch output is open-drain: needs a pull-up
  lidState = lastRawLid = readLidRaw();
  rawLidChangeMs = millis();
  analogWriteResolution(10);
  analogWrite(AUDIO_PIN, 0);

  display.begin();
  display.setBrightness(BRIGHT);
  display.setFlip(false);

#if USE_WIFI_TELEGRAM
  WiFi.setPins(8, 2, A3, -1);            // VERIFY: from TinyCircuits WiFi tutorial
  drawText("Connecting", "to WiFi...", "");
  if (ensureWiFi()) {
    for (int i = 0; i < 10 && !syncTime(); i++) delay(500);
  }
#endif

  lastActivityMs = millis();
  changeState(SETUP);
}

// ---------------------------------------------------------------- loop
void loop() {
  unsigned long now = millis();
  updateLid();

  // ---- buttons (edge detection)
  uint8_t b = display.getButtons();
  uint8_t pressed = b & ~prevButtons;
  prevButtons = b;
  bool upL = pressed & TSButtonUpperLeft;
  bool loL = pressed & TSButtonLowerLeft;
  bool upR = pressed & TSButtonUpperRight;
  bool loR = pressed & TSButtonLowerRight;
  if (pressed) wake();

  // ---- countdown: runs ONLY in RUNNING (frozen while the box is open or paused)
  if (state == RUNNING) {
    remainingMs -= (long)(now - lastTickMs);
    lastTickMs = now;
    if (remainingMs <= 0) {
      remainingMs = 0;
      stats.early = false;
      stats.endMs = now;
      setAlarm(true);
      changeState(DONE);
    }
  }

  // ---- state machine
  switch (state) {

    case SETUP:
      // 15 sec -> 5 min -> 10 min -> ... -> 180 min (and back down)
      if (upL) {
        if (setSeconds < MIN_MINUTES * 60L)          setSeconds = MIN_MINUTES * 60L;
        else if (setSeconds < MAX_MINUTES * 60L)     setSeconds += STEP_MINUTES * 60L;
        needRedraw = true;
      }
      if (loL) {
        if (setSeconds <= MIN_MINUTES * 60L)         setSeconds = MIN_SECONDS;
        else                                         setSeconds -= STEP_MINUTES * 60L;
        needRedraw = true;
      }
      if (upR) { changeState(WAIT_FOR_BOX); break; }
#if USE_WIFI_TELEGRAM
      if (now - lastSyncTryMs > TIME_RESYNC_MS && WiFi.status() == WL_CONNECTED) syncTime();
      pollTelegram();                                // /start <n> from Telegram
#endif
      break;

    case WAIT_FOR_BOX:
      if (loR) { changeState(SETUP); break; }
      if (lidClosed()) {
        remainingMs = setSeconds * 1000L;
        lastTickMs = now;
        startSession(now);
        changeState(RUNNING);
      }
      break;

    case RUNNING:
      if (upR) { segStartMs = now; changeState(PAUSED); break; }     // user pause (alarm stays silent)
      if (loR) { finishSession(true); break; }                       // stop early -> summary
      if (!lidClosed()) {                                            // distraction begins
        segStartMs = now;
        setAlarm(true);
        changeState(VIOLATION);
      }
      break;

    case VIOLATION:
      if (upR) { endDistraction(now); setAlarm(false); segStartMs = now; changeState(PAUSED); break; }
      if (loR) { endDistraction(now); setAlarm(false); finishSession(true); break; }
      if (lidClosed()) {                                             // distraction ends
        endDistraction(now);
        setAlarm(false);
        lastTickMs = now;
        changeState(RUNNING);
      }
      break;

    case PAUSED:
      if (loR) { endPause(now); finishSession(true); break; }
      if (upR && lidClosed()) {
        endPause(now);
        lastTickMs = now;
        changeState(RUNNING);
      }
      break;

    case DONE:
      if (upL || loL || upR || loR) {
        setAlarm(false);
        deliverSummary();
        changeState(SETUP);
      }
      break;
  }

  // ---- display (only redraw on change or once per second while counting)
  if (state == RUNNING) {
    long sec = remainingMs / 1000;
    if (sec != lastDrawnSec) { lastDrawnSec = sec; needRedraw = true; }
  }
  if (needRedraw) redraw();

  // ---- dim screen when idle (not during alarms)
  if (!dimmed && !alarmOn && state != VIOLATION && (now - lastActivityMs > DIM_AFTER_MS)) {
    display.setBrightness(DIM);
    dimmed = true;
  }

  alarmUpdate();
}
