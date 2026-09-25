#include <Wire.h>
void setup() {
Serial.begin(115200);
Wire.begin(21, 22); // SDA, SCL on most ESP32 boards
for (byte a = 1; a < 127; a++) {
Wire.beginTransmission(a);
if (Wire.endTransmission() == 0)
Serial.printf("device found at 0x%02X\n", a);
}
}
void loop() {}