#define BLYNK_TEMPLATE_ID "TMPL6OhTs9eFC"
#define BLYNK_TEMPLATE_NAME "Smart Parking System"
#define BLYNK_AUTH_TOKEN "4HMu3irTljjaA1I8PbDmwCxNW_etIRkT"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <BlynkSimpleEsp32.h>
#include <NTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>

char ssid[] = "SE203714";
char pass[] = "DungLTSE203714";
const char* GOOGLE_SCRIPT_URL =
    "https://script.google.com/macros/s/AKfycbxyViGJ82TTvjRTxnkTJ_Q9PVzmft2IquiIaVVl9pCs6C9WBqR-qK55B-SOVH1otbBEww/exec";

#define RFID_SS_PIN  5
#define RFID_RST_PIN 22
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);

String lastUidSent = "";
unsigned long lastUidTime = 0;
const unsigned long UID_COOLDOWN_MS = 2000;

bool isUidAlreadyParked(String uid);
bool saveEntry(String uid, String time);
String getEntryTime(String uid);
bool sendToGoogleSheet(String uid, String entryTime, String exitTime);
bool isValidNumber(String s);
void processData(String data);
String urlEncode(String str);
String getFullTimestamp();
String getDisplayTime();
void addPendingSync(String uid, String entryTime, String exitTime);
void retryPendingSyncs();
void savePendingToNVS(int index);
void clearPendingInNVS(int index);
void loadPendingFromNVS();
String getUidString(MFRC522::Uid uid);
void checkRfidCard();

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);

String inputBuffer = "";

#define MAX_SLOTS 4

struct ParkingRecord {
    String uid;
    String entryTime;
    bool occupied;
};
ParkingRecord records[MAX_SLOTS];

#define MAX_PENDING 20
struct PendingSync {
    String uid;
    String entryTime;
    String exitTime;
    bool valid;
};
PendingSync pendingQueue[MAX_PENDING];
Preferences prefs;

unsigned long lastRetryAttempt = 0;
const unsigned long RETRY_INTERVAL_MS = 30000;

void setup() {
    Serial.begin(115200);
    Serial2.begin(9600, SERIAL_8N1, 16, 17);

    SPI.begin();
    rfid.PCD_Init();

    for (int i = 0; i < MAX_PENDING; i++) pendingQueue[i].valid = false;

    prefs.begin("parking", false);
    loadPendingFromNVS();

    Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
    timeClient.begin();
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
    } else {
        Blynk.run();
    }
    timeClient.update();

    checkRfidCard();

    while (Serial2.available()) {
        char c = Serial2.read();
        if (c == '\n') {
            inputBuffer.trim();
            if (inputBuffer.length() > 0) {
                processData(inputBuffer);
            }
            inputBuffer = "";
        } else if (c != '\r') {
            inputBuffer += c;
        }
    }

    if (millis() - lastRetryAttempt > RETRY_INTERVAL_MS) {
        lastRetryAttempt = millis();
        retryPendingSyncs();
    }
}

String getUidString(MFRC522::Uid uid) {
    String result = "";
    for (byte i = 0; i < uid.size; i++) {
        if (uid.uidByte[i] < 0x10) result += "0";
        result += String(uid.uidByte[i], HEX);
    }
    result.toUpperCase();
    return result;
}

void checkRfidCard() {
    if (!rfid.PICC_IsNewCardPresent()) return;
    if (!rfid.PICC_ReadCardSerial()) return;

    String uid = getUidString(rfid.uid);

    if (uid != lastUidSent || millis() - lastUidTime > UID_COOLDOWN_MS) {
        lastUidSent = uid;
        lastUidTime = millis();

        Serial.println("Card detected: " + uid);
        Serial2.println("CARD," + uid);
    }

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}

bool isUidAlreadyParked(String uid) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (records[i].occupied && records[i].uid == uid) {
            return true;
        }
    }
    return false;
}

bool saveEntry(String uid, String time) {
    if (isUidAlreadyParked(uid)) {
        Serial.println("Warning: UID " + uid + " already parked, ignoring duplicate IN.");
        return false;
    }

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!records[i].occupied) {
            records[i].uid = uid;
            records[i].entryTime = time;
            records[i].occupied = true;
            return true;
        }
    }

    Serial.println("Error: no free record slot for UID " + uid);
    Blynk.logEvent("storage_full", "No free record slot for card: " + uid);
    return false;
}

String getEntryTime(String uid) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (records[i].occupied && records[i].uid == uid) {
            return records[i].entryTime;
        }
    }
    return "";
}

void freeSlot(String uid) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (records[i].occupied && records[i].uid == uid) {
            records[i].occupied = false;
            records[i].uid = "";
            records[i].entryTime = "";
            return;
        }
    }
}

void addPendingSync(String uid, String entryTime, String exitTime) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pendingQueue[i].valid) {
            pendingQueue[i].uid = uid;
            pendingQueue[i].entryTime = entryTime;
            pendingQueue[i].exitTime = exitTime;
            pendingQueue[i].valid = true;
            savePendingToNVS(i);
            Serial.println("Queued for retry: " + uid);
            return;
        }
    }
    Serial.println("Error: pending sync queue full, dropping record for UID " + uid);
    Blynk.logEvent("sync_queue_full", "Lost sync record for card: " + uid);
}

void savePendingToNVS(int index) {
    String idx = String(index);
    prefs.putString(("u" + idx).c_str(), pendingQueue[index].uid);
    prefs.putString(("e" + idx).c_str(), pendingQueue[index].entryTime);
    prefs.putString(("x" + idx).c_str(), pendingQueue[index].exitTime);
    prefs.putBool(("v" + idx).c_str(), pendingQueue[index].valid);
}

void clearPendingInNVS(int index) {
    String idx = String(index);
    prefs.putBool(("v" + idx).c_str(), false);
}

void loadPendingFromNVS() {
    int restored = 0;
    for (int i = 0; i < MAX_PENDING; i++) {
        String idx = String(i);
        bool valid = prefs.getBool(("v" + idx).c_str(), false);
        if (valid) {
            pendingQueue[i].uid = prefs.getString(("u" + idx).c_str(), "");
            pendingQueue[i].entryTime = prefs.getString(("e" + idx).c_str(), "");
            pendingQueue[i].exitTime = prefs.getString(("x" + idx).c_str(), "");
            pendingQueue[i].valid = true;
            restored++;
        }
    }
    if (restored > 0) {
        Serial.println("Restored " + String(restored) + " pending record(s) from NVS after reset.");
    }
}

void retryPendingSyncs() {
    static int nextIndex = 0;

    for (int count = 0; count < MAX_PENDING; count++) {
        int i = (nextIndex + count) % MAX_PENDING;
        if (pendingQueue[i].valid) {
            bool ok = sendToGoogleSheet(pendingQueue[i].uid, pendingQueue[i].entryTime, pendingQueue[i].exitTime);
            if (ok) {
                Serial.println("Retry OK for UID " + pendingQueue[i].uid);
                pendingQueue[i].valid = false;
                clearPendingInNVS(i);
            } else {
                Serial.println("Retry still failing for UID " + pendingQueue[i].uid);
            }
            nextIndex = (i + 1) % MAX_PENDING;
            return;
        }
    }
}

String getFullTimestamp() {
    unsigned long epoch = timeClient.getEpochTime();
    time_t rawtime = (time_t)epoch;
    struct tm* ti = gmtime(&rawtime);

    char buf[25];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
              ti->tm_year + 1900, ti->tm_mon + 1, ti->tm_mday,
              ti->tm_hour, ti->tm_min, ti->tm_sec);
    return String(buf);
}

String getDisplayTime() {
    unsigned long epoch = timeClient.getEpochTime();
    time_t rawtime = (time_t)epoch;
    struct tm* ti = gmtime(&rawtime);

    char buf[20];
    snprintf(buf, sizeof(buf), "%02d:%02d %02d/%02d/%04d",
              ti->tm_hour, ti->tm_min,
              ti->tm_mday, ti->tm_mon + 1, ti->tm_year + 1900);
    return String(buf);
}

String urlEncode(String str) {
    String encoded = "";
    char c;
    char code0, code1;
    for (unsigned int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (isalnum(c)) {
            encoded += c;
        } else {
            code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
            c = (c >> 4) & 0xf;
            code0 = c + '0';
            if (c > 9) code0 = c - 10 + 'A';
            encoded += '%';
            encoded += code0;
            encoded += code1;
        }
    }
    return encoded;
}

bool sendToGoogleSheet(String uid, String entryTime, String exitTime) {
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    String url = String(GOOGLE_SCRIPT_URL);
    url += "?uid=" + urlEncode(uid);
    url += "&entry=" + urlEncode(entryTime);
    url += "&exit=" + urlEncode(exitTime);

    Serial.println("Sending to Google Sheet: " + url);

    http.begin(client, url);
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(5000);

    int httpCode = http.GET();
    bool success = false;

    if (httpCode > 0) {
        Serial.print("Sheet update OK, HTTP code: ");
        Serial.println(httpCode);
        success = (httpCode == HTTP_CODE_OK);
    } else {
        Serial.print("Sheet update FAILED: ");
        Serial.println(http.errorToString(httpCode));
    }

    http.end();
    return success;
}

bool isValidNumber(String s) {
    if (s.length() == 0) return false;
    for (unsigned int i = 0; i < s.length(); i++) {
        if (!isDigit(s.charAt(i))) return false;
    }
    return true;
}

void processData(String data) {
    Serial.println("Received: " + data);

    int firstComma = data.indexOf(',');
    if (firstComma == -1) {
        Serial.println("Warning: malformed data (no comma): " + data);
        return;
    }
    String type = data.substring(0, firstComma);
    String remaining = data.substring(firstComma + 1);

    if (type == "SLOT") {
        int secondComma = remaining.indexOf(',');
        if (secondComma == -1) {
            Serial.println("Warning: malformed SLOT data: " + remaining);
            return;
        }

        String occupiedStr = remaining.substring(0, secondComma);
        String availableStr = remaining.substring(secondComma + 1);

         if (!isValidNumber(occupiedStr) || !isValidNumber(availableStr)) {
            Serial.println("Warning: non-numeric SLOT data: " + remaining);
            return;
        }

        int occupied = occupiedStr.toInt();
        int available = availableStr.toInt();

        Blynk.virtualWrite(V0, available);

        Serial.print("Occupied: ");
        Serial.println(occupied);
        Serial.print("Available: ");
        Serial.println(available);
    }

    else if (type == "SLOTSTATUS") {
        int secondComma = remaining.indexOf(',');
        if (secondComma == -1) {
            Serial.println("Warning: malformed SLOTSTATUS data: " + remaining);
            return;
        }

        String slotNumStr = remaining.substring(0, secondComma);
        String status = remaining.substring(secondComma + 1);

        if (!isValidNumber(slotNumStr)) {
            Serial.println("Warning: non-numeric slot number: " + slotNumStr);
            return;
        }

        int slotNum = slotNumStr.toInt();
        int vPin = 2 + slotNum;

        String slotStatusMsg = status;
        Blynk.virtualWrite(vPin, slotStatusMsg);

        Serial.println("Slot " + slotNumStr + ": " + slotStatusMsg);
    }

    else if (type == "IN") {
        String sheetTime = getFullTimestamp();
        String displayTime = getDisplayTime();
        bool saved = saveEntry(remaining, sheetTime);

        Blynk.virtualWrite(V1, remaining);

        String message;
        if (saved) {
            message = "Entry Time: " + displayTime;
            Blynk.logEvent("vehicle_entered", "Card: " + remaining + "\nEntry: " + displayTime);
        } else {
            message = "Entry ignored (duplicate or full)";
        }
        Blynk.virtualWrite(V2, message);
    }

    else if (type == "OUT") {
        String sheetExitTime = getFullTimestamp();
        String displayExitTime = getDisplayTime();
        String entrySheetTime = getEntryTime(remaining);

        if (entrySheetTime == "") {
            Serial.println("Warning: OUT event for UID not currently parked: " + remaining);
            entrySheetTime = "Unknown";
            Blynk.logEvent("unknown_exit", "OUT without matching IN for card: " + remaining);
        }

        bool sheetOk = sendToGoogleSheet(remaining, entrySheetTime, sheetExitTime);

        if (!sheetOk) {
            Blynk.logEvent("sheet_sync_failed", "Failed to log card: " + remaining);
            addPendingSync(remaining, entrySheetTime, sheetExitTime);
        }

        freeSlot(remaining);

        Blynk.virtualWrite(V1, remaining);

        String message = "Exit Time: " + displayExitTime;
        Blynk.virtualWrite(V2, message);

    }
}
