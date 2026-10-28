 #include <Wire.h>
#include <Adafruit_BMP280.h>
#include <BH1750.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>
#include <SPI.h>
#include <SD.h>

#define DHT_PIN 4
#define DHT_TYPE DHT11

#define BMP_ADDR 0x76
#define BH1750_ADDR 0x23
#define LCD_ADDR 0x27

#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23

#define FLAG_TEMP 0x01
#define FLAG_HUM 0x02
#define FLAG_PRESSURE 0x04
#define FLAG_LUX 0x08

DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_BMP280 bmp;
BH1750 lightMeter(BH1750_ADDR);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

struct EnvRecord {
  uint32_t ts;
  float tempC;
  float humPct;
  float presHpa;
  float lux;
  uint8_t flags;
};

EnvRecord currentRecord;

unsigned long lastAcquisition = 0;
unsigned long lastDisplay = 0;

const unsigned long ACQUISITION_INTERVAL = 2000;
const unsigned long DISPLAY_INTERVAL = 2000;

int displayPage = 0;
bool sdReady = false;

uint8_t checkPlausibility(const EnvRecord& r) {
  uint8_t f = 0;

  if (isnan(r.tempC) || !(r.tempC > -20 && r.tempC < 70))
    f |= FLAG_TEMP;

  if (isnan(r.humPct) || !(r.humPct >= 0 && r.humPct <= 100))
    f |= FLAG_HUM;

  if (isnan(r.presHpa) || !(r.presHpa > 800 && r.presHpa < 1100))
    f |= FLAG_PRESSURE;

  if (isnan(r.lux) || !(r.lux >= 0 && r.lux < 100000))
    f |= FLAG_LUX;

  return f;
}

EnvRecord acquire() {
  EnvRecord r;

  r.ts = millis() / 1000;
  r.tempC = dht.readTemperature();
  r.humPct = dht.readHumidity();
  r.presHpa = bmp.readPressure() / 100.0F;
  r.lux = lightMeter.readLightLevel();
  r.flags = checkPlausibility(r);

  return r;
}

void writeHeaderOnce() {
  if (SD.exists("/env.csv"))
    return;

  File file = SD.open("/env.csv", FILE_WRITE);

  if (!file) {
    Serial.println("Failed to create env.csv");
    return;
  }

  file.println("ts_s,temp_C,hum_pct,pres_hPa,lux,flags");
  file.close();
}

void logRecord(const EnvRecord& r) {
  File file = SD.open("/env.csv", FILE_APPEND);

  if (!file) {
    Serial.println("Failed to open env.csv");
    return;
  }

  file.printf(
    "%lu,%.2f,%.2f,%.2f,%.1f,%u\n",
    r.ts,
    r.tempC,
    r.humPct,
    r.presHpa,
    r.lux,
    r.flags
  );

  file.close();

  Serial.println("Record saved to SD.");
}

void displayRecord(const EnvRecord& r) {
  lcd.clear();

  if (displayPage == 0) {
    lcd.setCursor(0, 0);
    lcd.print("Temperature");
    lcd.setCursor(0, 1);

    if (r.flags & FLAG_TEMP)
      lcd.print("SUSPECT");
    else {
      lcd.print(r.tempC, 1);
      lcd.print(" C");
    }
  }

  else if (displayPage == 1) {
    lcd.setCursor(0, 0);
    lcd.print("Humidity");
    lcd.setCursor(0, 1);

    if (r.flags & FLAG_HUM)
      lcd.print("SUSPECT");
    else {
      lcd.print(r.humPct, 1);
      lcd.print(" %");
    }
  }

  else if (displayPage == 2) {
    lcd.setCursor(0, 0);
    lcd.print("Pressure");
    lcd.setCursor(0, 1);

    if (r.flags & FLAG_PRESSURE)
      lcd.print("SUSPECT");
    else {
      lcd.print(r.presHpa, 1);
      lcd.print(" hPa");
    }
  }

  else {
    lcd.setCursor(0, 0);
    lcd.print("Light");
    lcd.setCursor(0, 1);

    if (r.flags & FLAG_LUX)
      lcd.print("SUSPECT");
    else {
      lcd.print(r.lux, 1);
      lcd.print(" lux");
    }
  }

  displayPage = (displayPage + 1) % 4;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);

  dht.begin();

  lcd.init();
  lcd.backlight();

  if (!bmp.begin(BMP_ADDR))
    Serial.println("BMP280 not found.");

  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE))
    Serial.println("BH1750 not found.");

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (SD.begin(SD_CS)) {
    sdReady = true;
    writeHeaderOnce();
    Serial.println("SD initialized.");
  } else {
    Serial.println("SD initialization failed.");
  }
}

void loop() {
  unsigned long now = millis();

  if (now - lastAcquisition >= ACQUISITION_INTERVAL) {
    lastAcquisition = now;

    currentRecord = acquire();

    Serial.print("Time: ");
    Serial.print(currentRecord.ts);
    Serial.print(" s | Temp: ");
    Serial.print(currentRecord.tempC, 2);
    Serial.print(" C | Humidity: ");
    Serial.print(currentRecord.humPct, 2);
    Serial.print(" % | Pressure: ");
    Serial.print(currentRecord.presHpa, 2);
    Serial.print(" hPa | Light: ");
    Serial.print(currentRecord.lux, 1);
    Serial.print(" lux | Flags: 0x");
    Serial.println(currentRecord.flags, HEX);

    if (sdReady)
      logRecord(currentRecord);
  }

  if (now - lastDisplay >= DISPLAY_INTERVAL) {
    lastDisplay = now;
    displayRecord(currentRecord);
  }
}
