#include <Arduino.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>

void setup() {
  Serial.begin(115200);  // USB Serial to PC
  Serial2.begin(115200, SERIAL_8N1, 25, 26);  // UART2 to Raspberry Pi
  Serial.println("ESP32 ready.");
}

void loop() {
  // Handle data from Raspberry Pi (via Serial2)
  if (Serial2.available()) {
    String incoming = Serial2.readStringUntil('\n');
    incoming.trim();

    // Optionally parse JSON if needed
    JsonDocument doc;
    deserializeJson(incoming, doc);

    Serial.print("Received from Pi: ");
    Serial.println(incoming);

    // Respond to Pi
    //Serial2.print("ACK from ESP32: ");
    //Serial2.println(incoming);
  }

  // Handle data from PC (via USB Serial)
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    //Serial.print("ACK from Pi: ");
    //Serial.println(cmd);

    // Optionally send to Pi as well
    //Serial2.print("Received from ESP32: ");
    Serial2.println(cmd);
  }
}

