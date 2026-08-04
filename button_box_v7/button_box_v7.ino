// =====================================================================
// SIM RACING BUTTON BOX — Raspberry Pi Pico 2
// Reads 3 rotary encoders, 3 toggle switches, 4 LED buttons.
// Sends everything to Windows as joystick button presses (buttons 1-19).
// Also listens for simple text commands over USB so SimHub can control
// the 4 button LEDs (on/off/flash) and the OLED screen text, based on
// in-game data.
// =====================================================================

#include <Joystick.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------------------------------------------------------------
// PIN DEFINITIONS — matches your final wiring map exactly
// ---------------------------------------------------------------------

// Encoder 1
const int ENC1_CLK = 0;
const int ENC1_DT  = 1;
const int ENC1_SW  = 2;
// Encoder 2
const int ENC2_CLK = 3;
const int ENC2_DT  = 4;
const int ENC2_SW  = 5;
// Encoder 3
const int ENC3_CLK = 6;
const int ENC3_DT  = 7;
const int ENC3_SW  = 8;

// Toggle switches (top pin, bottom pin — centre pin is just wired to GND)
const int TOG1_TOP = 10;
const int TOG1_BOT = 11;
const int TOG2_TOP = 12;
const int TOG2_BOT = 13;
const int TOG3_TOP = 14;
const int TOG3_BOT = 15;

// LED Buttons — switch signal pins
const int BTN1_SW = 18;
const int BTN2_SW = 19;
const int BTN3_SW = 20;
const int BTN4_SW = 21;

// LED Buttons — LED control pins (through your resistors)
const int BTN1_LED = 22;
const int BTN2_LED = 26;
const int BTN3_LED = 27;
const int BTN4_LED = 28;

// Idle "off" brightness — LEDs never go fully dark, they rest at this
// faint glow for night-time button visibility. All commands still work
// exactly as before; this is just what "off" now means, and what
// flash/fade dip down to instead of true black.
const int LED_DIM_FLOOR = 1;

// OLED screen — confirmed working at I2C address 0x3C
const int OLED_SDA = 16;
const int OLED_SCL = 17;
#define SCREEN_ADDRESS 0x3C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ---------------------------------------------------------------------
// JOYSTICK BUTTON NUMBER ASSIGNMENTS (what SimHub/games will see)
// ---------------------------------------------------------------------
// 1  = Encoder 1 turned "up"      (e.g. Traction Control +)
// 2  = Encoder 1 turned "down"    (e.g. Traction Control -)
// 3  = Encoder 1 push-click       (e.g. TC off / reset)
// 4  = Encoder 2 turned "up"      (e.g. ABS +)
// 5  = Encoder 2 turned "down"    (e.g. ABS -)
// 6  = Encoder 2 push-click
// 7  = Encoder 3 turned "up"      (e.g. Brake Bias +)
// 8  = Encoder 3 turned "down"    (e.g. Brake Bias -)
// 9  = Encoder 3 push-click
// 10 = Toggle 1 up
// 11 = Toggle 1 down
// 12 = Toggle 2 up
// 13 = Toggle 2 down
// 14 = Toggle 3 up
// 15 = Toggle 3 down
// 16 = Button 1
// 17 = Button 2
// 18 = Button 3
// 19 = Button 4

// ---------------------------------------------------------------------
// DEBOUNCE / TIMING SETTINGS
// ---------------------------------------------------------------------
const unsigned long DEBOUNCE_MS = 15;     // ignore switch noise shorter than this
const unsigned long ENCODER_PULSE_MS = 60; // how long an encoder "click" button stays pressed

// If SimHub stops sending anything for this long, we assume it has closed
// or disconnected, and blank the screen ourselves rather than leaving old
// data frozen on display.
const unsigned long SIMHUB_TIMEOUT_MS = 3000;
unsigned long lastCommandReceivedTime = 0;
bool screenIsBlank = true; // starts blank, matches setup()'s initial blank screen

// ---------------------------------------------------------------------
// STATE TRACKING — the code needs to remember the last reading of every
// input so it only reacts to CHANGES, not constant repeats.
// ---------------------------------------------------------------------

// Toggle + button switches: simple debounced digital reads
struct DebouncedInput {
  int pin;
  int joystickButton;
  bool lastStableState;
  bool lastRawState;
  unsigned long lastChangeTime;
};

DebouncedInput inputs[] = {
  { TOG1_TOP, 10, false, false, 0 },
  { TOG1_BOT, 11, false, false, 0 },
  { TOG2_TOP, 12, false, false, 0 },
  { TOG2_BOT, 13, false, false, 0 },
  { TOG3_TOP, 14, false, false, 0 },
  { TOG3_BOT, 15, false, false, 0 },
  { BTN1_SW,  16, false, false, 0 },
  { BTN2_SW,  17, false, false, 0 },
  { BTN3_SW,  18, false, false, 0 },
  { BTN4_SW,  19, false, false, 0 },
  { ENC1_SW,   3, false, false, 0 },
  { ENC2_SW,   6, false, false, 0 },
  { ENC3_SW,   9, false, false, 0 },
};
const int NUM_INPUTS = sizeof(inputs) / sizeof(inputs[0]);

// Encoders: track the combined CLK+DT state as a 2-bit value, and only
// register a click once a complete, valid rotation sequence has been
// seen. This is far more resistant to fast-turn misreads than checking
// a single pin's edge, because a genuine click always produces the same
// specific sequence of states — partial/noisy transitions get ignored
// instead of being misread as a click in the wrong direction.
struct EncoderState {
  int clkPin;
  int dtPin;
  int buttonUp;
  int buttonDown;
  uint8_t stateHistory;   // rolling record of recent CLK+DT states
  int8_t accumulator;      // builds up across several reads before firing
  unsigned long upReleaseTime;
  unsigned long downReleaseTime;
};

EncoderState encoders[] = {
  { ENC1_CLK, ENC1_DT, 1, 2, 0, 0, 0, 0 },
  { ENC2_CLK, ENC2_DT, 4, 5, 0, 0, 0, 0 },
  { ENC3_CLK, ENC3_DT, 7, 8, 0, 0, 0, 0 },
};
const int NUM_ENCODERS = sizeof(encoders) / sizeof(encoders[0]);

// This table maps every possible "previous state -> new state" combination
// to +1 (one direction), -1 (other direction), or 0 (invalid/bounce,
// ignored). This is the standard, well-proven quadrature decode table —
// it's what makes fast, noisy turns read reliably instead of guessing
// from a single instant.
const int8_t QUAD_TABLE[16] = {
  0, -1, 1, 0,
  1, 0, 0, -1,
  -1, 0, 0, 1,
  0, 1, -1, 0
};

// LED state for each of the 4 button LEDs, so we can support on / off / flash
// (both harsh and smooth-fade versions of each)
struct LedState {
  int pin;
  bool on;              // steady on/off target (harsh mode)
  bool flashing;         // if true, ignore "on" and blink instead (harsh mode)
  unsigned long flashIntervalMs;
  unsigned long lastToggleTime;
  bool flashPhase;       // current on/off phase while flashing (harsh mode)

  bool fading;            // if true, this LED is in one of the smooth-fade modes
  bool fadeMode;           // true = flashing smoothly, false = one-off fade to a target
  int fadeTarget;           // brightness (0-255) we're fading toward, for fade on/off
  int fadeCurrent;          // current brightness level (0-255)
  int fadeStep;              // how much brightness changes per update
  unsigned long lastFadeUpdate;
};

LedState leds[] = {
  { BTN1_LED, false, false, 300, 0, false, false, false, 0, 0, 4, 0 },
  { BTN2_LED, false, false, 300, 0, false, false, false, 0, 0, 4, 0 },
  { BTN3_LED, false, false, 300, 0, false, false, false, 0, 0, 4, 0 },
  { BTN4_LED, false, false, 300, 0, false, false, false, 0, 0, 4, 0 },
};
const int NUM_LEDS = sizeof(leds) / sizeof(leds[0]);

// ---------------------------------------------------------------------
// SETUP
// ---------------------------------------------------------------------
void setup() {
  // All switch/button pins use the Pico's internal pull-up resistor.
  // This means: not pressed = reads HIGH, pressed = reads LOW (pulled to GND).
  for (int i = 0; i < NUM_INPUTS; i++) {
    pinMode(inputs[i].pin, INPUT_PULLUP);
  }
  for (int i = 0; i < NUM_ENCODERS; i++) {
    pinMode(encoders[i].clkPin, INPUT_PULLUP);
    pinMode(encoders[i].dtPin, INPUT_PULLUP);
    int clk = digitalRead(encoders[i].clkPin);
    int dt = digitalRead(encoders[i].dtPin);
    encoders[i].stateHistory = (clk << 1) | dt;
    encoders[i].accumulator = 0;
  }
  for (int i = 0; i < NUM_LEDS; i++) {
    pinMode(leds[i].pin, OUTPUT);
    analogWrite(leds[i].pin, LED_DIM_FLOOR);
  }

  // OLED setup — same steps proven working in oled_test.ino
  Wire.setSDA(OLED_SDA);
  Wire.setSCL(OLED_SCL);
  Wire.begin();
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.clearDisplay();
  display.display();
  // Screen stays blank from here until SimHub sends a DISP| command.

  Serial.begin(115200);   // USB serial link, used for LED + screen commands from SimHub
  Joystick.begin();       // starts the USB joystick device
}

// ---------------------------------------------------------------------
// MAIN LOOP
// ---------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  readDebouncedInputs(now);
  readEncoders(now);
  readSerialCommands();
  checkSimHubTimeout(now);
  updateLeds(now);
}

// ---------------------------------------------------------------------
// Reads all toggle switches, LED buttons, and encoder push-switches.
// Applies simple debouncing: a change is only accepted once the new
// state has held steady for DEBOUNCE_MS.
// ---------------------------------------------------------------------
void readDebouncedInputs(unsigned long now) {
  for (int i = 0; i < NUM_INPUTS; i++) {
    bool rawPressed = (digitalRead(inputs[i].pin) == LOW); // LOW = pressed

    if (rawPressed != inputs[i].lastRawState) {
      inputs[i].lastRawState = rawPressed;
      inputs[i].lastChangeTime = now;
    }

    if ((now - inputs[i].lastChangeTime) > DEBOUNCE_MS) {
      if (rawPressed != inputs[i].lastStableState) {
        inputs[i].lastStableState = rawPressed;
        Joystick.button(inputs[i].joystickButton, rawPressed);
      }
    }
  }
}

// ---------------------------------------------------------------------
// Reads all 3 rotary encoders using full quadrature state tracking.
// Every loop, we read both CLK and DT together, compare against the
// last reading, and look up whether that specific transition is part
// of a genuine rotation or is noise/bounce (ignored). A full detent
// click only fires once 4 valid steps in the same direction have been
// seen in a row, matching how these encoders mechanically work — this
// is what makes fast twists read cleanly instead of misfiring.
// ---------------------------------------------------------------------
void readEncoders(unsigned long now) {
  for (int i = 0; i < NUM_ENCODERS; i++) {
    int clk = digitalRead(encoders[i].clkPin);
    int dt = digitalRead(encoders[i].dtPin);
    uint8_t newState = (clk << 1) | dt;

    uint8_t lookup = ((encoders[i].stateHistory & 0x03) << 2) | newState;
    int8_t movement = QUAD_TABLE[lookup];

    if (movement != 0) {
      encoders[i].accumulator += movement;

      if (encoders[i].accumulator >= 4) {
        Joystick.button(encoders[i].buttonUp, true);
        encoders[i].upReleaseTime = now + ENCODER_PULSE_MS;
        encoders[i].accumulator = 0;
      } else if (encoders[i].accumulator <= -4) {
        Joystick.button(encoders[i].buttonDown, true);
        encoders[i].downReleaseTime = now + ENCODER_PULSE_MS;
        encoders[i].accumulator = 0;
      }
    }

    encoders[i].stateHistory = newState;

    // Auto-release the pulse buttons after their hold time expires
    if (encoders[i].upReleaseTime != 0 && now >= encoders[i].upReleaseTime) {
      Joystick.button(encoders[i].buttonUp, false);
      encoders[i].upReleaseTime = 0;
    }
    if (encoders[i].downReleaseTime != 0 && now >= encoders[i].downReleaseTime) {
      Joystick.button(encoders[i].buttonDown, false);
      encoders[i].downReleaseTime = 0;
    }
  }
}

// ---------------------------------------------------------------------
// Listens on USB serial for simple text commands to control the LEDs.
// This is what SimHub (or a small companion script) will send.
//
// Command format, one line at a time, e.g.:
//   LED1_ON                harsh instant on
//   LED1_OFF                harsh instant off
//   LED1_FLASH_300          harsh blink, 300ms on/off interval
//   LED1_FADE_ON             smooth fade up to fully on
//   LED1_FADE_OFF             smooth fade down to fully off
//   LED1_FADE_FLASH_500        smooth breathing fade, 500ms per half-cycle
// Both the harsh and fade versions exist side by side — SimHub can send
// whichever one fits the situation, freely mixed, nothing locked to one style.
//
// The OLED screen is controlled with a separate command, also one line:
//   DISP|Line1 text|Line2 text|Line3 text
// Each part between the | characters becomes one line of text on the
// screen, replacing whatever was there before. You can send 1, 2, or 3
// lines — e.g. DISP|Fuel: 12.4L on its own just shows one line.
// This is intentionally generic: the Pico doesn't know or care whether
// the text is fuel, tyre temps, or a flag — SimHub decides what to send
// and when, so what's shown on screen can be changed anytime from the
// SimHub side without ever touching this code again.
// ---------------------------------------------------------------------
void readSerialCommands() {
  if (!Serial.available()) return;

  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  // Any command at all counts as proof SimHub is still connected and running.
  lastCommandReceivedTime = millis();

  if (line.startsWith("DISP|")) {
    updateDisplay(line);
    screenIsBlank = false;
    return;
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    String prefix = "LED" + String(i + 1);
    if (!line.startsWith(prefix)) continue;

    if (line == prefix + "_ON") {
      leds[i].on = true;
      leds[i].flashing = false;
      leds[i].fading = false;
      digitalWrite(leds[i].pin, HIGH);
    } else if (line == prefix + "_OFF") {
      leds[i].on = false;
      leds[i].flashing = false;
      leds[i].fading = false;
      analogWrite(leds[i].pin, LED_DIM_FLOOR);
    } else if (line.startsWith(prefix + "_FLASH_")) {
      String msPart = line.substring((prefix + "_FLASH_").length());
      unsigned long interval = msPart.toInt();
      if (interval < 30) interval = 30; // safety minimum so it can't flicker unusably fast
      leds[i].flashing = true;
      leds[i].fading = false;
      leds[i].flashIntervalMs = interval;
      leds[i].lastToggleTime = millis();
      leds[i].flashPhase = true;
      digitalWrite(leds[i].pin, HIGH);
    } else if (line == prefix + "_FADE_ON") {
      leds[i].flashing = false;
      leds[i].fading = true;
      leds[i].fadeMode = false; // one-off fade, not breathing
      leds[i].fadeTarget = 255;
      leds[i].lastFadeUpdate = millis();
    } else if (line == prefix + "_FADE_OFF") {
      leds[i].flashing = false;
      leds[i].fading = true;
      leds[i].fadeMode = false;
      leds[i].fadeTarget = LED_DIM_FLOOR;
      leds[i].lastFadeUpdate = millis();
    } else if (line.startsWith(prefix + "_FADE_FLASH_")) {
      String msPart = line.substring((prefix + "_FADE_FLASH_").length());
      unsigned long halfCycle = msPart.toInt();
      if (halfCycle < 50) halfCycle = 50; // safety minimum for a smooth-looking fade
      leds[i].flashing = false;
      leds[i].fading = true;
      leds[i].fadeMode = true; // continuous breathing
      leds[i].fadeTarget = 255;
      // work out how much to step brightness by each 10ms tick to complete
      // the fade within roughly halfCycle milliseconds
      leds[i].fadeStep = max(1, (int)(255 / (halfCycle / 10)));
      leds[i].lastFadeUpdate = millis();
    }
  }
}

// ---------------------------------------------------------------------
// If we haven't heard anything from SimHub for SIMHUB_TIMEOUT_MS, assume
// it has closed or disconnected and blank the screen ourselves. This
// only fires once per disconnect (screenIsBlank guards against clearing
// the display repeatedly every loop while already blank).
// ---------------------------------------------------------------------
void checkSimHubTimeout(unsigned long now) {
  if (screenIsBlank) return; // already blank, nothing to do

  if (lastCommandReceivedTime == 0) return; // nothing ever received yet, stay blank

  if (now - lastCommandReceivedTime > SIMHUB_TIMEOUT_MS) {
    display.clearDisplay();
    display.display();
    screenIsBlank = true;
  }
}

// ---------------------------------------------------------------------
// Splits a DISP|Line1|Line2|Line3 command on the | character and draws
// each part on its own line of the OLED, replacing the previous content.
// ---------------------------------------------------------------------
void updateDisplay(String line) {
  // Strip the leading "DISP|"
  String remaining = line.substring(5);

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  int lineNum = 0;
  while (remaining.length() > 0 && lineNum < 4) {
    int barIndex = remaining.indexOf('|');
    String thisLine;

    if (barIndex == -1) {
      thisLine = remaining;
      remaining = "";
    } else {
      thisLine = remaining.substring(0, barIndex);
      remaining = remaining.substring(barIndex + 1);
    }

    display.println(thisLine);
    lineNum++;
  }

  display.display();
}

// ---------------------------------------------------------------------
// Handles the actual on/off blinking for any LED currently in harsh
// flash mode, AND the smooth brightness stepping for any LED in a
// fade mode (fade-on, fade-off, or continuous breathing fade).
// ---------------------------------------------------------------------
void updateLeds(unsigned long now) {
  for (int i = 0; i < NUM_LEDS; i++) {

    // --- Harsh flash handling (unchanged from before) ---
    if (leds[i].flashing) {
      if (now - leds[i].lastToggleTime >= leds[i].flashIntervalMs) {
        leds[i].flashPhase = !leds[i].flashPhase;
        analogWrite(leds[i].pin, leds[i].flashPhase ? 255 : LED_DIM_FLOOR);
        leds[i].lastToggleTime = now;
      }
      continue;
    }

    // --- Smooth fade handling (new) ---
    if (leds[i].fading) {
      // Update brightness roughly every 10ms for a smooth visual result
      if (now - leds[i].lastFadeUpdate < 10) continue;
      leds[i].lastFadeUpdate = now;

      if (leds[i].fadeMode) {
        // Continuous breathing: bounce between LED_DIM_FLOOR and 255
        leds[i].fadeCurrent += leds[i].fadeStep;
        if (leds[i].fadeCurrent >= 255) {
          leds[i].fadeCurrent = 255;
          leds[i].fadeStep = -leds[i].fadeStep;
        } else if (leds[i].fadeCurrent <= LED_DIM_FLOOR) {
          leds[i].fadeCurrent = LED_DIM_FLOOR;
          leds[i].fadeStep = -leds[i].fadeStep;
        }
      } else {
        // One-off fade toward fadeTarget (fully on or fully off), then stop
        if (leds[i].fadeCurrent < leds[i].fadeTarget) {
          leds[i].fadeCurrent = min(255, leds[i].fadeCurrent + 5);
        } else if (leds[i].fadeCurrent > leds[i].fadeTarget) {
          leds[i].fadeCurrent = max(0, leds[i].fadeCurrent - 5);
        }
      }

      analogWrite(leds[i].pin, leds[i].fadeCurrent);
    }
  }
}
