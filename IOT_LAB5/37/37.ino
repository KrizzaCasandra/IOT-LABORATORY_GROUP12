
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID    "a1b2c3d4-0001-4000-8000-00805f9b34fb"
#define CH_STATE_UUID   "a1b2c3d4-0002-4000-8000-00805f9b34fb" // Read + Notify
#define CH_VALUES_UUID   "a1b2c3d4-0003-4000-8000-00805f9b34fb" // Read + Notify
#define CH_SETPT_UUID   "a1b2c3d4-0004-4000-8000-00805f9b34fb" // Read + Write + Notify
#define CH_CMD_UUID    "a1b2c3d4-0005-4000-8000-00805f9b34fb" // Write



// --- NETWORK CREDENTIALS & API ---

const char* WIFI_SSID  = "KC";
const char* WIFI_PASSWORD = "realniggaakoniwends";

String FORECAST_URL = "http://api.openweathermap.org/data/2.5/forecast?q=Manila&appid=bd5e378503939ddaee76f12ad7a97608&units=metric";

const unsigned long FETCH_MS = 30UL * 60UL * 1000UL;       // Fetch weather every 30 mins
const unsigned long FORECAST_MAX_AGE = 3UL * 60UL * 60UL * 1000UL; // 3-hour staleness limit

const byte KEYPAD_ROWS[4] = {19, 18, 5, 17};
const byte KEYPAD_COLS[4] = {16, 4, 0, 2};

const int PUMP_PIN = 23;
const int VALVE_PIN = 22;

const int LED_PIN  = 33;
const int LED_OPENING = 13;
const int LED_RUNNING = 12;
const int LED_CLOSING = 14;

const int BUZZ_PIN = 25;
const int MOIST_PIN = 32;
const int RAIN_PIN = 35;
const int FLOAT_PIN = 39;

const int RAW_DRY = 4095;
const int RAW_WET = 1400;
const int RAIN_THRESHOLD = 2500;
const float MOIST_MARGIN = 8.0;                 
const float RAIN_HOLD_PCT = 60.0;                
const unsigned long HARD_MAX_MS = 20UL * 60UL * 1000UL;     
const bool FLOAT_ACTIVE_HIGH = true;

// --- ENUMS & STRUCTURES ---

enum FarmState { S_HOLD, S_IRRIGATE, S_TANK_EMPTY, S_RAIN, S_FORECAST_HOLD };
enum PumpPhase { PH_IDLE, PH_OPENING, PH_RUNNING, PH_CLOSING };
enum Alert { A_NONE, A_CONFIRM, A_WARN, A_ALARM };

char keys[4][4] = {

 {'1','2','3','A'},

 {'4','5','6','B'},

 {'7','8','9','C'},

 {'*','0','#','D'}

};

Keypad pad = Keypad(makeKeymap(keys), (byte*)KEYPAD_ROWS, (byte*)KEYPAD_COLS, 4, 4);
LiquidCrystal_I2C lcd(0x27, 16, 2);
Preferences prefs;

// BLE Server & Characteristics

BLEServer* pServer = nullptr;
BLECharacteristic* chState = nullptr;
BLECharacteristic* chValues = nullptr;
BLECharacteristic* chSetpt = nullptr;
BLECharacteristic* chCmd = nullptr;
bool clientConnected = false;

// SINGLE AUTHORITATIVE STATE VARIABLES

int userRunMinutes = 5; // Range: 1 to 30 mins

int setpointPct = 40;  // Range: 10% to 90%

FarmState currentState = S_HOLD;
PumpPhase phase = PH_IDLE;
Alert alert = A_NONE;

float rainChance = 0.0;

unsigned long forecastAt = 0;
bool forecastOk = false;

const unsigned long SETTLE_MS = 1500;
unsigned long tPhase = 0;
unsigned long runStarted = 0;

bool cutoffTripped = false;
bool irrigatingMoistureState = false;

// Keypad UI Variables

char inputBuffer[4] = "";
byte inputLen = 0;
bool inMenuMode = false;
int currentSetting = 0; // 0 = Run Minutes, 1 = Setpoint Pct
unsigned long lastKeypress = 0;

// Buzzer Variables

unsigned long tBuzz = 0;
int beep = 0;

// Task Timers

unsigned long tSense = 0;
unsigned long tUI = 0;
unsigned long tBleNotify = 0;

// Forward Declarations

void saveSettings();
void refreshDisplay();
float readMoisturePct();
bool isRainingLocally();
bool tankHasWater();
bool forecastFresh();


// PART E: SINGLE AUTHORITATIVE STATE SYNCHRONIZATION FUNCTION

void onSettingChanged() {

 // 1. Save authoritative values to NVM so state survives power loss

 saveSettings();

 // 2. Redraw local hardware display immediately

 refreshDisplay();

 // 3. Push real-time notification to BLE client if connected

 if (clientConnected && chSetpt != nullptr) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", setpointPct);
  chSetpt->setValue(buf);
  chSetpt->notify();
  Serial.printf("[STATE SYNC] Setpoint pushed over BLE: %d%%\n", setpointPct);
 }

}

// BLE LINK SERVER CALLBACKS 

class ServerCallbacks: public BLEServerCallbacks {
 void onConnect(BLEServer* pServer) override {
  clientConnected = true;
  Serial.println("[BLE] Mobile Client Connected!");

 }
 void onDisconnect(BLEServer* pServer) override {
  clientConnected = false;
  Serial.println("[BLE] Client Disconnected. Restarting Advertising...");
  BLEDevice::startAdvertising();

 }

};



// BLE WRITE CALLBACK: SETPOINT CHARACTERISTIC 

class SetptCallbacks: public BLECharacteristicCallbacks {
 void onWrite(BLECharacteristic *pCharacteristic) override {
  String rxValue = pCharacteristic->getValue();
  rxValue.trim();

  if (rxValue.length() > 0) {
   int val = rxValue.toInt();
   if (val >= 10 && val <= 90) { // Range check identical to Keypad
    setpointPct = val;     // Mutate the ONE authoritative copy
    Serial.printf("[BLE WRITE] New Setpoint Received: %d%%\n", setpointPct);
    onSettingChanged();    // Execute single state sync pipeline

   } else {
    Serial.printf("[BLE WRITE ERROR] Rejected Setpoint (%d%%). Must be 10-90%%\n", val);
    char spBuf[16];
    snprintf(spBuf, sizeof(spBuf), "%d", setpointPct);
    pCharacteristic->setValue(spBuf); // Echo authoritative value back

   }
  }
 }
};



// BLE WRITE CALLBACK: COMMAND CHARACTERISTIC 

class CmdCallbacks: public BLECharacteristicCallbacks {
 void onWrite(BLECharacteristic *pCharacteristic) override {
  String rxValue = pCharacteristic->getValue();
  rxValue.trim();

  

  if (rxValue.length() > 0) {
   Serial.printf("[BLE CMD] Remote Command Received: %s\n", rxValue.c_str());
   if (rxValue == "RESET" || rxValue == "ACK" || rxValue == "*") {
    cutoffTripped = false;
    alert = A_NONE;
    Serial.println("[BLE CMD] Safety hard cutoff trip reset via BLE.");
    refreshDisplay();

   }

   else if (rxValue.startsWith("RUN:")) {
    int minVal = rxValue.substring(4).toInt();
    if (minVal >= 1 && minVal <= 30) {
     userRunMinutes = minVal; // Mutate the ONE authoritative copy
     Serial.printf("[BLE CMD] Updated System Run Time: %d minutes\n", userRunMinutes);
     onSettingChanged();  // Execute single state sync pipeline

    } else {
     Serial.printf("[BLE CMD ERROR] Invalid run time (%d min). Range: 1-30 min.\n", minVal);

    }

   }

   else if (rxValue == "FORCE_ON") {
    irrigatingMoistureState = true;
    Serial.println("[BLE CMD] Manual Force Irrigation Triggered.");

   }

   else if (rxValue == "FORCE_OFF") {
    irrigatingMoistureState = false;
    Serial.println("[BLE CMD] Manual Force Stop Triggered.");

   }

   else {
    Serial.printf("[BLE CMD REJECTED] Unrecognized Command: '%s'\n", rxValue.c_str());

   }

  }

 }

};

// BLE SERVICE INITIALIZATION

void startBle() {
 BLEDevice::init("group 6");
 pServer = BLEDevice::createServer();
 pServer->setCallbacks(new ServerCallbacks());
 BLEService* pService = pServer->createService(SERVICE_UUID);

 // 1. State Characteristic (Read + Notify)
 chState = pService->createCharacteristic(
  CH_STATE_UUID,
  BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY

 );
 chState->addDescriptor(new BLE2902());

 // 2. Sensor Values Characteristic (Read + Notify)

 chValues = pService->createCharacteristic(
  CH_VALUES_UUID,
  BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY

 );
 chValues->addDescriptor(new BLE2902());

 // 3. Setpoint Characteristic (Read + Write + Notify)
 chSetpt = pService->createCharacteristic(
  CH_SETPT_UUID,
  BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY

 );

 chSetpt->addDescriptor(new BLE2902());
 chSetpt->setCallbacks(new SetptCallbacks());

 // 4. Command Characteristic (Write)

 chCmd = pService->createCharacteristic(
  CH_CMD_UUID,
  BLECharacteristic::PROPERTY_WRITE

 );

 chCmd->setCallbacks(new CmdCallbacks());

 pService->start();

 BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
 pAdvertising->addServiceUUID(SERVICE_UUID);
 pAdvertising->setScanResponse(true);
 pAdvertising->setMinPreferred(0x06);
 pAdvertising->setMinPreferred(0x12);
 BLEDevice::startAdvertising();

 Serial.println("[BLE] Service Started. Advertising as 'group 6'");

}

//  BLE TELEMETRY PUSH SERVICE

void updateBleTelemetry() {
 if (!clientConnected) return;

 // 1. Format and Push System State

 String stateStr = "";
 if (cutoffTripped) {
  stateStr = "TRIPPED";

 } else {

  switch (currentState) {
   case S_HOLD:     stateStr = "HOLD"; break;
   case S_IRRIGATE:   stateStr = "IRRIGATE"; break;
   case S_TANK_EMPTY:  stateStr = "TANK_EMPTY"; break;
   case S_RAIN:     stateStr = "RAIN"; break;
   case S_FORECAST_HOLD: stateStr = "FCST_HOLD"; break;

  }

 }

 chState->setValue(stateStr.c_str());
 chState->notify();
 // 2. Format and Push Telemetry CSV String (Format: "M:<moist>,R:<rain>,T:<tank>,F:<fcst>")

 char valBuf[64];
 snprintf(valBuf, sizeof(valBuf), "M:%.1f,R:%d,T:%d,F:%d\n",
      readMoisturePct(),
      isRainingLocally() ? 1 : 0,
      tankHasWater() ? 1 : 0,
      forecastFresh() ? (int)rainChance : -1);
 chValues->setValue(valBuf);
 chValues->notify();

 // 3. Format and Push Current Setpoint

 char spBuf[16];
 snprintf(spBuf, sizeof(spBuf), "%d", setpointPct);
 chSetpt->setValue(spBuf);
 chSetpt->notify();

}

void saveSettings() {
 prefs.begin("irrig", false);
 prefs.putInt("run", userRunMinutes);
 prefs.putInt("sp", setpointPct);
 prefs.end();
 Serial.printf("[NVM] Saved Settings: RunTime=%d min, Setpoint=%d%%\n", userRunMinutes, setpointPct);

}

void loadSettings() {

 prefs.begin("irrig", true);
 userRunMinutes = prefs.getInt("run", 5);
 setpointPct = prefs.getInt("sp", 40);
 prefs.end();
 Serial.printf("[NVM] Loaded Settings: RunTime=%d min, Setpoint=%d%%\n", userRunMinutes, setpointPct);
}



// SENSOR FUNCTIONS 

float readMoisturePct() {
 long sum = 0;
 for (int i = 0; i < 10; i++) { sum += analogRead(MOIST_PIN); delayMicroseconds(200); }
 float raw = sum / 10.0;
 float pct = 100.0 * (RAW_DRY - raw) / (float)(RAW_DRY - RAW_WET);
 return constrain(pct, 0.0, 100.0);

}

bool isRainingLocally() {
 return analogRead(RAIN_PIN) < RAIN_THRESHOLD;

}

bool tankHasWater() {
 int raw = analogRead(FLOAT_PIN);
 return FLOAT_ACTIVE_HIGH ? (raw > 1000) : (raw < 1000);

}
bool forecastFresh() {
 return forecastOk && (millis() - forecastAt < FORECAST_MAX_AGE);

}

bool moistureCallsForWater() {
 float pct = readMoisturePct();
 if (!irrigatingMoistureState && pct < setpointPct) {
  irrigatingMoistureState = true;
 } else if (irrigatingMoistureState && pct > (setpointPct + MOIST_MARGIN)) {
  irrigatingMoistureState = false;
 }
 return irrigatingMoistureState;
}

// SYSTEM DECISION ENGINE 

FarmState decideState() {
 if (!tankHasWater()) return S_TANK_EMPTY;
 if (isRainingLocally()) return S_RAIN;
 if (!moistureCallsForWater()) return S_HOLD;
 if (forecastFresh() && rainChance >= RAIN_HOLD_PCT) {
  return S_FORECAST_HOLD;

 }
 return S_IRRIGATE;

}

// --- WI-FI & HTTP FORECAST FETCHING ---

void connectWiFi() {

 Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
 WiFi.mode(WIFI_STA);
 WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

 int retries = 0;

 while (WiFi.status() != WL_CONNECTED && retries < 15) {
  delay(500);
  Serial.print(".");
  retries++;

 }

 if (WiFi.status() == WL_CONNECTED) {
  Serial.println("\n[WIFI] Connected Successfully!");
 } else {
  Serial.println("\n[WIFI] Connection Failed! Operating in Offline Local Mode.");
 }

}

void fetchForecast() {

 if (WiFi.status() != WL_CONNECTED) {
  forecastOk = false;
  return;

 }

 HTTPClient http;
 http.begin(FORECAST_URL);
 int code = http.GET();

 if (code == 200) {

  DynamicJsonDocument* doc = new DynamicJsonDocument(8192);
  DeserializationError err = deserializeJson(*doc, http.getString());

  if (err == DeserializationError::Ok) {
   JsonArray list = (*doc)["list"];
   rainChance = list[0]["pop"].as<float>() * 100;
   forecastAt = millis();
   forecastOk = true;

  } else {
   forecastOk = false;

  }
  delete doc;

 } else {
  forecastOk = false;

 }

 http.end();

}

bool runTimeExpired() {

 if (phase != PH_RUNNING) return false;
 unsigned long elapsed = millis() - runStarted;

 if (elapsed >= HARD_MAX_MS) {
  if (!cutoffTripped) {
   cutoffTripped = true;
   Serial.println("[SAFETY ALERT] Hard limit exceeded!");

  }
  return true;

 }

 return elapsed >= ((unsigned long)userRunMinutes * 60000UL);
}


void serviceOutputs(bool wantRun) {

 unsigned long now = millis();

digitalWrite(LED_OPENING, LOW);
 digitalWrite(LED_RUNNING, LOW);
 digitalWrite(LED_CLOSING, LOW);



 switch (phase) {

  case PH_IDLE:

   // No irrigation operation
   digitalWrite(LED_OPENING, LOW);
   digitalWrite(LED_RUNNING, LOW);
   digitalWrite(LED_CLOSING, LOW);

   if (wantRun && !cutoffTripped) {
    digitalWrite(VALVE_PIN, HIGH);
    // Opening phase LED

    digitalWrite(LED_OPENING, HIGH);
    tPhase = now;
    phase = PH_OPENING;

   }

   break;

  case PH_OPENING:
  digitalWrite(LED_OPENING, HIGH);
   if (now - tPhase > SETTLE_MS) {
    digitalWrite(PUMP_PIN, HIGH);
    // Opening OFF, Running ON

    digitalWrite(LED_OPENING, LOW);
    digitalWrite(LED_RUNNING, HIGH);
    runStarted = now;
    phase = PH_RUNNING;

   }

   break;

  case PH_RUNNING:
  digitalWrite(LED_OPENING, LOW);
   digitalWrite(LED_RUNNING, HIGH);
   digitalWrite(LED_CLOSING, LOW);



   if (!wantRun || runTimeExpired()) {

    digitalWrite(PUMP_PIN, LOW);

    // Running OFF, Closing ON
    digitalWrite(LED_RUNNING, LOW);
    digitalWrite(LED_CLOSING, HIGH);
    tPhase = now;
    phase = PH_CLOSING;

   }
   break;

    digitalWrite(LED_OPENING, LOW);
   digitalWrite(LED_RUNNING, LOW);
   digitalWrite(LED_CLOSING, HIGH);

   if (now - tPhase > SETTLE_MS) {
    digitalWrite(VALVE_PIN, LOW);

    // Closing OFF
    digitalWrite(LED_CLOSING, LOW);
    phase = PH_IDLE;
   }

   break;

 }

 if (currentState == S_TANK_EMPTY || cutoffTripped) {

  alert = A_ALARM;

  digitalWrite(LED_PIN, (now % 200) < 100);
 } else if (phase == PH_RUNNING) {
  alert = A_NONE;
  digitalWrite(LED_PIN, HIGH);

 } else {
  alert = A_NONE;
  digitalWrite(LED_PIN, LOW);
 }

 switch (alert) {

  case A_NONE:
   digitalWrite(BUZZ_PIN, LOW);
   break;

  case A_CONFIRM:
   if (beep == 0) { digitalWrite(BUZZ_PIN, HIGH); tBuzz = now; beep = 1; }
   else if (now - tBuzz > 120) { 
    digitalWrite(BUZZ_PIN, LOW); alert = A_NONE; beep = 0; 
    }
   break;

  case A_WARN:
   digitalWrite(BUZZ_PIN, (now % 2000) < 200);
   break;

  case A_ALARM:
   digitalWrite(BUZZ_PIN, (now % 200) < 100);
   break;

 }

}



// --- KEYPAD & UI ---

void updateLCDMenuView() {

 lcd.clear();
 lcd.setCursor(0, 0);

 if (currentSetting == 0) {

  lcd.print("1.RunMin (1-30)");
  lcd.setCursor(0, 1);
  lcd.print("Cur:"); lcd.print(userRunMinutes); lcd.print("m");

 } else {

  lcd.print("2.SetPct (10-90)");
  lcd.setCursor(0, 1);
  lcd.print("Cur:"); lcd.print(setpointPct); lcd.print("%");

 }

 lcd.print(" Val:"); lcd.print(inputBuffer);

}



void handleKeypad() {

 char key = pad.getKey();

 if (!key) return;

 inMenuMode = true;
 lastKeypress = millis();
 alert = A_CONFIRM;



 if (key >= '0' && key <= '9') {

  if (inputLen < 3) {

   inputBuffer[inputLen++] = key;
   inputBuffer[inputLen] = '\0';
   updateLCDMenuView();

  }

 }

 else if (key == '#') {

  if (inputLen > 0) {

   int val = atoi(inputBuffer);
   bool ok = false;

   if (currentSetting == 0 && val >= 1 && val <= 30) { userRunMinutes = val; ok = true; }
   else if (currentSetting == 1 && val >= 10 && val <= 90) { setpointPct = val; ok = true; }



   lcd.clear();

   if (ok) {

    onSettingChanged(); // Executes single authoritative pipeline update
    lcd.print("SAVED TO NVM!");

   } else {

    lcd.print("OUT OF RANGE!");

   }

   delay(800);

   inputLen = 0;
   inputBuffer[0] = '\0';
   updateLCDMenuView();

  }

 }

 else if (key == '*') {
  inputLen = 0;
  inputBuffer[0] = '\0';

  if (cutoffTripped) {
   cutoffTripped = false;
   Serial.println("[SAFETY] Hard cutoff trip reset.");

  }

  updateLCDMenuView();

 }

 else if (key == 'A' || key == 'B') {
  currentSetting = (currentSetting + 1) % 2;
  inputLen = 0;
  inputBuffer[0] = '\0';
  updateLCDMenuView();

 }

}

void refreshDisplay() {

 if (inMenuMode) return;

 lcd.clear();
 lcd.setCursor(0, 0);

 if (cutoffTripped) {

  lcd.print("TRIP: HARD LIMIT");
  lcd.setCursor(0, 1);
  lcd.print("Press * / BLE RST");

 } else if (phase == PH_RUNNING) {

  unsigned long runSec = (millis() - runStarted) / 1000;

  lcd.print("RUN:");
  lcd.print(runSec); 
  lcd.print("s / "); 
  lcd.print(userRunMinutes); 
  lcd.print("m");

  lcd.setCursor(0, 1);

  lcd.print("M:"); 
  lcd.print(readMoisturePct(), 0); lcd.print("% Fcst:");
  lcd.print(forecastFresh() ? String((int)rainChance) + "%" : "STL");

 } else {
  
  switch (currentState) {

   case S_TANK_EMPTY:  
   lcd.print("ST: TANK EMPTY"); 
   break;

   case S_RAIN:     
   lcd.print("ST: LOCAL RAIN"); 
   break;

   case S_HOLD:     
   lcd.print("ST: MOIST OK"); break;

   case S_FORECAST_HOLD: 
   lcd.print("ST: FCST HOLD"); 
   break;

   default:       
   lcd.print("ST: IDLE"); 
   break;

  }
  lcd.setCursor(0, 1);
  lcd.print("M:"); lcd.print(readMoisturePct(), 0); lcd.print("% ");
  lcd.print("Fcst:"); lcd.print(forecastFresh() ? String((int)rainChance) + "%" : "STL");
 }

}

void setup() {
 Serial.begin(115200);
 delay(500);

 pinMode(PUMP_PIN, OUTPUT);
 pinMode(VALVE_PIN, OUTPUT);

 pinMode(LED_PIN, OUTPUT);
 digitalWrite(LED_PIN, LOW);

  pinMode(LED_OPENING, OUTPUT);
  digitalWrite(LED_OPENING, LOW);

  pinMode(LED_RUNNING, OUTPUT);
  digitalWrite(LED_RUNNING, LOW);

   pinMode(LED_CLOSING, OUTPUT);
   digitalWrite(LED_CLOSING, LOW);

 pinMode(BUZZ_PIN, OUTPUT);
 pinMode(MOIST_PIN, INPUT);
 pinMode(RAIN_PIN, INPUT);
 pinMode(FLOAT_PIN, INPUT);

 digitalWrite(PUMP_PIN, LOW);
 digitalWrite(VALVE_PIN, LOW);

 lcd.init();
 lcd.backlight();
 lcd.setCursor(0, 0);
 lcd.print("Booting System...");

 loadSettings();
 connectWiFi();
 fetchForecast();

 // Start Bluetooth Low Energy GATT Service

 startBle();
}

void loop() {

 unsigned long now = millis();

 if (now - tSense >= 1000) {

  tSense = now;

  currentState = decideState();

  if (!forecastOk || (now - forecastAt >= FETCH_MS)) {

   fetchForecast();

  }

 }

 handleKeypad();

 if (inMenuMode && (now - lastKeypress > 5000)) {

  inMenuMode = false;

  inputLen = 0;

  inputBuffer[0] = '\0';

 }


 bool wantRun = (currentState == S_IRRIGATE) && !cutoffTripped;

 serviceOutputs(wantRun);

 if (now - tUI >= 200) {

  tUI = now;

  refreshDisplay();

 }

 if (now - tBleNotify >= 1000) {

  tBleNotify = now;

  updateBleTelemetry();

 }

}