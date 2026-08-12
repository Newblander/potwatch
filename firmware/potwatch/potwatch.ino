/**
 * PotWatch v4.0
 * ESP32-C3 Super Mini
 *
 * Clip-on pot thermometer: pick a preset, drop the probe in, walk away.
 * Detects boiling, counts down, alarms when the dish is ready.
 *
 * Project home: https://potwatch.net
 *
 * Copyright (C) 2026 Ilia Kuzmin
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Changelog:
 *   v3.2 Removed Bounce2 -> transparent SimpleButton (reliable on ESP32-C3).
 *   v4.0 Rebrand ClipEgg -> PotWatch. Non-blocking sound engine (device stays
 *        responsive during the alarm). Timer screen: dedicated input handler
 *        with chord window (fixes off-by-one) + hold-both-to-exit. Temperature
 *        polled during COUNTDOWN; optional dry-pot protection + dead-probe
 *        indication. Preset minutes single-sourced from PRESET_MINUTES[].
 *        NVS namespace kept as "clipegg" so saved calibration survives upgrade.
 */

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BMP280.h>
#include <Preferences.h>

#define FW_VERSION "v4.0"

#define PIN_DS18B20  3
#define PIN_SDA      4
#define PIN_SCL      5
#define PIN_BUZZER   6
#define PIN_BTN_OK   7
#define PIN_BTN_SET  10

#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_ADDR    0x3C
#define BUZZER_FREQ  3000

#define TEMP_POLL_MS       1000
#define DS18B20_CONV_MS     800
#define BOIL_CONFIRM_COUNT    5

#define DEFAULT_BOIL_TEMP   97.6f
#define DEFAULT_BOIL_PRES   1013.25f

// Dry-pot protection: if the probe reads well above boiling for several
// seconds (no water / pot boiled dry) during heat-up or countdown, raise the
// alarm instead of cooking a dead sensor. Set DRY_POT_PROTECT to 0 to disable.
#define DRY_POT_PROTECT       1
#define DRY_POT_TEMP     105.0f
#define DRY_POT_CONFIRM       4
#define SENSOR_DEAD_COUNT     5

#define DEBOUNCE_MS          15
#define HOLD_RESET_MS      3000
#define HOLD_CAL_SAVE_MS   2000
#define HOLD_PHASE1_MS      500
#define HOLD_PHASE2_MS      300
#define HOLD_PHASE3_MS      200
#define HOLD_P2_START      2000
#define HOLD_P3_START      5000

#define CHORD_WINDOW_MS      80   // two presses within this = a chord, not two taps
#define CHORD_EXIT_MS      1500   // hold both buttons this long = exit Timer

#define MENU_BENEDICT  0
#define MENU_SOFT      1
#define MENU_HARD      2
#define MENU_SOUP      3
#define MENU_TIMER     4
#define MENU_CALIBRATE 5
#define MENU_COUNT     6
#define MENU_VISIBLE   4

#define SOUP_MIN   15
#define SOUP_MAX   120
#define SOUP_STEP  15
#define TIMER_MIN  1
#define TIMER_MAX  360

// Alarm reasons (plain ints to avoid Arduino enum-prototype quirks)
#define ALARM_DONE 0
#define ALARM_DRY  1

// ─────────────────────────────────────────────────
// Simple transparent button debouncer — no library
// ─────────────────────────────────────────────────
class SimpleButton {
  uint8_t       _pin;
  bool          _raw;      // last raw digitalRead (true = pressed = LOW pin)
  bool          _state;    // confirmed debounced state
  unsigned long _changeMs; // when raw last changed
  bool          _fell;     // pressed event this update()
  bool          _rose;     // released event this update()
public:
  void begin(uint8_t pin) {
    _pin      = pin;
    pinMode(pin, INPUT_PULLUP);
    _raw      = (digitalRead(pin) == LOW);
    _state    = _raw;
    _changeMs = 0;
    _fell = _rose = false;
  }
  void update(unsigned long now) {
    _fell = _rose = false;
    bool r = (digitalRead(_pin) == LOW);
    if (r != _raw) { _raw = r; _changeMs = now; }   // edge: restart timer
    if (now - _changeMs >= DEBOUNCE_MS && r != _state) {
      _state = r;
      if (_state) _fell = true; else _rose = true;  // confirmed transition
    }
  }
  bool fell()   const { return _fell;  }  // just pressed
  bool rose()   const { return _rose;  }  // just released
  bool isDown() const { return _state; }  // currently held
};

// ─────────────────────────────────────────────────
OneWire           oneWire(PIN_DS18B20);
DallasTemperature tempSensor(&oneWire);
Adafruit_SSD1306  display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Adafruit_BMP280   bmp;
SimpleButton      btnSet, btnOk;
Preferences       prefs;

enum State {
  STATE_MENU, STATE_SOUP_TIME, STATE_CUSTOM_TIME,
  STATE_WAITING_BOIL, STATE_COUNTDOWN, STATE_ALARM, STATE_CALIBRATION
};
State appState = STATE_MENU;

int  menuIndex  = 0;
int  menuScroll = 0;

const char* PRESET_NAMES[MENU_COUNT] = {
  "Benedict", "Soft egg", "Hard egg", "Soup", "Timer", ""
};
// Single source of truth for the fixed egg presets' cook time (minutes).
// 0 = not a fixed-time preset (Soup/Timer/Calibrate handle their own).
const int   PRESET_MINUTES[MENU_COUNT] = { 1, 3, 5, 0, 0, 0 };

// Menu labels: egg rows generated from name + minutes so they can never
// disagree with PRESET_MINUTES; the rest are static.
char        menuLabelBuf[3][20];
const char* MENU_LABELS[MENU_COUNT];

int   activeMinutes   = 3;
int   activePresetIdx = MENU_SOFT;
int   soupMinutes     = 60;
int   timerMinutes    = 5;
float savedBoilBase   = DEFAULT_BOIL_TEMP;
float savedBoilPres   = DEFAULT_BOIL_PRES;
bool  hasBmp          = false;
float calBoilTemp     = DEFAULT_BOIL_TEMP;

float         currentTemp   = 0.0f;
int           boilConfirm   = 0;
int           dryConfirm    = 0;
int           consecutiveBad= 0;
unsigned long convRequestMs = 0;
bool          convPending   = false;
unsigned long countdownEnd  = 0;
int           alarmReason    = ALARM_DONE;

// SET hold state
unsigned long setHoldStart  = 0;
unsigned long setLastRepeat = 0;
bool          setActive     = false;
bool          setHoldFired  = false;

// OK hold state
unsigned long okHoldStart  = 0;
unsigned long okLastRepeat = 0;
bool          okActive     = false;
bool          okHoldFired  = false;

// Timer-screen (STATE_CUSTOM_TIME) dedicated input state
bool          ctSetPend    = false;
bool          ctOkPend     = false;
unsigned long ctSetPendMs  = 0;
unsigned long ctOkPendMs   = 0;
bool          ctChordActive= false;
bool          ctChordFired = false;
unsigned long ctChordStart = 0;

// ─────────────────────────────────────────────────
// Non-blocking sound sequencer (freq 0 = rest)
// ─────────────────────────────────────────────────
#define SEQ_GAP 30
const int* seqFreq   = nullptr;
const int* seqDur    = nullptr;
int        seqLen    = 0;
int        seqIdx    = 0;
unsigned long seqNoteStart = 0;
bool       seqPlaying = false;
bool       seqLoop    = false;

const int CONFIRM_F[] = { BUZZER_FREQ, 0, BUZZER_FREQ, 0, BUZZER_FREQ };
const int CONFIRM_D[] = { 140, 70, 140, 70, 140 };
const int CONFIRM_LEN = 5;

const int ALARM_F[] = { BUZZER_FREQ, 0, BUZZER_FREQ, 0, BUZZER_FREQ, 0 };
const int ALARM_D[] = { 180, 110, 180, 110, 180, 650 };
const int ALARM_LEN = 6;

const int RESET_F[] = { BUZZER_FREQ, 0, BUZZER_FREQ };
const int RESET_D[] = { 200, 80, 200 };
const int RESET_LEN = 3;

void soundStart(const int* f, const int* d, int len, bool loop) {
  seqFreq = f; seqDur = d; seqLen = len;
  seqIdx = 0; seqLoop = loop; seqPlaying = true;
  seqNoteStart = millis();
  if (f[0] > 0) tone(PIN_BUZZER, f[0], d[0]); else noTone(PIN_BUZZER);
}
void soundStop() { seqPlaying = false; noTone(PIN_BUZZER); }
void soundService(unsigned long now) {
  if (!seqPlaying) return;
  if (now - seqNoteStart < (unsigned long)seqDur[seqIdx] + SEQ_GAP) return;
  seqIdx++;
  if (seqIdx >= seqLen) {
    if (seqLoop) seqIdx = 0;
    else { seqPlaying = false; noTone(PIN_BUZZER); return; }
  }
  seqNoteStart = now;
  if (seqFreq[seqIdx] > 0) tone(PIN_BUZZER, seqFreq[seqIdx], seqDur[seqIdx]);
  else                     noTone(PIN_BUZZER);
}
void beepClick() { tone(PIN_BUZZER, BUZZER_FREQ, 25); }

// ─────────────────────────────────────────────────
float currentPressureHpa() {
  return hasBmp ? bmp.readPressure() / 100.0f : DEFAULT_BOIL_PRES;
}
float boilPoint() {
  return constrain(savedBoilBase + (currentPressureHpa() - savedBoilPres) * 0.03f, 85.0f, 102.0f);
}
bool sensorDead() { return consecutiveBad >= SENSOR_DEAD_COUNT; }

void buildMenuLabels() {
  for (int i = 0; i < 3; i++) {
    snprintf(menuLabelBuf[i], sizeof(menuLabelBuf[i]), "%-9s%d min",
             PRESET_NAMES[i], PRESET_MINUTES[i]);
    MENU_LABELS[i] = menuLabelBuf[i];
  }
  MENU_LABELS[MENU_SOUP]      = "Soup...";
  MENU_LABELS[MENU_TIMER]     = "Timer...";
  MENU_LABELS[MENU_CALIBRATE] = "Calibrate";
}

// ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_BUZZER, OUTPUT);
  noTone(PIN_BUZZER);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    Serial.println("OLED not found");
  display.clearDisplay();
  display.display();

  hasBmp = bmp.begin(0x76);
  if (!hasBmp) Serial.println("BMP280 not found");

  tempSensor.begin();
  tempSensor.setWaitForConversion(false);
  tempSensor.setResolution(12);
  Serial.printf("DS18B20: %d found\n", tempSensor.getDeviceCount());

  btnSet.begin(PIN_BTN_SET);
  btnOk.begin(PIN_BTN_OK);

  buildMenuLabels();
  loadPrefs();

  tone(PIN_BUZZER, BUZZER_FREQ, 80); delay(150);
  tone(PIN_BUZZER, BUZZER_FREQ, 80);

  Serial.printf("Boot OK PotWatch %s  menu=%d soup=%d timer=%d boilBase=%.1f\n",
                FW_VERSION, menuIndex, soupMinutes, timerMinutes, savedBoilBase);
}

// ─────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  btnSet.update(now);
  btnOk.update(now);
  if (appState == STATE_CUSTOM_TIME) {
    handleCustomTime(now);
  } else {
    handleSetButton(now);
    handleOkButton(now);
  }

  pollTemperature(now);
  soundService(now);

  if (appState == STATE_COUNTDOWN && now >= countdownEnd) {
    triggerAlarm(ALARM_DONE);
  }

  // Rate-limited display: buttons polled thousands of times between frames
  static unsigned long lastDraw = 0;
  if (now - lastDraw >= 50) {
    lastDraw = now;
    switch (appState) {
      case STATE_MENU:         drawMenu();         break;
      case STATE_SOUP_TIME:    drawSoupTime();     break;
      case STATE_CUSTOM_TIME:  drawCustomTime();   break;
      case STATE_CALIBRATION:  drawCalibration();  break;
      case STATE_WAITING_BOIL: drawWaitingBoil();  break;
      case STATE_COUNTDOWN:    drawCountdown(now); break;
      case STATE_ALARM:        drawAlarm(now);     break;
    }
  }
}

// ─────────────────────────────────────────────────
// SET BUTTON (all states except STATE_CUSTOM_TIME)
// ─────────────────────────────────────────────────
void handleSetButton(unsigned long now) {

  if (btnSet.fell()) {
    setHoldStart  = now;
    setLastRepeat = now;
    setActive     = true;
    setHoldFired  = false;
    // Calibration steps on release (tap vs hold-to-save); others step now.
    if (appState != STATE_CALIBRATION) onSetStep(1);
  }

  if (btnSet.isDown() && setActive && !setHoldFired) {
    unsigned long held = now - setHoldStart;

    if (appState == STATE_CALIBRATION && held >= HOLD_CAL_SAVE_MS) {
      setHoldFired = true;
      saveCalibration();
      return;
    }

    if (appState == STATE_SOUP_TIME) {
      unsigned long interval; int steps;
      if (held < HOLD_P2_START)      { interval = HOLD_PHASE1_MS; steps = 1; }
      else if (held < HOLD_P3_START) { interval = HOLD_PHASE2_MS; steps = 5; }
      else                           { interval = HOLD_PHASE3_MS; steps = 10; }
      if (now - setLastRepeat >= interval) {
        setLastRepeat = now;
        onSetStep(steps);
      }
    }
  }

  if (btnSet.rose()) {
    if (appState == STATE_CALIBRATION && !setHoldFired) onSetStep(1);
    setActive = false;
  }
}

void onSetStep(int steps) {
  switch (appState) {
    case STATE_MENU:
      menuIndex = (menuIndex + 1) % MENU_COUNT;
      if (menuIndex < menuScroll) menuScroll = menuIndex;
      if (menuIndex >= menuScroll + MENU_VISIBLE) menuScroll = menuIndex - MENU_VISIBLE + 1;
      beepClick();
      break;
    case STATE_SOUP_TIME: {
      int range = SOUP_MAX - SOUP_MIN + SOUP_STEP;
      soupMinutes = SOUP_MIN + ((soupMinutes - SOUP_MIN + SOUP_STEP) % range);
      beepClick();
      break;
    }
    case STATE_CUSTOM_TIME: {
      int range = TIMER_MAX - TIMER_MIN + 1;
      timerMinutes = TIMER_MIN + ((timerMinutes - TIMER_MIN + steps + range * 100) % range);
      beepClick();
      break;
    }
    case STATE_CALIBRATION:
      calBoilTemp = constrain(calBoilTemp + 0.1f, 85.0f, 102.0f);
      beepClick();
      break;
    default: break;
  }
}

// ─────────────────────────────────────────────────
// OK BUTTON (all states except STATE_CUSTOM_TIME)
// short press = action; hold 3s = full reset to menu
// ─────────────────────────────────────────────────
void handleOkButton(unsigned long now) {

  if (btnOk.fell()) {
    okHoldStart  = now;
    okActive     = true;
    okHoldFired  = false;
    beepClick();
    onOkPress();
  }

  if (btnOk.isDown() && okActive && !okHoldFired) {
    if (now - okHoldStart >= HOLD_RESET_MS) {
      okHoldFired = true;
      fullReset();
    }
  }

  if (btnOk.rose()) okActive = false;
}

void onOkPress() {
  switch (appState) {

    case STATE_MENU:
      switch (menuIndex) {
        case MENU_BENEDICT:
        case MENU_SOFT:
        case MENU_HARD:
          activeMinutes   = PRESET_MINUTES[menuIndex];
          activePresetIdx = menuIndex;
          savePrefs();
          startWaitingBoil();
          break;
        case MENU_SOUP:      appState = STATE_SOUP_TIME; break;
        case MENU_TIMER:     enterCustomTime();          break;
        case MENU_CALIBRATE: calBoilTemp = boilPoint(); appState = STATE_CALIBRATION; break;
      }
      break;

    case STATE_SOUP_TIME:
      activeMinutes = soupMinutes; activePresetIdx = MENU_SOUP;
      savePrefs(); startWaitingBoil();
      break;

    case STATE_WAITING_BOIL:
      soundStop();
      appState = STATE_MENU;
      Serial.println("WAITING_BOIL cancelled -> MENU");
      break;

    case STATE_COUNTDOWN:
      soundStop();
      appState = STATE_MENU;
      Serial.println("COUNTDOWN cancelled -> MENU");
      break;

    case STATE_CALIBRATION:
      calBoilTemp = constrain(calBoilTemp - 0.1f, 85.0f, 102.0f);
      break;

    case STATE_ALARM:
      soundStop();
      alarmReason = ALARM_DONE;
      loadPrefs();
      appState = STATE_MENU;
      break;

    default: break;
  }
}

// ─────────────────────────────────────────────────
// TIMER SCREEN input — dedicated handler
//   SET  tap = +1 min,  hold = accelerating +
//   OK   tap = -1 min,  hold = accelerating -
//   both tapped together   = start
//   both held >1.5s        = exit to menu
// The chord window defers a single-button step long enough to tell a
// deliberate chord from a lone tap, so START no longer lands off-by-one.
// ─────────────────────────────────────────────────
void enterCustomTime() {
  appState      = STATE_CUSTOM_TIME;
  setActive     = okActive = false;
  ctSetPend     = ctOkPend = false;
  ctChordActive = ctChordFired = false;
}

void handleCustomTime(unsigned long now) {
  bool s = btnSet.isDown();
  bool o = btnOk.isDown();

  // ---------- both down = chord ----------
  if (s && o) {
    ctSetPend = ctOkPend = false;              // it's a chord, not two taps
    if (!ctChordActive) { ctChordActive = true; ctChordFired = false; ctChordStart = now; }
    if (!ctChordFired && now - ctChordStart >= CHORD_EXIT_MS) {
      ctChordFired = true;                     // long chord -> exit
      setActive = okActive = false;
      appState  = STATE_MENU;
      beepClick();
      Serial.println("CUSTOM_TIME exit -> MENU");
    }
    return;
  }

  // ---------- chord released ----------
  if (ctChordActive) {
    ctChordActive = false;
    setActive = okActive = false;
    if (!ctChordFired) {                        // short chord -> start
      activeMinutes = timerMinutes; activePresetIdx = MENU_TIMER;
      savePrefs(); startWaitingBoil();
    }
    return;
  }

  // ---------- SET alone: +1 tap / accelerating hold ----------
  if (btnSet.fell()) { ctSetPend = true; ctSetPendMs = now; }
  if (ctSetPend && now - ctSetPendMs >= CHORD_WINDOW_MS) {   // survived window -> real press
    ctSetPend = false;
    setActive = true; setHoldStart = ctSetPendMs; setLastRepeat = now;
    onSetStep(1);
  }
  if (s && setActive) {
    unsigned long held = now - setHoldStart;
    unsigned long interval; int steps;
    if (held < HOLD_P2_START)      { interval = HOLD_PHASE1_MS; steps = 1; }
    else if (held < HOLD_P3_START) { interval = HOLD_PHASE2_MS; steps = 5; }
    else                           { interval = HOLD_PHASE3_MS; steps = 10; }
    if (now - setLastRepeat >= interval) { setLastRepeat = now; onSetStep(steps); }
  }
  if (btnSet.rose()) {
    if (ctSetPend) { ctSetPend = false; onSetStep(1); }   // quick tap shorter than window
    setActive = false;
  }

  // ---------- OK alone: -1 tap / accelerating hold ----------
  if (btnOk.fell()) { ctOkPend = true; ctOkPendMs = now; }
  if (ctOkPend && now - ctOkPendMs >= CHORD_WINDOW_MS) {
    ctOkPend = false;
    okActive = true; okHoldStart = ctOkPendMs; okLastRepeat = now;
    onSetStep(-1);
  }
  if (o && okActive) {
    unsigned long held = now - okHoldStart;
    unsigned long interval; int steps;
    if (held < HOLD_P2_START)      { interval = HOLD_PHASE1_MS; steps = 1; }
    else if (held < HOLD_P3_START) { interval = HOLD_PHASE2_MS; steps = 5; }
    else                           { interval = HOLD_PHASE3_MS; steps = 10; }
    if (now - okLastRepeat >= interval) { okLastRepeat = now; onSetStep(-steps); }
  }
  if (btnOk.rose()) {
    if (ctOkPend) { ctOkPend = false; onSetStep(-1); }
    okActive = false;
  }
}

// ─────────────────────────────────────────────────
void startWaitingBoil() {
  boilConfirm    = 0;
  dryConfirm     = 0;
  consecutiveBad = 0;
  convPending    = false;
  convRequestMs  = 0;          // trigger first DS18B20 read immediately
  appState       = STATE_WAITING_BOIL;
  tone(PIN_BUZZER, BUZZER_FREQ, 60);
  Serial.printf("-> WAITING_BOIL  %s  %d min  boilPt=%.2fC\n",
                PRESET_NAMES[activePresetIdx], activeMinutes, boilPoint());
}

void startCountdown() {
  appState     = STATE_COUNTDOWN;
  countdownEnd = millis() + (unsigned long)activeMinutes * 60UL * 1000UL;
  dryConfirm   = 0;
  soundStart(CONFIRM_F, CONFIRM_D, CONFIRM_LEN, false);
  Serial.println("Boil confirmed -> COUNTDOWN");
}

void triggerAlarm(int reason) {
  alarmReason = reason;
  appState    = STATE_ALARM;
  soundStart(ALARM_F, ALARM_D, ALARM_LEN, true);
  Serial.printf("-> ALARM (%s)\n", reason == ALARM_DRY ? "DRY POT" : "READY");
}

void saveCalibration() {
  savedBoilBase = calBoilTemp;
  savedBoilPres = currentPressureHpa();
  prefs.begin("clipegg", false);
  prefs.putFloat("boil_base", savedBoilBase);
  prefs.putFloat("boil_pres", savedBoilPres);
  prefs.end();
  appState = STATE_MENU;
  soundStart(CONFIRM_F, CONFIRM_D, CONFIRM_LEN, false);
  Serial.printf("Calibration saved: %.1fC @ %.1f hPa\n", savedBoilBase, savedBoilPres);
}

void fullReset() {
  soundStop();
  setActive = false;
  okActive  = false;
  loadPrefs();
  appState  = STATE_MENU;
  soundStart(RESET_F, RESET_D, RESET_LEN, false);
  Serial.println("Full reset -> MENU");
}

// ─────────────────────────────────────────────────
// TEMPERATURE — async, polled whenever the reading matters
// ─────────────────────────────────────────────────
void pollTemperature(unsigned long now) {
  bool needsTemp = (appState == STATE_WAITING_BOIL ||
                    appState == STATE_CALIBRATION   ||
                    appState == STATE_COUNTDOWN     ||
                    appState == STATE_ALARM);
  if (!needsTemp) {
    convPending   = false;
    convRequestMs = now - TEMP_POLL_MS;
    return;
  }

  if (!convPending && now - convRequestMs >= TEMP_POLL_MS) {
    tempSensor.requestTemperatures();
    convRequestMs = now;
    convPending   = true;
    return;
  }
  if (!convPending || now - convRequestMs < DS18B20_CONV_MS) return;

  convPending = false;
  float t = tempSensor.getTempCByIndex(0);

  // Classify the reading. 85.0 is the DS18B20 power-on reset value; -127 is
  // "disconnected"; both are garbage. A sustained 105-150C is a plausible
  // real reading meaning the water is gone, not noise.
  bool garbage = (t == DEVICE_DISCONNECTED_C || t == 85.0f || t < -10.0f || t > 150.0f);

  if (garbage) {
    if (consecutiveBad < 1000) consecutiveBad++;
    Serial.printf("DS18B20 bad: %.2f (badN=%d)\n", t, consecutiveBad);
    return;                       // keep last good currentTemp on screen
  }
  consecutiveBad = 0;
  currentTemp = t;

#if DRY_POT_PROTECT
  if (appState == STATE_WAITING_BOIL || appState == STATE_COUNTDOWN) {
    if (currentTemp >= DRY_POT_TEMP) {
      if (++dryConfirm >= DRY_POT_CONFIRM) {
        Serial.println("DRY POT detected");
        triggerAlarm(ALARM_DRY);
        return;
      }
    } else {
      dryConfirm = 0;
    }
  }
#endif

  float threshold = boilPoint() - 0.3f;
  Serial.printf("T=%.2f  boilPt=%.2f  thr=%.2f  confirm=%d\n",
                currentTemp, boilPoint(), threshold, boilConfirm);

  if (appState == STATE_WAITING_BOIL) {
    if (currentTemp >= threshold) {
      if (++boilConfirm >= BOIL_CONFIRM_COUNT) startCountdown();
    } else {
      boilConfirm = 0;
    }
  }

  // After the ready signal, pulling the probe out (temp drops) returns to menu.
  if (appState == STATE_ALARM && currentTemp < (boilPoint() - 5.0f)) {
    soundStop();
    alarmReason = ALARM_DONE;
    loadPrefs();
    appState = STATE_MENU;
    Serial.println("Probe removed -> MENU");
  }
}

// ─────────────────────────────────────────────────
// DISPLAY
// ─────────────────────────────────────────────────
void drawMenu() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(25, 0);
  display.print("PotWatch " FW_VERSION);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  for (int i = 0; i < MENU_VISIBLE; i++) {
    int idx = menuScroll + i;
    if (idx >= MENU_COUNT) break;
    int y = 12 + i * 13;
    if (idx == menuIndex) {
      display.fillRect(0, y-1, 128, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }
    display.setCursor(4, y);
    display.print(MENU_LABELS[idx]);
  }
  display.setTextColor(SSD1306_WHITE);
  if (menuScroll > 0)                         { display.setCursor(120,10); display.print("^"); }
  if (menuScroll + MENU_VISIBLE < MENU_COUNT) { display.setCursor(120,56); display.print("v"); }
  display.display();
}

void drawSoupTime() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(22, 0); display.print("Soup timer");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  char buf[10];
  int h = soupMinutes/60, m = soupMinutes%60;
  if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d h", h, m);
  else        snprintf(buf, sizeof(buf), "%d min",   soupMinutes);
  display.setTextSize(3);
  int16_t x1,y1; uint16_t w,ht;
  display.getTextBounds(buf,0,0,&x1,&y1,&w,&ht);
  display.setCursor((128-w)/2, 14); display.print(buf);

  display.setTextSize(1);
  display.setCursor(0,48); display.print("SET=+15min   OK=start");
  display.setCursor(0,56); display.print("holdOK=exit");
  display.display();
}

void drawCustomTime() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(30, 0); display.print("Set timer");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  char buf[10];
  int h = timerMinutes/60, m = timerMinutes%60;
  if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d", h, m);
  else        snprintf(buf, sizeof(buf), "%d min",  timerMinutes);
  display.setTextSize(3);
  int16_t x1,y1; uint16_t w,ht;
  display.getTextBounds(buf,0,0,&x1,&y1,&w,&ht);
  display.setCursor((128-w)/2, 14); display.print(buf);

  display.setTextSize(1);
  display.setCursor(0,48); display.print("SET +  OK -  hold=fast");
  display.setCursor(0,56); display.print("both=start hold both=exit");
  display.display();
}

void drawCalibration() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Title
  display.setTextSize(1);
  display.setCursor(28, 0); display.print("Calibration");
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  // Vertical divider splitting screen in half
  display.drawLine(63, 10, 63, 44, SSD1306_WHITE);

  // Column labels
  display.setTextSize(1);
  display.setCursor(16, 11); display.print("live");
  display.setCursor(76, 11); display.print("boil pt");

  int16_t x1, y1; uint16_t w, h;
  char buf[8];

  // Live temperature — left half, large number ("--" if probe is dead)
  if (sensorDead()) snprintf(buf, sizeof(buf), "--");
  else              snprintf(buf, sizeof(buf), "%.1f", currentTemp);
  display.setTextSize(2);
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((62 - (int)w) / 2, 21); display.print(buf);
  display.setTextSize(1); display.print("C");

  // Adjustable boil point — right half, large number
  snprintf(buf, sizeof(buf), "%.1f", calBoilTemp);
  display.setTextSize(2);
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(64 + (62 - (int)w) / 2, 21); display.print(buf);
  display.setTextSize(1); display.print("C");

  // Bottom separator + hints
  display.drawLine(0, 44, 127, 44, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 47); display.print("SET=+0.1    OK=-0.1");
  display.setCursor(0, 56); display.print("holdSET=save  holdOK=exit");
  display.display();
}

void drawWaitingBoil() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  char top[22];
  snprintf(top, sizeof(top), "Heat > %s", PRESET_NAMES[activePresetIdx]);
  display.setCursor(0, 0); display.print(top);
  display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

  if (sensorDead()) {
    display.setTextSize(2);
    display.setCursor(4, 20); display.print("CHECK");
    display.setCursor(4, 40); display.print("PROBE");
    display.display();
    return;
  }

  char buf[12];
  snprintf(buf, sizeof(buf), "%.1fC", currentTemp);
  display.setTextSize(2);
  display.setCursor(8, 12); display.print(buf);

  display.setTextSize(1);
  snprintf(buf, sizeof(buf), "Boil pt: %.1fC", boilPoint());
  display.setCursor(0, 34); display.print(buf);

  char tbuf[18];
  int th=activeMinutes/60, tm=activeMinutes%60;
  if (th > 0) snprintf(tbuf, sizeof(tbuf), "Timer: %d:%02d h", th, tm);
  else        snprintf(tbuf, sizeof(tbuf), "Timer: %d min",    activeMinutes);
  display.setCursor(0, 44); display.print(tbuf);

  float frac = constrain((currentTemp-20.0f)/(boilPoint()-20.0f), 0.0f, 1.0f);
  int barW = (int)(frac * 124);
  display.drawRect(2, 55, 124, 7, SSD1306_WHITE);
  display.fillRect(2, 55, barW,  7, SSD1306_WHITE);
  display.display();
}

void drawCountdown(unsigned long now) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  unsigned long remaining = (countdownEnd > now) ? (countdownEnd - now) : 0;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d:%02d", (int)(remaining/60000), (int)((remaining%60000)/1000));
  display.setTextSize(3);
  int16_t x1,y1; uint16_t w,h;
  display.getTextBounds(buf,0,0,&x1,&y1,&w,&h);
  display.setCursor((128-w)/2, 8); display.print(buf);

  unsigned long total = (unsigned long)activeMinutes * 60UL * 1000UL;
  float frac = constrain((float)(total-remaining)/(float)total, 0.0f, 1.0f);
  int barW = (int)(frac * 124);
  display.drawRect(2, 47, 124, 8, SSD1306_WHITE);
  display.fillRect(2, 47, barW,  8, SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(20, 58); display.print("Water at temp!");
  display.display();
}

void drawAlarm(unsigned long now) {
  display.clearDisplay();
  if ((now/400) % 2 == 0) {
    display.drawRect(0, 0, 128, 64, SSD1306_WHITE);
    display.drawRect(2, 2, 124, 60, SSD1306_WHITE);
    display.setTextSize(2); display.setTextColor(SSD1306_WHITE);
    const char* msg = (alarmReason == ALARM_DRY) ? "NO WATER" : "READY!";
    int16_t x1,y1; uint16_t w,h;
    display.getTextBounds(msg,0,0,&x1,&y1,&w,&h);
    display.setCursor((128-w)/2, 14); display.print(msg);
    display.setTextSize(1);
    display.setCursor(34, 46); display.print("Press OK");
  }
  display.display();
}

// ─────────────────────────────────────────────────
// NVS  (namespace kept "clipegg" so saved calibration survives the rebrand)
// ─────────────────────────────────────────────────
void savePrefs() {
  prefs.begin("clipegg", false);
  prefs.putInt("menu_idx",  menuIndex);
  prefs.putInt("soup_min",  soupMinutes);
  prefs.putInt("timer_min", timerMinutes);
  prefs.end();
}

void loadPrefs() {
  prefs.begin("clipegg", true);
  menuIndex     = prefs.getInt(  "menu_idx",  0);
  soupMinutes   = prefs.getInt(  "soup_min",  60);
  timerMinutes  = prefs.getInt(  "timer_min",  5);
  savedBoilBase = prefs.getFloat("boil_base", DEFAULT_BOIL_TEMP);
  savedBoilPres = prefs.getFloat("boil_pres", DEFAULT_BOIL_PRES);
  prefs.end();

  menuIndex     = constrain(menuIndex,     0,        MENU_COUNT-1);
  soupMinutes   = constrain(soupMinutes,   SOUP_MIN, SOUP_MAX);
  timerMinutes  = constrain(timerMinutes,  TIMER_MIN,TIMER_MAX);
  savedBoilBase = constrain(savedBoilBase, 85.0f,    102.0f);
  menuScroll = (menuIndex >= MENU_VISIBLE) ? menuIndex - MENU_VISIBLE + 1 : 0;
}
