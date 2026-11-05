/*
 * Study Box - phone lock-away timer for TinyScreen+  (Hall sensor only)
 *
 * Hardware:
 *   TinyScreen+ + Wireling Adapter TinyShield (stacked)
 *   Port 1: Audio Amplifier Wireling   (alarm tone driven from A0 / DAC)
 *   Port 3: Digital Hall Wireling AST1006-D (magnet on the box lid)
 *
 * Buttons:
 *   Upper-left  : +5 min   (SETUP)
 *   Lower-left  : -5 min   (SETUP)
 *   Upper-right : start / pause / resume
 *   Lower-right : stop (back to SETUP) / dismiss "time's up"
 *
 * !! SKELETON - items marked VERIFY must be checked against the TinyCircuits
 *    example sketches for your board/library versions. !!
 */

#include <Wire.h>
#include <SPI.h>
#include <TinyScreen.h>
#include <Wireling.h>

// ---------------------------------------------------------------- config
#define AMP_PORT            1                // Wireling Adapter port for amplifier
#define HALL_PORT           3                // Wireling Adapter port for hall sensor
#define AUDIO_PIN           A0               // A0 = SAMD21 DAC pin; confirmed working on your board even with amp on port 1
#define HALL_PIN            (A0 + HALL_PORT) // VERIFY: port N maps to A<N> on the adapter
#define HALL_CLOSED_LEVEL   LOW              // VERIFY: LOW = magnet detected = lid closed

#define LID_DEBOUNCE_MS     50               // ignore hall flicker shorter than this
#define MIN_SECONDS         15               // shortest session (handy for testing)
#define MIN_MINUTES         5                // next step up after MIN_SECONDS
#define MAX_MINUTES         180
#define STEP_MINUTES        5
#define DEFAULT_MINUTES     25

#define ALARM_FREQ_HZ       2700
#define ALARM_BEEP_MS       250              // beep on/off period
#define DIM_AFTER_MS        10000
#define BRIGHT              10
#define DIM                 1

// ---------------------------------------------------------------- globals
TinyScreen display = TinyScreen(TinyScreenPlus);

enum State { SETUP, WAIT_FOR_BOX, RUNNING, VIOLATION, PAUSED, DONE };
State state = SETUP;

long setSeconds = DEFAULT_MINUTES * 60L;   // chosen session length in seconds
long remainingMs = 0;
unsigned long lastTickMs = 0;

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

// ---------------------------------------------------------------- helpers
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
      drawText("Set study time", buf, "+/-  then START");
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

  // ---- countdown: runs ONLY in RUNNING (frozen while the box is open)
  if (state == RUNNING) {
    remainingMs -= (long)(now - lastTickMs);
    lastTickMs = now;
    if (remainingMs <= 0) {
      remainingMs = 0;
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
      if (upR) changeState(WAIT_FOR_BOX);
      break;

    case WAIT_FOR_BOX:
      if (loR) { changeState(SETUP); break; }
      if (lidClosed()) {
        remainingMs = setSeconds * 1000L;
        lastTickMs = now;
        changeState(RUNNING);
      }
      break;

    case RUNNING:
      if (upR) { changeState(PAUSED); break; }            // user pauses to use phone
      if (loR) { changeState(SETUP);  break; }            // user stops session
      if (!lidClosed()) {
        setAlarm(true);
        changeState(VIOLATION);
      }
      break;

    case VIOLATION:
      if (upR) { setAlarm(false); changeState(PAUSED); break; }
      if (loR) { setAlarm(false); changeState(SETUP);  break; }
      if (lidClosed()) {                                  // phone back, lid shut
        setAlarm(false);
        lastTickMs = now;
        changeState(RUNNING);
      }
      break;

    case PAUSED:
      if (loR) { changeState(SETUP); break; }
      if (upR && lidClosed()) {
        lastTickMs = now;
        changeState(RUNNING);
      }
      break;

    case DONE:
      if (upL || loL || upR || loR) {
        setAlarm(false);
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
