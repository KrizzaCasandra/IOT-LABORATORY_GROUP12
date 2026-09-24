// Experiment 1 - ESP32 verification sketch
const int LDR_PIN = 34; // ADC1 channel, input only
const int LED_PIN = 2; // on-board LED on many boards
const int THRESHOLD = 1600; // 12-bit range is 0 to 4095
void setup() {
Serial.begin(115200);
pinMode(LED_PIN, OUTPUT);
analogReadResolution(12);
Serial.println("ESP32 ready");
}
void loop() {
int raw = analogRead(LDR_PIN); // 0 to 4095
float volts = raw * 3.3 / 4095.0;
digitalWrite(LED_PIN, raw < THRESHOLD ? HIGH : LOW);
Serial.printf("raw=%d volts=%.3f\n", raw, volts);
delay(500);
}

