#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

Servo barrierServo;
#define SERVO_PIN          9
#define SERVO_CLOSED_ANGLE 0
#define SERVO_OPEN_ANGLE   45
#define SERVO_OPEN_TIME_MS 4000UL

#define TRIG_ENTRY 3
#define ECHO_ENTRY 2
#define TRIG_EXIT  5
#define ECHO_EXIT  4

#define TRIG_SLOT1 6
#define ECHO_SLOT1 7
#define TRIG_SLOT2 10
#define ECHO_SLOT2 11
#define TRIG_SLOT3 12
#define ECHO_SLOT3 13

#define NUM_SLOTS 3
const int SLOT_TRIG[NUM_SLOTS] = {TRIG_SLOT1, TRIG_SLOT2, TRIG_SLOT3};
const int SLOT_ECHO[NUM_SLOTS] = {ECHO_SLOT1, ECHO_SLOT2, ECHO_SLOT3};
bool slotOccupied[NUM_SLOTS] = {false, false, false};
int lastAvailable = -1;
int lastOccupied  = -1;

#define VEHICLE_DETECT_CM 2
#define SLOT_DETECT_CM     2

#define NUM_CARDS 4
struct CardInfo {
  const char* uid;
  bool isInside;
};

CardInfo cards[NUM_CARDS] = {
  {"E9551B06", false},
  {"23691805", false},
  {"7C8C1B06", false},
  {"A7601B06", false}
};

enum GateMode { GATE_IDLE, WAIT_CARD_ENTRY, WAIT_CARD_EXIT };
GateMode gateMode = GATE_IDLE;

unsigned long modeStartTime = 0;
const unsigned long WAIT_CARD_TIMEOUT_MS = 15000UL;

String serialBuffer = "";

void setup() {
  Serial.begin(9600);

  lcd.init();
  lcd.backlight();

  barrierServo.attach(SERVO_PIN);
  barrierServo.write(SERVO_CLOSED_ANGLE);

  pinMode(TRIG_ENTRY, OUTPUT); pinMode(ECHO_ENTRY, INPUT);
  pinMode(TRIG_EXIT,  OUTPUT); pinMode(ECHO_EXIT,  INPUT);
  for (int i = 0; i < NUM_SLOTS; i++) {
    pinMode(SLOT_TRIG[i], OUTPUT);
    pinMode(SLOT_ECHO[i], INPUT);
  }

  updateSlotStatus();
  sendSlotStatusIfChanged();
}

void loop() {
  readSerialFromESP32();

  updateSlotStatus();
  sendSlotStatusIfChanged();

  switch (gateMode) {
    case GATE_IDLE:
      showIdleScreen();
      checkForApproachingVehicle();
      break;

    case WAIT_CARD_ENTRY:
    case WAIT_CARD_EXIT:
      if (millis() - modeStartTime > WAIT_CARD_TIMEOUT_MS) {
        gateMode = GATE_IDLE;
      }
      break;
  }
}

long readDistanceCM(int trigPin, int echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000UL);
  if (duration == 0) return 999;
  return duration * 0.0343 / 2;
}

void updateSlotStatus() {
  for (int i = 0; i < NUM_SLOTS; i++) {
    long d = readDistanceCM(SLOT_TRIG[i], SLOT_ECHO[i]);
    slotOccupied[i] = (d <= SLOT_DETECT_CM);
    delay(15);
  }
}

int getAvailableSlots() {
  int count = 0;
  for (int i = 0; i < NUM_SLOTS; i++) if (!slotOccupied[i]) count++;
  return count;
}

void sendSlotStatusIfChanged() {
  int occupiedCount = 0;
  for (int i = 0; i < NUM_SLOTS; i++) if (slotOccupied[i]) occupiedCount++;
  int availableCount = NUM_SLOTS - occupiedCount;

  if (occupiedCount != lastOccupied || availableCount != lastAvailable) {
    lastOccupied  = occupiedCount;
    lastAvailable = availableCount;

    Serial.print("SLOT,");
    Serial.print(occupiedCount);
    Serial.print(",");
    Serial.println(availableCount);

    for (int i = 0; i < NUM_SLOTS; i++) {
      Serial.print("SLOTSTATUS,");
      Serial.print(i + 1);
      Serial.print(",");
      Serial.println(slotOccupied[i] ? "Unavailable" : "Available");
    }
  }
}

void showIdleScreen() {
  static unsigned long lastUpdate = 0;
  if (millis() - lastUpdate < 500) return;
  lastUpdate = millis();

  lcd.setCursor(0, 0);
  lcd.print("Free Slot: ");
  lcd.print(getAvailableSlots());
  lcd.print("   ");
  lcd.setCursor(0, 1);
  lcd.print("                ");
}

void checkForApproachingVehicle() {
  long dEntry = readDistanceCM(TRIG_ENTRY, ECHO_ENTRY);
  delay(5);
  long dExit = readDistanceCM(TRIG_EXIT, ECHO_EXIT);

  if (dEntry <= VEHICLE_DETECT_CM) {
    gateMode = WAIT_CARD_ENTRY;
    modeStartTime = millis();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Scan Card: ");
  } else if (dExit <= VEHICLE_DETECT_CM) {
    gateMode = WAIT_CARD_EXIT;
    modeStartTime = millis();
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Scan Card: ");
  }
}

void readSerialFromESP32() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      serialBuffer.trim();
      if (serialBuffer.length() > 0) {
        processIncomingLine(serialBuffer);
      }
      serialBuffer = "";
    } else if (c != '\r') {
      serialBuffer += c;
    }
  }
}

void processIncomingLine(String line) {
  if (line.startsWith("CARD,")) {
    String uid = line.substring(5);
    uid.trim();
    uid.toUpperCase();
    handleCardScanned(uid);
  }
}

int findCardIndex(String uid) {
  for (int i = 0; i < NUM_CARDS; i++) {
    if (uid.equals(cards[i].uid)) return i;
  }
  return -1;
}

void handleCardScanned(String uid) {
  if (gateMode != WAIT_CARD_ENTRY && gateMode != WAIT_CARD_EXIT) {
    return;
  }

  int idx = findCardIndex(uid);
  if (idx == -1) {
    showDeny("Invalid Card Id");
    return;
  }

  if (gateMode == WAIT_CARD_ENTRY) {
    if (getAvailableSlots() <= 0) {
      showDeny("Full Slot");
      return;
    }
    if (cards[idx].isInside) {
      showDeny("Inside Already");
      return;
    }
    cards[idx].isInside = true;
    Serial.print("IN,");
    Serial.println(uid);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ID:");

    lcd.setCursor(0, 1);
    lcd.print(uid);

    openBarrier();
    waitVehicleClear(TRIG_ENTRY, ECHO_ENTRY);
    gateMode = GATE_IDLE;
  }
  else if (gateMode == WAIT_CARD_EXIT) {
    if (!cards[idx].isInside) {
      showDeny("Outside Already");
      return;
    }
    cards[idx].isInside = false;
    Serial.print("OUT,");
    Serial.println(uid);

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ID:");

    lcd.setCursor(0, 1);
    lcd.print(uid);

    openBarrier();
    waitVehicleClear(TRIG_EXIT, ECHO_EXIT);
    gateMode = GATE_IDLE;
  }
}

void openBarrier() {
  barrierServo.write(SERVO_OPEN_ANGLE);
  delay(SERVO_OPEN_TIME_MS);
  barrierServo.write(SERVO_CLOSED_ANGLE);
}

void waitVehicleClear(int trigPin, int echoPin) {
  unsigned long start = millis();
  while (millis() - start < 8000UL) {
    long d = readDistanceCM(trigPin, echoPin);
    if (d > VEHICLE_DETECT_CM) break;
    delay(100);
  }
}

void showDeny(String reason) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Access Deny");
  lcd.setCursor(0, 1);
  lcd.print(reason);
  delay(2500);
  gateMode = GATE_IDLE;
  lcd.clear();
}
