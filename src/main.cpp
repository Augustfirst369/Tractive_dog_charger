// ====== switch from Arduino IDE to Antigravity ====++
#include <Arduino.h>
#include <Wire.h>
#include <INA219_WE.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// ================= USER CONFIG =================
#define UNIT_ID 1
#define SERIAL_BAUD 115200
#define NUM_CH 10

const float CELL_CAPACITY_mAh = 930.0; // battery cell capacity

const float CURRENT_ACTIVE_mA = 20.0; //setup batery's minimus charging current
const float CURRENT_RESET_mA = 1.0;
const unsigned long RESET_DELAY_MS = 2000;
const unsigned long REPORT_MS = 1000;

// LED / Button / Fan
#define LED_PIN 10
#define LED_COUNT 10
#define LED_BRIGHTNESS 50
#define BUTTON_PIN 9
#define FAN_PIN 11

#define SOC_SELECT_TIMEOUT_MS 3000
#define RED_BLINK_PERIOD_MS 1000

// ---------- FAN CONFIG ----------
#define FAN_START_mA 60.0

// ================= OBJECTS =================
INA219_WE ina[NUM_CH] = {
  INA219_WE(0x40), INA219_WE(0x41), INA219_WE(0x42), INA219_WE(0x43), INA219_WE(0x44),
  INA219_WE(0x45), INA219_WE(0x46), INA219_WE(0x47), INA219_WE(0x48), INA219_WE(0x49)
};

Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ================= CONSTANTS =================
const float SHUNT_OHMS = 0.033;
const float CC_SCALE = 4.94 / 1023.0;

// ================= STATE =================
float soc[NUM_CH];
float added_mAh[NUM_CH];
unsigned long last_ms[NUM_CH];
unsigned long zeroCurrentSince_ms[NUM_CH];

// UI
uint8_t targetSocLevel;
bool socSelectActive = false;
unsigned long lastButtonMs = 0;

// Blink
bool redBlinkState = false;
unsigned long lastBlinkMs = 0;

// Fan state
bool fanRunning = false;

// ================= CC MUX =================
const uint8_t PIN_S0 = 3, PIN_S1 = 4, PIN_S2 = 5, PIN_S3 = 6;
const uint8_t EN_MUX1 = 7, EN_MUX2 = 8;
const uint8_t ADC_MUX1 = A0, ADC_MUX2 = A1;

struct CCMap {
  uint8_t sel, mux;
};

CCMap cc1_map[NUM_CH] = {
  { 8, 2 }, { 1, 2 }, { 3, 2 }, { 5, 2 }, { 7, 2 }, { 8, 1 }, { 1, 1 }, { 3, 1 }, { 5, 1 }, { 7, 1 }
};
CCMap cc2_map[NUM_CH] = {
  { 9, 2 }, { 0, 2 }, { 2, 2 }, { 4, 2 }, { 6, 2 }, { 9, 1 }, { 0, 1 }, { 2, 1 }, { 4, 1 }, { 6, 1 }
};

// ================= HELPERS =================
void muxEnable(uint8_t id) {
  digitalWrite(EN_MUX1, id == 1 ? LOW : HIGH);
  digitalWrite(EN_MUX2, id == 2 ? LOW : HIGH);
}
void muxSelect(uint8_t sel) {
  digitalWrite(PIN_S0, sel & 1);
  digitalWrite(PIN_S1, sel & 2);
  digitalWrite(PIN_S2, sel & 4);
  digitalWrite(PIN_S3, sel & 8);
}
float readCC(uint8_t id, uint8_t sel) {
  muxEnable(id);
  muxSelect(sel);
  delayMicroseconds(200);
  int raw = analogRead(id == 1 ? ADC_MUX1 : ADC_MUX2);
  digitalWrite(EN_MUX1, HIGH);
  digitalWrite(EN_MUX2, HIGH);
  return raw * CC_SCALE;
}
void setupINA(INA219_WE &s) {
  s.init();
  s.setShuntSizeInOhms(SHUNT_OHMS);
  s.setADCMode(INA219_SAMPLE_MODE_64);
  s.setMeasureMode(INA219_CONTINUOUS);
  s.setPGain(INA219_PG_40);
  s.setBusRange(INA219_BRNG_32);
}

// ================= FAN CONTROL =================
void updateFan(float totalCurrent) {
  if (totalCurrent >= FAN_START_mA) {
    if (!fanRunning) {
      fanRunning = true;
      digitalWrite(FAN_PIN, HIGH);
      Serial.println("Fan state: ON");
    }
  } else {
    if (fanRunning) {
      fanRunning = false;
      digitalWrite(FAN_PIN, LOW);
      Serial.println("Fan state: OFF");
    }
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(SERIAL_BAUD);
  Wire.begin();

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(FAN_PIN, LOW);

  pinMode(PIN_S0, OUTPUT);
  pinMode(PIN_S1, OUTPUT);
  pinMode(PIN_S2, OUTPUT);
  pinMode(PIN_S3, OUTPUT);
  pinMode(EN_MUX1, OUTPUT);
  pinMode(EN_MUX2, OUTPUT);
  digitalWrite(EN_MUX1, HIGH);
  digitalWrite(EN_MUX2, HIGH);

  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.show();

  EEPROM.get(0, targetSocLevel);
  if (targetSocLevel < 1 || targetSocLevel > 10) targetSocLevel = 1;

  for (int i = 0; i < NUM_CH; i++) {
    setupINA(ina[i]);
    soc[i] = 0;
    added_mAh[i] = 0;
    last_ms[i] = millis();
    zeroCurrentSince_ms[i] = 0;
  }

  Serial.println("READY");
}

// ================= LOOP =================
void loop() {
  unsigned long now = millis();

  // ---- blink engine ----
  if (now - lastBlinkMs >= RED_BLINK_PERIOD_MS / 2) {
    lastBlinkMs = now;
    redBlinkState = !redBlinkState;
  }

  // ---- button ----
  static bool lastBtn = HIGH;
  bool btn = digitalRead(BUTTON_PIN);
  if (lastBtn == HIGH && btn == LOW) {
    if (!socSelectActive) socSelectActive = true;
    else {
      targetSocLevel++;
      if (targetSocLevel > 10) targetSocLevel = 1;
      EEPROM.put(0, targetSocLevel);
    }
    lastButtonMs = now;
  }
  lastBtn = btn;
  if (socSelectActive && now - lastButtonMs > SOC_SELECT_TIMEOUT_MS)
    socSelectActive = false;

  // ---- SoC + INA recovery ----
  float totalCurrent = 0;
  bool anyLoad = false;

  for (int i = 0; i < NUM_CH; i++) {
    float cur = ina[i].getCurrent_mA();
    totalCurrent += cur;
    if (cur >= CURRENT_RESET_mA) anyLoad = true;

    float cc1 = readCC(cc1_map[i].mux, cc1_map[i].sel);
    float cc2 = readCC(cc2_map[i].mux, cc2_map[i].sel);
    bool cc1_ok = (cc1 >= 0.4 && cc1 <= 0.6);
    bool cc2_ok = (cc2 >= 0.4 && cc2 <= 0.6);

    if ((cc1_ok ^ cc2_ok) && cur < CURRENT_RESET_mA) {
      setupINA(ina[i]);
      last_ms[i] = now;
    }

    unsigned long dt = now - last_ms[i];
    last_ms[i] = now;

    if (cur < CURRENT_RESET_mA) {
      if (zeroCurrentSince_ms[i] == 0) zeroCurrentSince_ms[i] = now;
      if (now - zeroCurrentSince_ms[i] >= RESET_DELAY_MS) {
        added_mAh[i] = 0;
        soc[i] = 0;
      }
      continue;
    }
    zeroCurrentSince_ms[i] = 0;

    if (cur >= CURRENT_ACTIVE_mA) {
      added_mAh[i] += cur * dt / 3600000.0;
      soc[i] = min(100.0, 100.0 * added_mAh[i] / CELL_CAPACITY_mAh);
    }
  }

  // ---- LEDs (UNCHANGED) ----
  strip.clear();

  if (socSelectActive) {
    for (int i = 0; i < targetSocLevel; i++)
      strip.setPixelColor(i, strip.Color(0, 255, 0));
  } else if (!anyLoad) {
    static int pos = 0, dir = 1;
    static unsigned long lastAnim = 0;
    if (now - lastAnim > 1000) {
      lastAnim = now;
      pos += dir;
      if (pos == 0 || pos == LED_COUNT - 1) dir = -dir;
    }
    strip.setPixelColor(pos, strip.Color(255, 120, 0));
  } else {
    for (int i = 0; i < NUM_CH; i++) {
      float cur = ina[i].getCurrent_mA();
      if (anyLoad && cur < CURRENT_RESET_mA) continue;

      float cc1 = readCC(cc1_map[i].mux, cc1_map[i].sel);
      float cc2 = readCC(cc2_map[i].mux, cc2_map[i].sel);
      bool cc1_ok = (cc1 >= 0.4 && cc1 <= 0.6);
      bool cc2_ok = (cc2 >= 0.4 && cc2 <= 0.6);

      uint32_t color = 0;
      if (cc1_ok == cc2_ok) color = strip.Color(255, 0, 0);
      else if (cur <= 400 && redBlinkState) color = strip.Color(255, 0, 0);
      else if (soc[i] >= targetSocLevel * 10) color = strip.Color(0, 255, 0);
      else if (cur > 300) color = strip.Color(0, 0, 255);

      strip.setPixelColor(i, color);
    }
  }
  strip.show();

  // ---- reporting + FAN UPDATE ----
  static unsigned long lastReport = 0;
  if (now - lastReport >= REPORT_MS) {
    lastReport = now;

    updateFan(totalCurrent);

    Serial.print("UNIT:");
    Serial.println(UNIT_ID);
    for (int i = 0; i < NUM_CH; i++) {
      float shunt = ina[i].getShuntVoltage_mV();
      float busV = ina[i].getBusVoltage_V();
      float loadV = busV + shunt / 1000.0;
      Serial.print("CH");
      Serial.print(i + 1);
      Serial.print(": I=");
      Serial.print(ina[i].getCurrent_mA(), 1);
      Serial.print(", V=");
      Serial.print(loadV, 1);
      Serial.print(", CC1=");
      Serial.print(readCC(cc1_map[i].mux, cc1_map[i].sel), 1);
      Serial.print(", CC2=");
      Serial.print(readCC(cc2_map[i].mux, cc2_map[i].sel), 1);
      Serial.print(", SOC=");
      Serial.println(soc[i], 1);
    }
    Serial.println("END");
  }
}