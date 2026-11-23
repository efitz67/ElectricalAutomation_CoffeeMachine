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

void pollStatus() {
  uint8_t raw = readStatusRaw();
  if (raw != lastRaw) { lastRaw = raw; rawChanged = millis(); }
  if (millis() - rawChanged >= STABLE_MS && raw != stableCode) {
    uint8_t prev = stableCode;
    stableCode = raw;
    // New cup session (idle or fresh menu): start from "no sugar, no milk"
    if (raw == 0 || raw == 1) {
      out(PIN_SUGAR, false);
      out(PIN_MILK,  false);
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

  initOut(PIN_SUGAR);
  initOut(PIN_MILK);
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
    // SUGAR and MILK are levels: they stay set until the cup session ends.
    if (stableCode == 1) {
      if      (strncmp(lineBuf, "SUGAR:", 6) == 0) out(PIN_SUGAR, lineBuf[6] == '1');
      else if (strncmp(lineBuf, "MILK:", 5) == 0)  out(PIN_MILK,  lineBuf[5] == '1');
      else if (strcmp(lineBuf, "START") == 0)    { delay(SETTLE_MS); pulse(PIN_START); }
    }
  }
  pollStatus();
}
