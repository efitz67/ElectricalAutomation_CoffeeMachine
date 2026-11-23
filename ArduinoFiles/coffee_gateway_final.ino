/*
  Coffee machine gateway - Arduino Uno <-> Mitsubishi PLC
  Talks to coffee_interface.html over USB serial (9600 baud).
  The PLC runs the machine and decides everything (cup detection, timing,
  pumps, stopping). This sketch only translates.

  Page -> Arduino:   SUGAR:0 | SUGAR:1
                     MILK:0  | MILK:1
                     START
  Arduino -> page:   STATE:IDLE | STATE:MENU | STATE:BREW | POS:1..4 | STATE:PICKUP

  Brew order (matches the ladder): 1 coffee, 2 sugar, 3 water, 4 milk

  PLC status code (3 bits from PLC outputs Y13, Y14, Y15 = bit 0, 1, 2):
    0 idle (no cup)
    1 menu (cup present, waiting for Start)
    2 coffee  (ladder M1 move + M2 coffee)       -> page step 1
    3 sugar   (ladder M3 move + M4 sugar)        -> page step 2
    4 water   (ladder M5 move + M6 water pump)   -> page step 3
    5 milk    (ladder M7 milk pump + M8 return)  -> page step 4
    6 pickup  (drink finished, waiting for cup removal)

  Wiring (non-isolated: 2N3904/2N3906 outputs, resistor-divider inputs).
  Arduino GND must be tied to PLC 0V.
    D4  SUGAR -> PLC X0   (ladder: "sugar selected")
    D5  MILK  -> PLC X1   (ladder: "milk selected")
    D6  START -> PLC X2   (ladder: "turned on = runs")
    D10 <- PLC Y13   status bit 0
    D11 <- PLC Y14   status bit 1
    D12 <- PLC Y15   status bit 2

  Drink-ready hold: once the last step (code 5, milk) has been reached, the page shows
  "ready / lift your cup" and stays there until the PLC has reported code 0 (cup gone)
  for REMOVED_MS, and for at least READY_MIN_MS in total. A code 1 (cup detected again
  because the cup is back at the start position) does NOT open the menu during that time.

  Heater thermostat (runs on its own, independent of the PLC and the page):
    A0  <- Pt100 temperature probe: 5V -> reference resistor (390R) -> A0 -> Pt100 -> GND
           (join the two red wires together and the two white wires together;
            optional 100 nF capacitor from A0 to GND to calm the reading)
    D8  -> heater switch: logic-level N-MOSFET gate (100R in series, 10k gate to GND)
  The heater turns ON below TEMP_ON_BELOW and OFF above TEMP_OFF_ABOVE.
  If the probe is missing or faulty the heater is forced OFF.
  Extra serial lines like "TEMP:31.2,HEATER:ON" are ignored by the page.
  No extra libraries are needed.
*/

// ---------- Arduino -> PLC ----------
const uint8_t PIN_SUGAR = 4;    // level -> X0: 0 = no sugar, 1 = sugar
const uint8_t PIN_MILK  = 5;    // level -> X1: 0 = no milk,  1 = milk
const uint8_t PIN_START = 6;    // pulse -> X2: order is final, PLC begins

// ---------- PLC -> Arduino ----------
const uint8_t PIN_ST0 = 10;     // <- Y13, status code bit 0
const uint8_t PIN_ST1 = 11;     // <- Y14, status code bit 1
const uint8_t PIN_ST2 = 12;     // <- Y15, status code bit 2

// Transistor circuit: Arduino HIGH = PLC input ON.  Divider: PLC output ON = pin HIGH.
// (If you switch to optocouplers on the PLC -> Arduino side, set IN_ACTIVE_LOW = true.)
const bool OUT_ACTIVE_LOW = false;
const bool IN_ACTIVE_LOW  = false;

const unsigned long PULSE_MS  = 300;  // START pulse length (PLC scan time is far shorter)
const unsigned long SETTLE_MS = 100;  // let SUGAR/MILK settle before START
const unsigned long STABLE_MS = 80;   // status bits must hold still this long before we trust them
const unsigned long READY_MIN_MS = 3000;  // "drink ready" stays on screen at least this long
const unsigned long REMOVED_MS   = 1000;  // code 0 must hold this long to count as "cup removed"

uint8_t stableCode = 0xFF;   // last announced status code (0xFF = nothing announced yet)
uint8_t lastRaw    = 0xFF;
unsigned long rawChanged = 0;

bool brewed = false;           // a drink was finished and the cup has not been removed yet
bool readyShown = false;       // STATE:PICKUP has been sent for this drink
unsigned long readyAt = 0;     // when "ready" was first shown
unsigned long idleSince = 0;   // when the status last became 0

// ---------- Helpers ----------
void out(uint8_t pin, bool on) {
  digitalWrite(pin, (on != OUT_ACTIVE_LOW) ? HIGH : LOW);
}

void initOut(uint8_t pin) {
  out(pin, false);       // "off" level first, so the PLC never sees a glitch at boot
  pinMode(pin, OUTPUT);
}

uint8_t readStatusRaw() {
  uint8_t v = 0;
  if ((digitalRead(PIN_ST0) == LOW) == IN_ACTIVE_LOW) v |= 1;
  if ((digitalRead(PIN_ST1) == LOW) == IN_ACTIVE_LOW) v |= 2;
  if ((digitalRead(PIN_ST2) == LOW) == IN_ACTIVE_LOW) v |= 4;
  return v;
}

char lineBuf[32];
uint8_t lineLen = 0;
bool readLine() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { lineBuf[lineLen] = 0; lineLen = 0; return true; }
    if (c != '\r' && lineLen < sizeof(lineBuf) - 1) lineBuf[lineLen++] = c;
  }
  return false;
}

// ---------- PLC status -> page ----------
void announce(uint8_t code, uint8_t prev) {
  switch (code) {
    case 0: Serial.println(F("STATE:IDLE")); break;
    case 1: Serial.println(F("STATE:MENU")); break;
    case 2:   // coffee
    case 3:   // sugar
    case 4:   // water
    case 5:   // milk
      if (prev < 2 || prev > 5) Serial.println(F("STATE:BREW"));
      Serial.print(F("POS:")); Serial.println(code - 1);
      break;
    case 6: Serial.println(F("STATE:PICKUP")); break;
    default: Serial.println(F("ERROR:PLC_STATUS")); break;  // the page ignores unknown lines
  }
}

// Called once for every new stable status code.
void onNewCode(uint8_t code, uint8_t prev) {
  if (code >= 2 && code <= 4) brewed = false;                  // a new brew started
  if (code == 5) { brewed = true; readyShown = false; }        // last step reached: drink is being finished

  if (brewed && code != 5) {
    // Drink finished. Show "ready" and wait for the cup to be removed instead of
    // opening the menu again just because the cup is back at the start position.
    if (!readyShown) {
      readyShown = true;
      readyAt = millis();
      Serial.println(F("STATE:PICKUP"));
    }
    if (code == 0) idleSince = millis();
    return;                                                    // STATE:IDLE is sent by checkCupRemoved()
  }

  // New cup session (idle or fresh menu): start from "no sugar, no milk"
  if (code == 0 || code == 1) {
    out(PIN_SUGAR, false);
    out(PIN_MILK,  false);
  }
  announce(code, prev);
}

// After a finished drink: go back to idle once the cup has really been removed.
void checkCupRemoved() {
  if (!brewed || !readyShown || stableCode != 0) return;
  unsigned long now = millis();
  if (now - idleSince >= REMOVED_MS && now - readyAt >= READY_MIN_MS) {
    brewed = false;
    readyShown = false;
    out(PIN_SUGAR, false);
    out(PIN_MILK,  false);
    Serial.println(F("STATE:IDLE"));
  }
}

void pollStatus() {
  uint8_t raw = readStatusRaw();
  if (raw != lastRaw) { lastRaw = raw; rawChanged = millis(); }
  if (millis() - rawChanged >= STABLE_MS && raw != stableCode) {
    uint8_t prev = stableCode;
    stableCode = raw;
    onNewCode(raw, prev);
  }
  checkCupRemoved();
}

// ---------- Page -> PLC ----------
void pulse(uint8_t pin) {
  out(pin, true);
  delay(PULSE_MS);
  out(pin, false);
}

// ---------- Heater + temperature (independent of the PLC) ----------
const uint8_t PIN_TEMP   = A0;    // Pt100 divider midpoint
const uint8_t PIN_HEATER = 8;     // -> MOSFET gate (HIGH = heater on)
const bool HEATER_ACTIVE_LOW = false;   // set true if you use an active-LOW relay module

// Pt100 divider: 5V -> R_REF -> A0 -> Pt100 -> GND
const float R_REF      = 390.0;   // ohms: MEASURE your resistor with the multimeter and type its real value here
const float R0         = 100.0;   // Pt100 resistance at 0 degC
const float TEMP_CAL_C = 0.0;     // calibration offset (degC): adjust after comparing with a thermometer
const uint8_t SAMPLES  = 64;      // readings averaged per measurement

const float TEMP_ON_BELOW  = 33.0;   // heater switches ON below this (deg C)
const float TEMP_OFF_ABOVE = 37.0;   // heater switches OFF above this (deg C)
const float TEMP_MAX       = 50.0;   // hard safety cut-off (deg C)

const unsigned long TEMP_PERIOD_MS = 2000;  // how often we measure
const unsigned long PRINT_MS       = 5000;  // how often TEMP: lines are printed

bool heaterOn = false;
bool sensorOk = true;
unsigned long lastTempRead = 0;
unsigned long lastTempPrint = 0;

void setHeater(bool on) {
  heaterOn = on;
  digitalWrite(PIN_HEATER, (on != HEATER_ACTIVE_LOW) ? HIGH : LOW);
}

// Returns the temperature in deg C, or NAN if the probe looks open or shorted.
float readTempC() {
  long sum = 0;
  for (uint8_t i = 0; i < SAMPLES; i++) {
    sum += analogRead(PIN_TEMP);
    delayMicroseconds(200);
  }
  float adc = sum / (float)SAMPLES;
  if (adc < 5.0 || adc > 1018.0) return NAN;          // open or shorted probe

  float r = R_REF * adc / (1023.0 - adc);             // Pt100 resistance in ohms
  // Callendar-Van Dusen (0 to 100 degC): R = R0 * (1 + A*T + B*T^2), solved for T
  const float A = 3.9083e-3, B = -5.775e-7;
  float inner = A * A - 4.0 * B * (1.0 - r / R0);
  if (inner < 0) return NAN;
  return (-A + sqrt(inner)) / (2.0 * B) + TEMP_CAL_C;
}

void pollTemperature() {
  unsigned long now = millis();
  if (now - lastTempRead < TEMP_PERIOD_MS) return;
  lastTempRead = now;

  float t = readTempC();
  if (isnan(t) || t < -20.0 || t > 120.0) {           // probe missing or faulty
    setHeater(false);
    if (sensorOk) Serial.println(F("ERROR:TEMP_SENSOR"));
    sensorOk = false;
    return;
  }
  sensorOk = true;

  bool wasOn = heaterOn;
  if (t >= TEMP_OFF_ABOVE || t >= TEMP_MAX) setHeater(false);
  else if (t <= TEMP_ON_BELOW) setHeater(true);
  // between the two limits: keep the current state (hysteresis)

  if (heaterOn != wasOn || now - lastTempPrint >= PRINT_MS) {
    lastTempPrint = now;
    Serial.print(F("TEMP:")); Serial.print(t, 1);
    Serial.print(F(",HEATER:")); Serial.println(heaterOn ? "ON" : "OFF");
  }
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(9600);

  initOut(PIN_SUGAR);
  initOut(PIN_MILK);
  initOut(PIN_START);

  // Active-low inputs (optocouplers) use the internal pull-up.
  // Active-high inputs (resistor divider with its own pull-down) must not.
  uint8_t inMode = IN_ACTIVE_LOW ? INPUT_PULLUP : INPUT;
  pinMode(PIN_ST0, inMode);
  pinMode(PIN_ST1, inMode);
  pinMode(PIN_ST2, inMode);

  // Heater (starts OFF) and temperature probe
  setHeater(false);               // set the "off" level first, then make it an output
  pinMode(PIN_HEATER, OUTPUT);
}

void loop() {
  if (readLine()) {
    // Commands are only accepted while the PLC is in the menu state.
    // SUGAR and MILK are levels: they stay set until the cup session ends.
    if (stableCode == 1 && !brewed) {
      if      (strncmp(lineBuf, "SUGAR:", 6) == 0) out(PIN_SUGAR, lineBuf[6] == '1');
      else if (strncmp(lineBuf, "MILK:", 5) == 0)  out(PIN_MILK,  lineBuf[5] == '1');
      else if (strcmp(lineBuf, "START") == 0)    { delay(SETTLE_MS); pulse(PIN_START); }
    }
  }
  pollStatus();
  pollTemperature();
}
