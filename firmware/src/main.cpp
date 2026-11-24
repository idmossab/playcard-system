// playcard-system — arcade reader firmware (Wokwi simulation, OFFLINE mode)
//
// One ESP32 does two jobs here:
//   1. READER  : reads RFID cards, decides, pulses the coin line (GPIO26).
//   2. MACHINE : SIMULATION ONLY — pretends to be the arcade machine that
//                counts coin pulses on GPIO4. On real hardware this part is
//                deleted: the real machine's coin input sits behind a PC817.

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------------- Settings ----------------
const char*         READER_ID   = "machine-1";
const int           COST_POINTS = 10;
const unsigned long PULSE_MS    = 100;    // coin pulse length
const unsigned long GAME_MS     = 10000;  // 10 s in sim = 10 min real
const unsigned long RESULT_MS   = 1500;   // how long a result stays on screen

// ---------------- Pins ----------------
// RC522 uses the ESP32's default SPI pins: SCK=18, MISO=19, MOSI=23.
const int PIN_RFID_SS  = 5;
const int PIN_RFID_RST = 27;
// OLED uses the default I2C pins: SDA=21, SCL=22.
const int PIN_BUZZER   = 25;
const int PIN_COIN_OUT = 26;  // HIGH = "coin" (drives PC817 LED / relay in sim)
const int PIN_COIN_IN  = 4;   // SIMULATION ONLY: machine's coin input
const int PIN_GAME_LED = 14;  // SIMULATION ONLY: "game running" lamp

MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);  // -1 = no reset pin

// ---------------- Offline accounts table ----------------
// UIDs are Wokwi's preset cards. Replaced by the PC backend later.
struct Account {
  const char* uid;
  const char* name;
  int         points;
  bool        blocked;
};

Account accounts[] = {
  { "01:02:03:04", "Blue",   100, false },
  { "11:22:33:44", "Green",   10, false },
  { "55:66:77:88", "Yellow",   0, false },
  { "AA:BB:CC:DD", "Red",     50, true  },
};

unsigned long resultShownAt = 0;  // 0 = showing READY screen

// ---------------- Small helpers ----------------

// Screen: one big line + two small lines.
void showScreen(const String& big, const String& line2 = "", const String& line3 = "") {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(2);           // 12x16 px per char -> ~10 chars per line
  oled.setCursor(0, 0);
  oled.println(big);
  oled.setTextSize(1);           // 6x8 px per char
  oled.setCursor(0, 30);
  oled.println(line2);
  oled.setCursor(0, 44);
  oled.println(line3);
  oled.display();                // nothing appears until display() is called
}

void showReady() {
  showScreen("READY", "Tap your card");
  resultShownAt = 0;
}

// Blocking beep: tone() makes a square wave on the pin (passive buzzer).
void beep(int freqHz, int ms) {
  tone(PIN_BUZZER, freqHz);
  delay(ms);
  noTone(PIN_BUZZER);
}

void beepOk()    { beep(2000, 80); }
void beepError() { beep(400, 150); delay(100); beep(400, 150); }

// One coin pulse on GPIO26. Like pressing the coin switch for PULSE_MS.
void coinPulse() {
  digitalWrite(PIN_COIN_OUT, HIGH);
  delay(PULSE_MS);
  digitalWrite(PIN_COIN_OUT, LOW);
}

// Card UID bytes -> "AA:BB:CC:DD" (uppercase, works for 4 or 7 byte UIDs).
String uidToString(const MFRC522::Uid& uid) {
  String s;
  char hex[3];
  for (byte i = 0; i < uid.size; i++) {
    if (i) s += ':';
    snprintf(hex, sizeof(hex), "%02X", uid.uidByte[i]);
    s += hex;
  }
  return s;
}

Account* findAccount(const String& uid) {
  for (Account& a : accounts) {
    if (uid == a.uid) return &a;
  }
  return nullptr;
}

// ---------------- The decision ----------------
// Everything about "may this card play?" lives here.
void handleTap(const String& uid) {
  Serial.printf("[READER] %s tap uid=%s\n", READER_ID, uid.c_str());

  // TODO: replace the local table with the PC backend:
  //   POST http://192.168.1.10:8000/api/tap
  //   body: { "reader_id": READER_ID, "card_uid": uid, "tap_id": <unique per tap> }
  //   The PC answers ok/unknown/blocked/recharge + points left; the PC does
  //   the deduction, not the ESP32. tap_id lets the PC ignore a retried request.
  Account* acc = findAccount(uid);

  if (!acc) {
    Serial.println("[READER] -> UNKNOWN card");
    showScreen("UNKNOWN", "Go to cashier");
    beepError();
  } else if (acc->blocked) {
    Serial.printf("[READER] -> BLOCKED (%s)\n", acc->name);
    showScreen("BLOCKED", "Go to cashier");
    beepError();
  } else if (acc->points < COST_POINTS) {
    Serial.printf("[READER] -> RECHARGE (%s has %d, cost %d)\n", acc->name, acc->points, COST_POINTS);
    showScreen("RECHARGE", "Points: " + String(acc->points), "Cost:   " + String(COST_POINTS));
    beepError();
  } else {
    acc->points -= COST_POINTS;
    Serial.printf("[READER] -> OK (%s) -%d, left %d, sending coin pulse\n", acc->name, COST_POINTS, acc->points);
    showScreen("OK -" + String(COST_POINTS), "Points left: " + String(acc->points));
    beepOk();
    coinPulse();
  }
  resultShownAt = millis();
}

// ================= SIMULATION ONLY: the arcade machine =================
// The relay (standing in for the PC817 transistor) pulls GPIO4 to GND
// while GPIO26 is HIGH. A falling edge on GPIO4 = one coin inserted.
volatile int           credits      = 0;
volatile unsigned long lastCoinEdge = 0;
bool                   gameRunning  = false;
unsigned long          gameStartedAt = 0;

// Interrupt routine: runs the instant GPIO4 goes HIGH->LOW. Keep it tiny.
// IRAM_ATTR puts it in fast RAM, which the ESP32 requires for interrupts.
void IRAM_ATTR onCoin() {
  unsigned long now = millis();
  if (now - lastCoinEdge > 30) credits++;  // 30 ms debounce: ignore contact bounce
  lastCoinEdge = now;
}

void machineLoop() {
  if (!gameRunning && credits > 0) {
    noInterrupts();  // credits is shared with the interrupt; change it safely
    credits--;
    int left = credits;
    interrupts();
    gameRunning = true;
    gameStartedAt = millis();
    digitalWrite(PIN_GAME_LED, HIGH);
    Serial.printf("[MACHINE] coin accepted, game started (credits left %d)\n", left);
  }
  if (gameRunning && millis() - gameStartedAt >= GAME_MS) {
    gameRunning = false;
    digitalWrite(PIN_GAME_LED, LOW);
    Serial.println("[MACHINE] game over");
  }
}
// ======================= end SIMULATION ONLY ===========================

void setup() {
  Serial.begin(115200);

  pinMode(PIN_COIN_OUT, OUTPUT);
  digitalWrite(PIN_COIN_OUT, LOW);

  SPI.begin();
  rfid.PCD_Init();

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[READER] OLED not found at 0x3C");
  }

  // SIMULATION ONLY
  pinMode(PIN_GAME_LED, OUTPUT);
  pinMode(PIN_COIN_IN, INPUT_PULLUP);  // idle HIGH, relay pulls it LOW
  attachInterrupt(digitalPinToInterrupt(PIN_COIN_IN), onCoin, FALLING);

  Serial.printf("[READER] %s started, cost %d points\n", READER_ID, COST_POINTS);
  showReady();
}

void loop() {
  machineLoop();  // SIMULATION ONLY

  if (resultShownAt) {
    // Result on screen: ignore any re-read of the same tap until it times out.
    if (millis() - resultShownAt >= RESULT_MS) showReady();
    return;
  }

  // New card in the field? Then read its UID.
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = uidToString(rfid.uid);
  rfid.PICC_HaltA();  // put the card to sleep so it isn't read again while held there
  handleTap(uid);
}
