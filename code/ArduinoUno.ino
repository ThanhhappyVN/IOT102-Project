#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <SoftwareSerial.h>

SoftwareSerial espSerial(8, 9);
LiquidCrystal_I2C lcd(0x27, 16, 2);

#define SS_PIN 10
#define RST_PIN A1

MFRC522 rfid(SS_PIN, RST_PIN);
Servo gateServo;

#define SERVO_PIN A2
#define TRIG_IN 3
#define ECHO_IN 2
#define TRIG_OUT 5
#define ECHO_OUT 4

const int TOTAL_SLOT = 4;
int occupiedSlot = 0;
int availableSlot = TOTAL_SLOT;

const int DETECT_DISTANCE = 10;
const int SERVO_OPEN = 90;
const int SERVO_CLOSE = 0;

byte card1[4] = {0xE9, 0x55, 0x1B, 0x06};
byte card2[4] = {0x23, 0x69, 0x18, 0x05};
byte card3[4] = {0x7C, 0x8C, 0x1B, 0x06};
byte card4[4] = {0xA7, 0x60, 0x1B, 0x06};

enum ParkingState {
    IDLE,
    WAIT_ENTRY_CARD,
    WAIT_EXIT_CARD,
    GATE_OPENING,
    VEHICLE_PASSING
};

ParkingState state = IDLE;
unsigned long stateTimer = 0;
unsigned long closeTimer = 0;
String currentCard = "";

bool checkCard(String &id);
String getCardID();
long readDistance(int trigPin, int echoPin);
void openGate();
void closeGate();
void sendToESP(String data);
void showReady();
void showFull();
void showScan();
void showInvalid();
void showVehicleIn(String id);
void showVehicleOut(String id);

void setup() {
    Serial.begin(9600);
    espSerial.begin(9600);

    SPI.begin();
    rfid.PCD_Init();
    Wire.begin();

    lcd.init();
    lcd.backlight();

    gateServo.attach(SERVO_PIN);
    gateServo.write(SERVO_CLOSE);

    pinMode(TRIG_IN, OUTPUT);
    pinMode(ECHO_IN, INPUT);
    pinMode(TRIG_OUT, OUTPUT);
    pinMode(ECHO_OUT, INPUT);

    showReady();
}

void loop() {
    long inDistance = readDistance(TRIG_IN, ECHO_IN);
    long outDistance = readDistance(TRIG_OUT, ECHO_OUT);

    switch (state) {
        case IDLE:
            if (inDistance > DETECT_DISTANCE && outDistance > DETECT_DISTANCE) {
                static unsigned long lcdUpdateTimer = 0;
                if (millis() - lcdUpdateTimer > 2000) {
                    if (occupiedSlot >= TOTAL_SLOT) showFull();
                    else showReady();
                    lcdUpdateTimer = millis();
                }
            }

            if (inDistance <= DETECT_DISTANCE && occupiedSlot < TOTAL_SLOT) {
                state = WAIT_ENTRY_CARD;
                stateTimer = millis();
                showScan();
            }
            else if (outDistance <= DETECT_DISTANCE && occupiedSlot > 0) {
                state = WAIT_EXIT_CARD;
                stateTimer = millis();
                showScan();
            }
            break;

        case WAIT_ENTRY_CARD:
            if (millis() - stateTimer > 10000 || inDistance > DETECT_DISTANCE) {
                state = IDLE;
                if (occupiedSlot >= TOTAL_SLOT) showFull();
                else showReady();
                break;
            }

            if (checkCard(currentCard)) {
                showVehicleIn(currentCard);
                occupiedSlot++;
availableSlot = TOTAL_SLOT - occupiedSlot;
                
                openGate();
                state = GATE_OPENING;
                
                sendToESP("IN," + currentCard);
                sendToESP("SLOT," + String(occupiedSlot) + "," + String(availableSlot));
                stateTimer = millis();
            } else if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
                showInvalid();
                if (occupiedSlot >= TOTAL_SLOT) showFull();
                else showReady();
                state = IDLE;
            }
            break;

        case WAIT_EXIT_CARD:
            if (millis() - stateTimer > 10000 || outDistance > DETECT_DISTANCE) {
                state = IDLE;
                if (occupiedSlot >= TOTAL_SLOT) showFull();
                else showReady();
                break;
            }

            if (checkCard(currentCard)) {
                showVehicleOut(currentCard);
                if (occupiedSlot > 0) occupiedSlot--;
                availableSlot = TOTAL_SLOT - occupiedSlot;
                
                openGate();
                state = GATE_OPENING;
                
                sendToESP("OUT," + currentCard);
                sendToESP("SLOT," + String(occupiedSlot) + "," + String(availableSlot));
                stateTimer = millis();
            } else if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
                showInvalid();
                if (occupiedSlot >= TOTAL_SLOT) showFull();
                else showReady();
                state = IDLE;
            }
            break;

        case GATE_OPENING:
            if (millis() - stateTimer > 1500) {
                state = VEHICLE_PASSING;
                closeTimer = 0;
            }
            break;

        case VEHICLE_PASSING:
            if (inDistance > DETECT_DISTANCE && outDistance > DETECT_DISTANCE) {
                if (closeTimer == 0) {
                    closeTimer = millis();
                }
                if (millis() - closeTimer >= 3000) {
                    closeGate();
                    state = IDLE;
                    currentCard = "";
                }
            } else {
                closeTimer = 0;
            }
            break;
    }
    delay(50);
}

long readDistance(int trigPin, int echoPin) {
    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    long duration = pulseIn(echoPin, HIGH, 20000);
    if (duration == 0) return 999;
    return duration * 0.034 / 2;
}

bool checkCard(String &id) {
    if (!rfid.PICC_IsNewCardPresent()) return false;
    if (!rfid.PICC_ReadCardSerial()) return false;

    byte *uid = rfid.uid.uidByte;
    bool match = (memcmp(uid, card1, 4) == 0 ||
                  memcmp(uid, card2, 4) == 0 ||
                  memcmp(uid, card3, 4) == 0 ||
                  memcmp(uid, card4, 4) == 0);

    if (match) {
id = getCardID();
        return true;
    }
    
    getCardID();
    return false;
}

String getCardID() {
    String id = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) id += "0";
        id += String(rfid.uid.uidByte[i], HEX);
        if (i != rfid.uid.size - 1) id += " ";
    }
    id.toUpperCase();
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return id;
}

void openGate() { gateServo.write(SERVO_OPEN); }
void closeGate() { gateServo.write(SERVO_CLOSE); }
void sendToESP(String data) { espSerial.println(data); Serial.println(data); }

void showReady() {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("System Ready");
    lcd.setCursor(0, 1); lcd.print("Available: "); lcd.print(availableSlot);
}
void showFull() {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Parking Full!");
    lcd.setCursor(0, 1); lcd.print("Available: 0");
}
void showScan() {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Available: "); lcd.print(availableSlot);
    lcd.setCursor(0, 1); lcd.print("Please Scan");
}
void showInvalid() {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("Invalid Card");
    delay(1500);
}
void showVehicleIn(String id) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(id);
    lcd.setCursor(0, 1); lcd.print("Vehicle IN");
}
void showVehicleOut(String id) {
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(id);
    lcd.setCursor(0, 1); lcd.print("Vehicle OUT");
}
