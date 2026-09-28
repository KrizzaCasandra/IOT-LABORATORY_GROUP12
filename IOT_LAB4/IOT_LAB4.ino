//PART A
 const int MOIST_PIN = 34;   // ADC1, input only

// Measured for this probe in Part A
const int RAW_DRY = 3100;   // reading in air
const int RAW_WET = 1350;   // reading in water

int readMoistureRaw(int n = 10) {
  // Average multiple readings to make the value steadier
  long sum = 0;

  for (int i = 0; i < n; i++) {
    sum += analogRead(MOIST_PIN);
    delay(5);
  }

  return sum / n;
}

float readMoisturePct() {
  int raw = readMoistureRaw();

  float pct = 100.0 * (RAW_DRY - raw) /
              (float)(RAW_DRY - RAW_WET);

  return constrain(pct, 0.0, 100.0);
}

void setup() {
  Serial.begin(115200);
  pinMode(MOIST_PIN, INPUT);

  Serial.println("Soil Moisture Sensor Test");
}

void loop() {
  int raw = readMoistureRaw();
  float moisture = readMoisturePct();

  Serial.print("Raw: ");
  Serial.print(raw);

  Serial.print(" | Moisture: ");
  Serial.print(moisture, 1);
  Serial.println("%");

  delay(1000);
}
 
