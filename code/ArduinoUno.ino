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

bool cardStatus[4] = {false, false, false, false}; 

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

int checkCard(String &id, bool isEntry);
String getCardID();
long readDistance(int trigPin, int echoPin);
void openGate();
void closeGate();
void sendToESP(String data);
void showReady();
void showFull();
void showScan();
void showError(int errorCode);
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

            if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
                int cardResult = checkCard(currentCard, true);
                if (cardResult == 1) {
                    showVehicleIn(currentCard);
                    occupiedSlot++;
                    availableSlot = TOTAL_SLOT - occupiedSlot;
                    
                    openGate();
                    state = GATE_OPENING;
                    
                    sendToESP("IN," + currentCard);
                    sendToESP("SLOT," + String(occupiedSlot) + "," + String(availableSlot));
                    stateTimer = millis();
                } else {
                    showError(cardResult);
                    if (occupiedSlot >= TOTAL_SLOT) showFull();
                    else showReady();
                    state = IDLE;
                }
            }
            break;

        case WAIT_EXIT_CARD:
            if (millis() - stateTimer > 10000 || outDistance > DETECT_DISTANCE) {
                state = IDLE;
                if (occupiedSlot >= TOTAL_SLOT) showFull();
                else showReady();
                break;
            }

            if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
                int cardResult = checkCard(currentCard, false);
                if (cardResult == 1) {
                    showVehicleOut(currentCard);
                    if (occupiedSlot > 0) occupiedSlot--;
                    availableSlot = TOTAL_SLOT - occupiedSlot;
                    
                    openGate();
                    state = GATE_OPENING;
                    
                    sendToESP("OUT," + currentCard);
                    sendToESP("SLOT," + String(occupiedSlot) + "," + String(availableSlot));
                    stateTimer = millis();
                } else {
                    showError(cardResult);
                    if (occupiedSlot >= TOTAL_SLOT) showFull();
                    else showReady();
                    state = IDLE;
                }
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

int checkCard(String &id, bool isEntry) {
    byte *uid = rfid.uid.uidByte;
    int cardIndex = -1;

    if (memcmp(uid, card1, 4) == 0) cardIndex = 0;
    else if (memcmp(uid, card2, 4) == 0) cardIndex = 1;
    else if (memcmp(uid, card3, 4) == 0) cardIndex = 2;
    else if (memcmp(uid, card4, 4) == 0) cardIndex = 3;

    if (cardIndex == -1) {
        getCardID();
        return 2;
    }

    if (isEntry) {
        if (cardStatus[cardIndex]) {
            getCardID();
            return 3;
        }
        cardStatus[cardIndex] = true;
        id = getCardID();
        return 1;
    } else {
        if (!cardStatus[cardIndex]) {
            getCardID();
            return 4;
        }
        cardStatus[cardIndex] = false;
        id = getCardID();
        return 1;
    }
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
void showError(int errorCode) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Access Denied!");
    lcd.setCursor(0, 1);
    if (errorCode == 2) lcd.print("Unknown Card");
    else if (errorCode == 3) lcd.print("Already Inside");
    else if (errorCode == 4) lcd.print("Already Outside");
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