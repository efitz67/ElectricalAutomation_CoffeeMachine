/*
  Coffee machine gateway - Arduino Uno <-> Mitsubishi FX5S PLC (simplified)
  Talks to coffee_interface.html over USB serial (9600 baud).
  The PLC runs the machine and decides everything (cup detection, delay,
  pouring, stopping if the cup is removed). This sketch only translates.

  Page -> Arduino:   MILK:0 | MILK:1
                     SUGAR:0 | SUGAR:1
                     START
  Arduino -> page:   STATE:IDLE | STATE:MENU | STATE:BREW | POS:1..4 | STATE:PICKUP

  PLC status code (3 bits, from PLC outputs Y10-Y12):
    0 idle (no cup)
    1 menu (cup detected, waiting for the Start button)
    2 grinds | 3 water | 4 milk | 5 sugar   (page shows these as steps 1..4)
    6 pickup (drink finished, waiting for cup removal)
  If the cup is removed at any time, the PLC stops and returns to code 0.

  Wiring: non-isolated build (MOSFET outputs, resistor-divider inputs).
  Arduino GND must be tied to PLC 0V.
*/

// ---------- Arduino -> PLC (MOSFET circuits into PLC inputs X) ----------
const uint8_t PIN_MILK  = 4;    // level: 0 = no milk,  1 = milk
const uint8_t PIN_SUGAR = 5;    // level: 0 = no sugar, 1 = sugar
const uint8_t PIN_START = 6;    // pulse: order is final, PLC latches milk/sugar and begins

// ---------- PLC -> Arduino (resistor dividers from PLC outputs Y10-Y12) ----------
const uint8_t PIN_ST0 = 10;     // status code bit 0
const uint8_t PIN_ST1 = 11;     // status code bit 1
const uint8_t PIN_ST2 = 12;     // status code bit 2

// MOSFET circuit: Arduino HIGH = PLC input ON.  Divider: PLC output ON = pin HIGH.
// (If you switch to optocouplers on the PLC -> Arduino side, set IN_ACTIVE_LOW = true.)
const bool OUT_ACTIVE_LOW = false;
const bool IN_ACTIVE_LOW  = false;

const unsigned long PULSE_MS  = 300;  // START pulse length (PLC scan time is far shorter)
const unsigned long SETTLE_MS = 100;  // let MILK/SUGAR settle before START
const unsigned long STABLE_MS = 80;   // status bits must hold still this long before we trust them

uint8_t stableCode = 0xFF;   // last announced status code (0xFF = nothing announced yet)
uint8_t lastRaw    = 0xFF;
unsigned long rawChanged = 0;

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
    case 2:
    case 3:
    case 4:
    case 5:
      if (prev < 2 || prev > 5) Serial.println(F("STATE:BREW"));
      Serial.print(F("POS:")); Serial.println(code - 1);
      break;
    case 6: Serial.println(F("STATE:PICKUP")); break;
    default: Serial.println(F("ERROR:PLC_STATUS")); break;  // the page ignores unknown lines
  }
}

void pollStatus() {
  uint8_t raw = readStatusRaw();
  if (raw != lastRaw) { lastRaw = raw; rawChanged = millis(); }
  if (millis() - rawChanged >= STABLE_MS && raw != stableCode) {
    uint8_t prev = stableCode;
    stableCode = raw;
    // New cup session (idle or fresh menu): start from "no milk, no sugar"
    if (raw == 0 || raw == 1) {
      out(PIN_MILK,  false);
      out(PIN_SUGAR, false);
    }
    announce(raw, prev);
  }
}

// ---------- Page -> PLC ----------
void pulse(uint8_t pin) {
  out(pin, true);
  delay(PULSE_MS);
  out(pin, false);
}

// ---------- Arduino entry points ----------
void setup() {
  Serial.begin(9600);

  initOut(PIN_MILK);
  initOut(PIN_SUGAR);
  initOut(PIN_START);

  // Active-low inputs (optocouplers) use the internal pull-up.
  // Active-high inputs (resistor divider with its own pull-down) must not.
  uint8_t inMode = IN_ACTIVE_LOW ? INPUT_PULLUP : INPUT;
  pinMode(PIN_ST0, inMode);
  pinMode(PIN_ST1, inMode);
  pinMode(PIN_ST2, inMode);
}

void loop() {
  if (readLine()) {
    // Commands are only accepted while the PLC is in the menu state.
    // MILK and SUGAR are levels: they stay set until the PLC latches them on START.
    if (stableCode == 1) {
      if      (strncmp(lineBuf, "MILK:", 5) == 0)  out(PIN_MILK,  lineBuf[5] == '1');
      else if (strncmp(lineBuf, "SUGAR:", 6) == 0) out(PIN_SUGAR, lineBuf[6] == '1');
      else if (strcmp(lineBuf, "START") == 0)    { delay(SETTLE_MS); pulse(PIN_START); }
    }
  }
  pollStatus();
}
