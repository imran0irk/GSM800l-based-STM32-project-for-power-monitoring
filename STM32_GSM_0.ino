/*
  STM32F103 (Blue Pill) + SIM800L/C
  Features:
  - Admin number stored in EEPROM emulation
  - Set admin via SMS: "SET 01711223344"
  - Optional confirm button requirement for SET
  - Power outage/restored alerts: SMS + missed call
  - Missed call from admin toggles relay (only if power present)
  - SMS commands from admin: ON / OFF / STATUS / SET <number>
*/

#include <Arduino.h>
#include <EEPROM.h>

// ------------------ Pins ----------------------
#define RELAY_PIN         PB12
#define POWER_SENSE_PIN   PA0     // analog input from opto/transistor circuit (0..3.3V)
#define CONFIRM_BTN_PIN   PB11    // optional, to GND (INPUT_PULLUP)
#define CONFIRM_BTN_LED   PB13    // LED indication SET mode

#define POWER_LED_PIN PB0   // POWER indicator
#define PUMP_LED_PIN  PB1   // LOAD indicator


// ------------------ Behavior ------------------
static const uint32_t MODEM_BAUD = 9600;

// Power detection threshold (tune for your circuit)
static const int POWER_THRESHOLD_ADC = 2000; // 0..4095 for STM32 ADC
static const uint32_t POWER_DEBOUNCE_MS = 2000;

// Missed call hang time (seconds)
static const uint32_t MISSED_CALL_SECONDS = 8;

// Optional: require confirm button press for SET command
static const bool REQUIRE_CONFIRM_BUTTON_FOR_SET = false;  /// OPTIONAL***

// ------------------ EEPROM layout ------------------

static const uint16_t EEPROM_MAGIC_ADDR = 0;
static const uint32_t EEPROM_MAGIC      = 0xA5C3F10D;
static const uint16_t EEPROM_ADMIN_ADDR = 8;   // store admin as fixed char[20]
static const uint8_t  ADMIN_MAX_LEN     = 20;

// ------------------ Globals -------------------------
HardwareSerial &MODEM = Serial2;

char adminNumber[ADMIN_MAX_LEN] = "19944832701"; // default if EEPROM empty

bool powerPresent = false;
bool pumpOn = false;

bool lastPowerSample = false;
uint32_t powerChangeCandidateAt = 0;

bool ringSeen = false;

bool dialing = false;
uint32_t hangupAt = 0;

uint32_t ringAt = 0; //2nd PATCH
bool clipHandledForThisRing = false; //2nd PATCH

//---------------- EEPRom -------------------------

uint32_t eepromReadU32(uint16_t addr) {
  uint32_t v = 0;
  v |= ((uint32_t)EEPROM.read(addr + 0)) << 0;
  v |= ((uint32_t)EEPROM.read(addr + 1)) << 8;
  v |= ((uint32_t)EEPROM.read(addr + 2)) << 16;
  v |= ((uint32_t)EEPROM.read(addr + 3)) << 24;
  return v;
}

void eepromWriteU32(uint16_t addr, uint32_t v) {
  EEPROM.update(addr + 0, (uint8_t)((v >> 0) & 0xFF));
  EEPROM.update(addr + 1, (uint8_t)((v >> 8) & 0xFF));
  EEPROM.update(addr + 2, (uint8_t)((v >> 16) & 0xFF));
  EEPROM.update(addr + 3, (uint8_t)((v >> 24) & 0xFF));
}

// write fixed-length null-terminated string
void eepromWriteFixedString(uint16_t addr, const char *s, uint8_t maxLen) {
  for (uint8_t i = 0; i < maxLen; i++) {
    uint8_t b = 0;
    if (s && s[i]) b = (uint8_t)s[i];
    EEPROM.update(addr + i, b);
    if (b == 0) {
      // fill remaining with zeros (optional, keeps clean)
      for (uint8_t j = i + 1; j < maxLen; j++) EEPROM.update(addr + j, 0);
      return;
    }
  }
  // ensure last byte is null
  EEPROM.update(addr + (maxLen - 1), 0);
}

void eepromReadFixedString(uint16_t addr, char *out, uint8_t maxLen) {
  for (uint8_t i = 0; i < maxLen; i++) {
    out[i] = (char)EEPROM.read(addr + i);
    if (out[i] == '\0') return;
  }
  out[maxLen - 1] = '\0';
}

// ------------------ Helpers ------------------
static inline bool isDigitChar(char c) { return c >= '0' && c <= '9'; }

// remove spaces, CR/LF, etc.
void trimInPlace(char *s) {
  if (!s) return;
  // trim leading
  int start = 0;
  while (s[start] && (s[start] == ' ' || s[start] == '\r' || s[start] == '\n' || s[start] == '\t')) start++;
  if (start > 0) memmove(s, s + start, strlen(s + start) + 1);
  // trim trailing
  int end = (int)strlen(s) - 1;
  while (end >= 0 && (s[end] == ' ' || s[end] == '\r' || s[end] == '\n' || s[end] == '\t')) {
    s[end] = '\0';
    end--;
  }
}

// Keep + optional, keep digits; drop other chars
bool sanitizePhone(const char *in, char *out, size_t outLen) {
  if (!in || !out || outLen < 4) return false;
  size_t j = 0;

  // allow leading '+'
  if (in[0] == '+') {
    out[j++] = '+';
  }

  for (size_t i = 0; in[i] && j < outLen - 1; i++) {
    if (isDigitChar(in[i])) out[j++] = in[i];
  }
  out[j] = '\0';

  // validate: must have at least 8 digits
  int digits = 0;
  for (size_t i = 0; out[i]; i++) if (isDigitChar(out[i])) digits++;
  return digits >= 8 && digits <= 15;
}

// Compare numbers by last N digits (helps when CLIP returns +countrycode)
bool numbersMatchLoose(const char *a, const char *b, int lastDigits = 10) {
  if (!a || !b) return false;

  char sa[ADMIN_MAX_LEN], sb[ADMIN_MAX_LEN];
  if (!sanitizePhone(a, sa, sizeof(sa))) return false;
  if (!sanitizePhone(b, sb, sizeof(sb))) return false;

  // collect digits only
  char da[ADMIN_MAX_LEN], db[ADMIN_MAX_LEN];
  int ia = 0, ib = 0;
  for (int i = 0; sa[i] && ia < (int)sizeof(da) - 1; i++) if (isDigitChar(sa[i])) da[ia++] = sa[i];
  da[ia] = '\0';
  for (int i = 0; sb[i] && ib < (int)sizeof(db) - 1; i++) if (isDigitChar(sb[i])) db[ib++] = sb[i];
  db[ib] = '\0';

  if (ia == 0 || ib == 0) return false;

  int na = min(lastDigits, ia);
  int nb = min(lastDigits, ib);
  if (na != nb) {
    // compare with min digits
    int n = min(na, nb);
    return strcmp(da + (ia - n), db + (ib - n)) == 0;
  }
  return strcmp(da + (ia - na), db + (ib - na)) == 0;
}

bool modemGetIncomingCallerFromCLCC(char *out, size_t outLen) {  //2nd PATCH
  out[0] = '\0';
  MODEM.print("AT+CLCC\r");

  uint32_t start = millis();
  char line[220];

  while (millis() - start < 2000) {
    if (readModemLine(line, sizeof(line))) {
      if (strncmp(line, "+CLCC:", 6) == 0) {
        // +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>[,<number>,...]
        const char *p = strchr(line, '\"');
        if (!p) continue;
        p++;
        const char *q = strchr(p, '\"');
        if (!q) continue;

        size_t n = (size_t)(q - p);
        if (n >= outLen) n = outLen - 1;
        strncpy(out, p, n);
        out[n] = '\0';
        return true;
      }
      if (strcmp(line, "OK") == 0) break;
      if (strcmp(line, "ERROR") == 0) break;
    }
  }
  return false;
}


// ========================= Functions ================================================

void updateStatusLEDs() {
  digitalWrite(POWER_LED_PIN, powerPresent ? HIGH : LOW);
  digitalWrite(PUMP_LED_PIN, pumpOn ? HIGH : LOW);
}

void relayWrite(bool on) {
  pumpOn = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  updateStatusLEDs();
}

// ------------------ EEPROM store/load ------------------
void eepromInit() {
  uint32_t magic = eepromReadU32(EEPROM_MAGIC_ADDR);

  if (magic != EEPROM_MAGIC) {
    // first-time init
    eepromWriteU32(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
    eepromWriteFixedString(EEPROM_ADMIN_ADDR, adminNumber, ADMIN_MAX_LEN);
    return;
  }

  char stored[ADMIN_MAX_LEN];
  eepromReadFixedString(EEPROM_ADMIN_ADDR, stored, ADMIN_MAX_LEN);

  // If stored looks empty/invalid, restore default
  if (stored[0] == '\0') {
    eepromWriteFixedString(EEPROM_ADMIN_ADDR, adminNumber, ADMIN_MAX_LEN);
    return;
  }

  // Keep your existing sanitizePhone() if you have it:
  char sanitized[ADMIN_MAX_LEN];
  if (sanitizePhone(stored, sanitized, sizeof(sanitized))) {
    strncpy(adminNumber, sanitized, sizeof(adminNumber));
    adminNumber[sizeof(adminNumber) - 1] = '\0';
  } else {
    eepromWriteFixedString(EEPROM_ADMIN_ADDR, adminNumber, ADMIN_MAX_LEN);
  }
}

void saveAdminNumber(const char *newNum) {
  strncpy(adminNumber, newNum, sizeof(adminNumber));
  adminNumber[sizeof(adminNumber) - 1] = '\0';
  eepromWriteFixedString(EEPROM_ADMIN_ADDR, adminNumber, ADMIN_MAX_LEN);
}

// ------------------ Modem AT helpers ------------------
bool modemWaitFor(const char *needle, uint32_t timeoutMs) {
  uint32_t start = millis();
  static char buf[200];
  size_t len = 0;
  memset(buf, 0, sizeof(buf));

  while (millis() - start < timeoutMs) {
    while (MODEM.available()) {
      char c = MODEM.read();
      if (len < sizeof(buf) - 1) buf[len++] = c;
      buf[len] = '\0';
      if (strstr(buf, needle)) return true;
      if (strstr(buf, "ERROR")) return false;
    }
    delay(1);
  }
  return false;
}

bool modemSendCmd(const char *cmd, const char *expect, uint32_t timeoutMs) {
  MODEM.print(cmd);
  MODEM.print("\r");
  if (!expect) return true;
  return modemWaitFor(expect, timeoutMs);
}

bool modemWaitForPrompt(char promptChar, uint32_t timeoutMs) {
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (MODEM.available()) {
      char c = MODEM.read();
      if (c == promptChar) return true;
    }
    delay(1);
  }
  return false;
}

bool modemSendSMS(const char *number, const char *text) {
  char num[ADMIN_MAX_LEN];
  if (!sanitizePhone(number, num, sizeof(num))) return false;

  char cmd[40];
  snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", num);
  MODEM.print(cmd);
  MODEM.print("\r");

  if (!modemWaitForPrompt('>', 5000)) return false;

  MODEM.print(text);
  MODEM.write(26); // Ctrl+Z
  // wait for OK / +CMGS
  return modemWaitFor("OK", 20000);
}

void modemDialMissedCall(const char *number) {
  if (dialing) return;

  char num[ADMIN_MAX_LEN];
  if (!sanitizePhone(number, num, sizeof(num))) return;

  char cmd[40];
  // Semicolon = voice call
  snprintf(cmd, sizeof(cmd), "ATD%s;", num);
  modemSendCmd(cmd, "OK", 5000);

  dialing = true;
  hangupAt = millis() + (MISSED_CALL_SECONDS * 1000UL);
}

void modemHangup() {
  modemSendCmd("ATH", "OK", 3000);
  dialing = false;
}

bool modemInit() {
  // Basic wake
  for (int i = 0; i < 5; i++) {
    if (modemSendCmd("AT", "OK", 1000)) break;
    delay(300);
  }

  modemSendCmd("ATE0", "OK", 1000);                  // echo off
  modemSendCmd("AT+CMGF=1", "OK", 2000);             // SMS text mode
  modemSendCmd("AT+CPMS=\"ME\",\"ME\",\"ME\"", "OK", 5000); // use SM for SIM storage & ME for modem storage
  modemSendCmd("AT+CLIP=1", "OK", 2000);             // caller ID
  modemSendCmd("AT+CNMI=2,2,0,0,0", "OK", 2000);     // push +CMTI on new SMS, SIM storage: 2,1,0,0,0; Route SMS to UART: 2,2,0,0,0

  // Optional: delete all SMS on boot (uncomment if desired)
  modemSendCmd("AT+CMGDA=\"DEL ALL\"", "OK", 10000);

  return true;
}

// ------------------ Modem line reader (URCs) ------------------
bool readModemLine(char *out, size_t outLen) {
  static char line[220];
  static size_t idx = 0;

  while (MODEM.available()) {
    char c = MODEM.read();

    if (c == '\r') continue;

    if (c == '\n') {
      if (idx == 0) continue;
      line[idx] = '\0';
      strncpy(out, line, outLen);
      out[outLen - 1] = '\0';
      idx = 0;
      return true;
    }

    if (idx < sizeof(line) - 1) line[idx++] = c;
  }
  return false;
}

// Parse +CLIP: "number",...
bool parseCLIPNumber(const char *line, char *out, size_t outLen) {
  const char *p = strchr(line, '\"');
  if (!p) return false;
  p++;
  const char *q = strchr(p, '\"');
  if (!q) return false;
  size_t n = (size_t)(q - p);
  if (n >= outLen) n = outLen - 1;
  strncpy(out, p, n);
  out[n] = '\0';
  return true;
}

// ------------------ SMS read/delete ------------------
bool modemReadSMS(int index, char *sender, size_t senderLen, char *msg, size_t msgLen) {
  sender[0] = '\0';
  msg[0] = '\0';

  char cmd[24];
  snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
  MODEM.print(cmd);
  MODEM.print("\r");

  // We will read lines until OK. Expect:
  // +CMGR: "REC UNREAD","+12345",...  then message line
  uint32_t start = millis();
  bool gotHeader = false;
  bool gotBody = false;

  char line[220];
  while (millis() - start < 8000) {
    if (readModemLine(line, sizeof(line))) {
      // Serial.println(line); // debug

      if (strstr(line, "+CMGR:")) {
        // find sender between quotes after the status field
        // +CMGR: "REC UNREAD","+12345",...
        const char *p = strstr(line, "\",\"");
        if (p) {
          p += 3;
          const char *q = strchr(p, '\"');
          if (q) {
            size_t n = (size_t)(q - p);
            if (n >= senderLen) n = senderLen - 1;
            strncpy(sender, p, n);
            sender[n] = '\0';
            gotHeader = true;
          }
        }
      } else if (!gotBody && gotHeader) {
        // First line after header is message body (your commands are single-line)
        strncpy(msg, line, msgLen);
        msg[msgLen - 1] = '\0';
        gotBody = true;
      } else if (strcmp(line, "OK") == 0) {
        break;
      } else if (strcmp(line, "ERROR") == 0) {
        break;
      }
    } else {
      delay(2);
    }
  }

  trimInPlace(sender);
  trimInPlace(msg);
  return gotHeader; // body might be empty but header means we got sender
}

void modemDeleteSMS(int index) {
  char cmd[20];
  snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
  modemSendCmd(cmd, "OK", 3000);
}

// ------------------ Status / Notifications ------------------
void sendStatusSMS(const char *reason) {
  char text[180];
  snprintf(text, sizeof(text),
           "%s\nPower: %s\nPump: %s\nAdmin: %s",
           reason,
           powerPresent ? "ON" : "OFF",
           pumpOn ? "ON" : "OFF",
           adminNumber);

  modemSendSMS(adminNumber, text);
}

void notifyPowerOff() {
  sendStatusSMS("Load Shedding: Power OFF. Pump turned OFF.");
  modemDialMissedCall(adminNumber);
}

void notifyPowerOn() {
  sendStatusSMS("Power Restored.");
  modemDialMissedCall(adminNumber);
}

void notifyPumpChanged(bool on) {
  if (on) sendStatusSMS("Pump turned ON.");
  else    sendStatusSMS("Pump turned OFF.");
  modemDialMissedCall(adminNumber);
}

// ------------------ Power sensing ------------------
bool readPowerNow() {
  int v = analogRead(POWER_SENSE_PIN);
  return v > POWER_THRESHOLD_ADC;
}

void updatePowerState() {
  bool sample = readPowerNow();

  if (sample != lastPowerSample) {
    lastPowerSample = sample;
    powerChangeCandidateAt = millis();
    return;
  }

  // stable long enough
  if (powerPresent != sample && (millis() - powerChangeCandidateAt) >= POWER_DEBOUNCE_MS) {
    powerPresent = sample;
    updateStatusLEDs();

    if (!powerPresent) {
      // force pump off on outage
      if (pumpOn) relayWrite(false);
      notifyPowerOff();
    } else {
      notifyPowerOn();
    }
  }
}

// ------------------ Command handling ------------------
bool confirmButtonPressed() {
  if (!REQUIRE_CONFIRM_BUTTON_FOR_SET) return true;
  return digitalRead(CONFIRM_BTN_PIN) == LOW;
}

void handleSetCommand(const char *newNumRaw) {
  if (!confirmButtonPressed()) {
    modemSendSMS(adminNumber, "SET rejected: press confirm button then resend SET <number>.");
    return;
  }

  char newNum[ADMIN_MAX_LEN];
  if (!sanitizePhone(newNumRaw, newNum, sizeof(newNum))) {
    modemSendSMS(adminNumber, "Invalid SET format. Use: SET 01711223344");
    return;
  }

  // Save & notify new admin
  saveAdminNumber(newNum);

  char msg[120];
  snprintf(msg, sizeof(msg), "Admin number updated successfully.\nNew admin: %s", adminNumber);
  modemSendSMS(adminNumber, msg);
}

void handleAuthorizedSMS(const char *body) {
  char buf[160];
  strncpy(buf, body, sizeof(buf));
  buf[sizeof(buf) - 1] = '\0';
  trimInPlace(buf);

  // Convert to uppercase for command compare (keep digits intact)
  for (size_t i = 0; buf[i]; i++) {
    if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 32);
  }

  if (strncmp(buf, "SET ", 4) == 0) {
    const char *numPart = body + 4; // use original case version for '+' etc.
    while (*numPart == ' ') numPart++;
    handleSetCommand(numPart);
    return;
  }

  if (strcmp(buf, "STATUS") == 0) {
    sendStatusSMS("Status:");
    return;
  }

  if (strcmp(buf, "ON") == 0) {
    if (!powerPresent) {
      modemSendSMS(adminNumber, "Cannot turn ON: Power is OFF.");
      return;
    }
    if (!pumpOn) {
      relayWrite(true);
      notifyPumpChanged(true);
    } else {
      modemSendSMS(adminNumber, "Pump is already ON.");
    }
    return;
  }

  if (strcmp(buf, "OFF") == 0) {
    if (pumpOn) {
      relayWrite(false);
      notifyPumpChanged(false);
    } else {
      modemSendSMS(adminNumber, "Pump is already OFF.");
    }
    return;
  }

  // Unknown command -> delete silently (or reply help)
  modemSendSMS(adminNumber, "Unknown command. Use: STATUS / ON / OFF / SET <number>");
}

void handleNewSMSIndex(int index) {
  char sender[40];
  char msg[160];

  bool ok = modemReadSMS(index, sender, sizeof(sender), msg, sizeof(msg));
  // Always delete to keep inbox clean
  modemDeleteSMS(index);

  if (!ok) return;

  // Reject non-admin senders
  if (!numbersMatchLoose(sender, adminNumber)) {
    // ignore completely
    return;
  }

  handleAuthorizedSMS(msg);
}

void togglePumpFromAuthorizedCall() {
  if (!powerPresent) {
    modemHangup();
    modemSendSMS(adminNumber, "Call command rejected: Power is OFF.");
    return;
  }
  relayWrite(!pumpOn);
  modemHangup();
  notifyPumpChanged(pumpOn);
}

// ------------------ URC processing ------------------

void handleModemLine(const char *line) {
  if (!line || !line[0]) return;

  if (strcmp(line, "RING") == 0) {
    // If we were dialing a feedback missed call, stop it so we can receive commands
    if (dialing) modemHangup();

    ringAt = millis();
    clipHandledForThisRing = false;
    return;
  }

  if (strncmp(line, "+CLIP:", 6) == 0) {
    char caller[40];
    if (parseCLIPNumber(line, caller, sizeof(caller))) {
      if (numbersMatchLoose(caller, adminNumber)) togglePumpFromAuthorizedCall(); // authorized: toggle
      else modemHangup();
    } else {
      modemHangup(); // unauthorized: hang up immediately
    }
    clipHandledForThisRing = true;
    ringAt = 0;
    return;
  }

  // Incoming call
  if (strcmp(line, "RING") == 0) {
    ringSeen = true;
    return;
  }

//  if (strncmp(line, "+CLIP:", 6) == 0) {
//    char caller[40];
//    if (parseCLIPNumber(line, caller, sizeof(caller))) {
//      if (numbersMatchLoose(caller, adminNumber)) {
//        // authorized: toggle
//        togglePumpFromAuthorizedCall();
//      } else {
//        // unauthorized: hang up immediately
//        modemHangup();
//      }
//    } else {
//      modemHangup();
//    }
//    ringSeen = false;
//    return;
//  }

  // Call ended indications
  if (strstr(line, "NO CARRIER") || strstr(line, "BUSY")) {
    dialing = false;
    ringSeen = false;
    return;
  }
}


// ------------------ Setup / Loop ------------------
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);

  pinMode(POWER_LED_PIN, OUTPUT);
  pinMode(PUMP_LED_PIN, OUTPUT);
  digitalWrite(POWER_LED_PIN, LOW);
  digitalWrite(PUMP_LED_PIN, LOW);


  pinMode(CONFIRM_BTN_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  delay(300);

  eepromInit();

  MODEM.begin(MODEM_BAUD);
  delay(1500);

  // ADC setup
  //analogReadResolution(12);

  // Initial power read
  lastPowerSample = readPowerNow();
  powerPresent = lastPowerSample;
  powerChangeCandidateAt = millis();

  modemInit();

  // Boot message
  sendStatusSMS("System online.");
}

void loop() {
  // 1) Debounced power state update + notifications
  updatePowerState();

  // 2) Handle missed call hangup timing
  if (dialing && (int32_t)(millis() - hangupAt) >= 0) {
    modemHangup();
  }

  // 3) Process modem URC lines
  char line[220];
  while (readModemLine(line, sizeof(line))) {
    // Optional debug:
    // Serial.print("MODEM> "); Serial.println(line);
    handleModemLine(line);
  }

  // If we got RING but no CLIP, fallback to CLCC after 1500ms
  if (ringAt && !clipHandledForThisRing && (millis() - ringAt) > 1500) {
    char caller[40];
    if (modemGetIncomingCallerFromCLCC(caller, sizeof(caller))) {
      if (numbersMatchLoose(caller, adminNumber)) togglePumpFromAuthorizedCall();
      else modemHangup();
    } else {
      modemHangup();
    }
    ringAt = 0;
  }

  delay(5);
}
