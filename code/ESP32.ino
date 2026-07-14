#define BLYNK_TEMPLATE_ID "TMPL6OhTs9eFC"
#define BLYNK_TEMPLATE_NAME "Smart Parking System"
#define BLYNK_AUTH_TOKEN "4HMu3irTljjaA1I8PbDmwCxNW_etIRkT"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <BlynkSimpleEsp32.h>
#include <NTPClient.h>

char ssid[] = "SE203714";
char pass[] = "DungLTSE203714";

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7 * 3600, 60000);

String inputBuffer = "";

void setup() {
    Serial.begin(115200);
    Serial2.begin(9600, SERIAL_8N1, 16, 17);

    Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
    timeClient.begin();
}

void loop() {
    Blynk.run();
    timeClient.update();

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
}

void processData(String data) {
    Serial.println("Received: " + data);

    int firstComma = data.indexOf(',');
    if (firstComma == -1) return;

    String type = data.substring(0, firstComma);
    String remaining = data.substring(firstComma + 1);

    if (type == "SLOT") {
        int secondComma = remaining.indexOf(',');
        if (secondComma == -1) return;

        int occupied = remaining.substring(0, secondComma).toInt();
        int available = remaining.substring(secondComma + 1).toInt();

        Blynk.virtualWrite(0, available);

        Serial.print("Occupied: ");
        Serial.println(occupied);
        Serial.print("Available: ");
        Serial.println(available);
    }
    else if (type == "IN") {
        Blynk.virtualWrite(1, remaining);

        String currentTime = timeClient.getFormattedTime();
        Blynk.logEvent("vehicle_entered", "[" + currentTime + "] RFID: " + remaining);
    }
    else if (type == "OUT") {
        Blynk.virtualWrite(1, remaining);

        String currentTime = timeClient.getFormattedTime();
        Blynk.logEvent("vehicle_exited", "[" + currentTime + "] RFID: " + remaining);
    }
}