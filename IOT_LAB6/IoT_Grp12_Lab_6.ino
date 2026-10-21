#include <Wire.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

const int SOIL_PIN  = 32;
const int RAIN_PIN  = 35;
const int FLOAT_PIN = 39;

const int PUMP_PIN  = 27;
const int VALVE_PIN = 26;

const int LED_STATUS  = 33;
const int LED_OPENING = 13;
const int LED_RUNNING = 12;
const int LED_CLOSING = 14;

const int BUZZER_PIN = 25;

const int RELAY_ON  = LOW;
const int RELAY_OFF = HIGH;

const int RAW_DRY = 4095;
const int RAW_WET = 1400;

const int RAIN_THRESHOLD = 2500;

const bool FLOAT_ACTIVE_HIGH = true;

LiquidCrystal_I2C lcd(0x27, 16, 2);

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

byte rowPins[ROWS] = {
  19, 18, 5, 17
};

byte colPins[COLS] = {
  16, 4, 0, 2
};

Keypad keypad = Keypad(
  makeKeymap(keys),
  rowPins,
  colPins,
  ROWS,
  COLS
);

struct SystemConfig {
  int runMinutes;
  int setpointPct;
};

SystemConfig cfg = {
  3,
  60
};

Preferences preferences;

enum EditMode {
  EDIT_NONE,
  EDIT_SETPOINT,
  EDIT_RUNTIME
};

EditMode editMode = EDIT_NONE;

String inputBuffer = "";

#define SERVICE_UUID \
"a1b2c3d4-0001-4000-8000-00805f9b34fb"

#define CH_STATE_UUID \
"a1b2c3d4-0002-4000-8000-00805f9b34fb"

#define CH_VALUES_UUID \
"a1b2c3d4-0003-4000-8000-00805f9b34fb"

#define CH_SETPT_UUID \
"a1b2c3d4-0004-4000-8000-00805f9b34fb"

#define CH_CMD_UUID \
"a1b2c3d4-0005-4000-8000-00805f9b34fb"

BLEServer *bleServer = nullptr;

BLECharacteristic *chState  = nullptr;
BLECharacteristic *chValues = nullptr;
BLECharacteristic *chSetpt  = nullptr;
BLECharacteristic *chCmd    = nullptr;

bool deviceConnected = false;

String systemState = "HOLD";

bool manualMode = false;
bool manualRun  = false;

bool alarmAcknowledged = false;

unsigned long manualStartTime = 0;
unsigned long lastBleUpdate   = 0;
unsigned long lastDisplay     = 0;

int forecastValue = -1;

void saveSettings();
void loadSettings();
void onSettingChanged();

void refreshDisplay();
void processKeypad();

int getMoisturePercent();
int readRainRaw();

bool rainDetected();
bool tankHasWater();

void pumpOn();
void pumpOff();

void valveOn();
void valveOff();

void stopIrrigation();
void startIrrigation();

void updateBLE();
void automaticControl();
void checkManualRuntime();

void saveSettings() {
  preferences.begin("irrig", false);

  preferences.putInt(
    "runtime",
    cfg.runMinutes
  );

  preferences.putInt(
    "setpoint",
    cfg.setpointPct
  );

  preferences.end();

  Serial.println(
    "[NVM] Settings saved"
  );
}

void loadSettings() {
  preferences.begin("irrig", true);

  cfg.runMinutes =
    preferences.getInt(
      "runtime",
      3
    );

  cfg.setpointPct =
    preferences.getInt(
      "setpoint",
      60
    );

  preferences.end();

  Serial.printf(
    "[NVM] Loaded: Run=%d min, Setpoint=%d%%\n",
    cfg.runMinutes,
    cfg.setpointPct
  );
}

int readRainRaw() {
  return analogRead(RAIN_PIN);
}

int getMoisturePercent() {
  int raw =
    analogRead(SOIL_PIN);

  int pct =
    map(
      raw,
      RAW_DRY,
      RAW_WET,
      0,
      100
    );

  pct =
    constrain(
      pct,
      0,
      100
    );

  return pct;
}

bool rainDetected() {
  int raw =
    readRainRaw();

  return
    raw < RAIN_THRESHOLD;
}

bool tankHasWater() {
  int value =
    digitalRead(FLOAT_PIN);

  if (FLOAT_ACTIVE_HIGH) {
    return value == HIGH;
  }

  return value == LOW;
}

void pumpOn() {
  digitalWrite(
    PUMP_PIN,
    RELAY_ON
  );

  Serial.println(
    "[PUMP] ON"
  );
}

void pumpOff() {
  digitalWrite(
    PUMP_PIN,
    RELAY_OFF
  );
}

void valveOn() {
  digitalWrite(
    VALVE_PIN,
    RELAY_ON
  );

  Serial.println(
    "[VALVE] OPEN"
  );
}

void valveOff() {
  digitalWrite(
    VALVE_PIN,
    RELAY_OFF
  );
}

void stopIrrigation() {
  pumpOff();
  valveOff();

  digitalWrite(
    LED_OPENING,
    LOW
  );

  digitalWrite(
    LED_RUNNING,
    LOW
  );

  digitalWrite(
    LED_CLOSING,
    LOW
  );

  digitalWrite(
    LED_STATUS,
    HIGH
  );
}

void startIrrigation() {
  if (!tankHasWater()) {
    systemState =
      "TANK_EMPTY";

    stopIrrigation();

    digitalWrite(
      BUZZER_PIN,
      HIGH
    );

    Serial.println(
      "[SYSTEM] Irrigation rejected: tank empty"
    );

    return;
  }

  digitalWrite(
    LED_STATUS,
    LOW
  );

  digitalWrite(
    LED_OPENING,
    HIGH
  );

  valveOn();

  delay(500);

  digitalWrite(
    LED_OPENING,
    LOW
  );

  digitalWrite(
    LED_RUNNING,
    HIGH
  );

  pumpOn();

  systemState =
    "IRRIGATE";

  Serial.println(
    "[SYSTEM] Irrigation started"
  );
}

void refreshDisplay() {
  int moisture =
    getMoisturePercent();

  lcd.clear();

  lcd.setCursor(
    0,
    0
  );

  lcd.print("M:");
  lcd.print(moisture);
  lcd.print("% ");

  lcd.print("SP:");
  lcd.print(
    cfg.setpointPct
  );

  lcd.setCursor(
    0,
    1
  );

  lcd.print(
    systemState
  );
}

void onSettingChanged() {
  saveSettings();

  refreshDisplay();

  if (chSetpt != nullptr) {
    String value =
      String(
        cfg.setpointPct
      );

    chSetpt->setValue(
      value.c_str()
    );

    if (deviceConnected) {
      chSetpt->notify();
    }
  }

  Serial.printf(
    "[SYNC] Run=%d min, Setpoint=%d%%\n",
    cfg.runMinutes,
    cfg.setpointPct
  );
}

void processKeypad() {
  char key =
    keypad.getKey();

  if (!key) {
    return;
  }

  Serial.print(
    "[KEYPAD] "
  );

  Serial.println(key);

  if (
    key == 'A' &&
    editMode == EDIT_NONE
  ) {
    editMode =
      EDIT_SETPOINT;

    inputBuffer = "";

    lcd.clear();

    lcd.setCursor(
      0,
      0
    );

    lcd.print(
      "Set Setpoint:"
    );

    lcd.setCursor(
      0,
      1
    );

    lcd.print(
      "#Save *Cancel"
    );

    return;
  }

  if (
    key == 'B' &&
    editMode == EDIT_NONE
  ) {
    editMode =
      EDIT_RUNTIME;

    inputBuffer = "";

    lcd.clear();

    lcd.setCursor(
      0,
      0
    );

    lcd.print(
      "Set Run Time:"
    );

    lcd.setCursor(
      0,
      1
    );

    lcd.print(
      "#Save *Cancel"
    );

    return;
  }

  if (
    editMode ==
    EDIT_NONE
  ) {
    return;
  }

  if (key == '*') {
    editMode =
      EDIT_NONE;

    inputBuffer = "";

    refreshDisplay();

    Serial.println(
      "[KEYPAD] Edit cancelled"
    );

    return;
  }

  if (key == '#') {
    if (
      inputBuffer.length() ==
      0
    ) {
      editMode =
        EDIT_NONE;

      refreshDisplay();

      return;
    }

    int newValue =
      inputBuffer.toInt();

    if (
      editMode ==
      EDIT_SETPOINT
    ) {
      if (
        newValue >= 10 &&
        newValue <= 90
      ) {
        cfg.setpointPct =
          newValue;

        Serial.printf(
          "[KEYPAD] Setpoint changed to %d%%\n",
          cfg.setpointPct
        );

        onSettingChanged();
      }

      else {
        Serial.println(
          "[KEYPAD] Invalid setpoint"
        );
      }
    }

    else if (
      editMode ==
      EDIT_RUNTIME
    ) {
      if (
        newValue >= 1 &&
        newValue <= 30
      ) {
        cfg.runMinutes =
          newValue;

        Serial.printf(
          "[KEYPAD] Runtime changed to %d min\n",
          cfg.runMinutes
        );

        onSettingChanged();
      }

      else {
        Serial.println(
          "[KEYPAD] Invalid runtime"
        );
      }
    }

    editMode =
      EDIT_NONE;

    inputBuffer = "";

    refreshDisplay();

    return;
  }

  if (
    key >= '0' &&
    key <= '9'
  ) {
    if (
      inputBuffer.length() <
      3
    ) {
      inputBuffer += key;

      lcd.clear();

      if (
        editMode ==
        EDIT_SETPOINT
      ) {
        lcd.print(
          "Setpoint:"
        );
      }

      else {
        lcd.print(
          "Run Time:"
        );
      }

      lcd.setCursor(
        0,
        1
      );

      lcd.print(
        inputBuffer
      );
    }
  }
}

class ServerCallbacks :
  public BLEServerCallbacks {

  void onConnect(
    BLEServer *pServer
  ) override {
    deviceConnected =
      true;

    Serial.println(
      "[BLE] Device connected"
    );
  }

  void onDisconnect(
    BLEServer *pServer
  ) override {
    deviceConnected =
      false;

    Serial.println(
      "[BLE] Device disconnected"
    );

    BLEDevice::
      startAdvertising();

    Serial.println(
      "[BLE] Advertising restarted"
    );
  }
};

class SetpointCallbacks :
  public BLECharacteristicCallbacks {

  void onWrite(
    BLECharacteristic *pCharacteristic
  ) override {
    String value =
      pCharacteristic
        ->getValue()
        .c_str();

    value.trim();

    if (
      value.length() ==
      0
    ) {
      return;
    }

    int newSetpoint =
      value.toInt();

    Serial.print(
      "[BLE] Setpoint request: "
    );

    Serial.println(
      newSetpoint
    );

    if (
      newSetpoint >= 10 &&
      newSetpoint <= 90
    ) {
      cfg.setpointPct =
        newSetpoint;

      onSettingChanged();

      Serial.printf(
        "[BLE] Setpoint changed to %d%%\n",
        cfg.setpointPct
      );
    }

    else {
      Serial.println(
        "[BLE] Invalid setpoint rejected"
      );

      String currentValue =
        String(
          cfg.setpointPct
        );

      pCharacteristic
        ->setValue(
          currentValue.c_str()
        );

      if (deviceConnected) {
        pCharacteristic
          ->notify();
      }
    }
  }
};

class CommandCallbacks :
  public BLECharacteristicCallbacks {

  void onWrite(
    BLECharacteristic *pCharacteristic
  ) override {
    String command =
      pCharacteristic
        ->getValue()
        .c_str();

    command.trim();
    command.toUpperCase();

    Serial.print(
      "[BLE CMD] "
    );

    Serial.println(
      command
    );

    if (
      command ==
      "START"
    ) {
      if (!tankHasWater()) {
        systemState =
          "TANK_EMPTY";

        stopIrrigation();

        Serial.println(
          "[BLE] START rejected: tank empty"
        );

        updateBLE();

        return;
      }

      manualMode =
        true;

      manualRun =
        true;

      manualStartTime =
        millis();

      startIrrigation();

      Serial.println(
        "[BLE] Manual irrigation START"
      );
    }

    else if (
      command ==
      "STOP"
    ) {
      manualMode =
        true;

      manualRun =
        false;

      stopIrrigation();

      systemState =
        "MANUAL_STOP";

      Serial.println(
        "[BLE] Manual irrigation STOP"
      );
    }

    else if (
      command ==
      "AUTO"
    ) {
      manualMode =
        false;

      manualRun =
        false;

      stopIrrigation();

      systemState =
        "AUTO";

      Serial.println(
        "[BLE] Automatic mode restored"
      );
    }

    else if (
      command ==
      "ACK"
    ) {
      alarmAcknowledged =
        true;

      digitalWrite(
        BUZZER_PIN,
        LOW
      );

      Serial.println(
        "[BLE] Alarm acknowledged"
      );
    }

    else if (
      command.startsWith(
        "RUN:"
      )
    ) {
      String number =
        command.substring(4);

      int newRunTime =
        number.toInt();

      if (
        newRunTime >= 1 &&
        newRunTime <= 30
      ) {
        cfg.runMinutes =
          newRunTime;

        onSettingChanged();

        Serial.printf(
          "[BLE] Runtime changed to %d minute(s)\n",
          cfg.runMinutes
        );
      }

      else {
        Serial.println(
          "[BLE] Invalid runtime rejected"
        );
      }
    }

    else {
      Serial.println(
        "[BLE] Unknown command rejected"
      );
    }

    refreshDisplay();
    updateBLE();
  }
};

void startBLE() {
  BLEDevice::init(
    "CpE-Node-G12"
  );

  bleServer =
    BLEDevice::
      createServer();

  bleServer
    ->setCallbacks(
      new ServerCallbacks()
    );

  BLEService *service =
    bleServer
      ->createService(
        SERVICE_UUID
      );

  chState =
    service
      ->createCharacteristic(
        CH_STATE_UUID,

        BLECharacteristic::
          PROPERTY_READ |

        BLECharacteristic::
          PROPERTY_NOTIFY
      );

  chState
    ->addDescriptor(
      new BLE2902()
    );

  chValues =
    service
      ->createCharacteristic(
        CH_VALUES_UUID,

        BLECharacteristic::
          PROPERTY_READ |

        BLECharacteristic::
          PROPERTY_NOTIFY
      );

  chValues
    ->addDescriptor(
      new BLE2902()
    );

  chSetpt =
    service
      ->createCharacteristic(
        CH_SETPT_UUID,

        BLECharacteristic::
          PROPERTY_READ |

        BLECharacteristic::
          PROPERTY_WRITE |

        BLECharacteristic::
          PROPERTY_NOTIFY
      );

  chSetpt
    ->addDescriptor(
      new BLE2902()
    );

  chSetpt
    ->setCallbacks(
      new SetpointCallbacks()
    );

  chCmd =
    service
      ->createCharacteristic(
        CH_CMD_UUID,

        BLECharacteristic::
          PROPERTY_WRITE
      );

  chCmd
    ->setCallbacks(
      new CommandCallbacks()
    );

  chState
    ->setValue(
      "HOLD"
    );

  chValues
    ->setValue(
      "M:0,R:0,T:0,F:-1"
    );

  chSetpt
    ->setValue(
      String(
        cfg.setpointPct
      ).c_str()
    );

  service->start();

  BLEAdvertising *advertising =
    BLEDevice::
      getAdvertising();

  advertising
    ->addServiceUUID(
      SERVICE_UUID
    );

  advertising
    ->setScanResponse(
      true
    );

  BLEDevice::
    startAdvertising();

  Serial.println(
    "[BLE] Advertising started"
  );

  Serial.println(
    "[BLE] Device name: CpE-Node-G12"
  );
}

void updateBLE() {
  if (!deviceConnected) {
    return;
  }

  int moisture =
    getMoisturePercent();

  int rain =
    rainDetected()
      ? 1
      : 0;

  int tank =
    tankHasWater()
      ? 1
      : 0;

  String stateValue =
    systemState;

  String sensorValue =
    "M:" +
    String(moisture) +

    ",R:" +
    String(rain) +

    ",T:" +
    String(tank) +

    ",F:" +
    String(forecastValue);

  chState->setValue(
    stateValue.c_str()
  );

  chState->notify();

  chValues->setValue(
    sensorValue.c_str()
  );

  chValues->notify();

  chSetpt->setValue(
    String(
      cfg.setpointPct
    ).c_str()
  );

  chSetpt->notify();

  Serial.print(
    "[BLE STATE] "
  );

  Serial.println(
    stateValue
  );

  Serial.print(
    "[BLE VALUES] "
  );

  Serial.println(
    sensorValue
  );
}

void automaticControl() {
  if (manualMode) {
    return;
  }

  int moisture =
    getMoisturePercent();

  if (!tankHasWater()) {
    systemState =
      "TANK_EMPTY";

    stopIrrigation();

    if (!alarmAcknowledged) {
      digitalWrite(
        BUZZER_PIN,
        HIGH
      );
    }

    return;
  }

  digitalWrite(
    BUZZER_PIN,
    LOW
  );

  alarmAcknowledged =
    false;

  if (rainDetected()) {
    systemState =
      "RAIN";

    stopIrrigation();

    return;
  }

  if (
    moisture <
    cfg.setpointPct
  ) {
    if (
      systemState !=
      "IRRIGATE"
    ) {
      startIrrigation();
    }
  }

  else {
    systemState =
      "HOLD";

    stopIrrigation();
  }
}

void checkManualRuntime() {
  if (
    !manualMode ||
    !manualRun
  ) {
    return;
  }

  unsigned long duration =
    (unsigned long)
    cfg.runMinutes *
    60UL *
    1000UL;

  if (
    millis() -
    manualStartTime >=
    duration
  ) {
    manualRun =
      false;

    stopIrrigation();

    systemState =
      "MANUAL_STOP";

    Serial.println(
      "[SYSTEM] Manual runtime completed"
    );
  }
}

void setup() {
  Serial.begin(
    115200
  );

  delay(1000);

  Serial.println();

  Serial.println(
    "SMART FARMING SYSTEM - LAB 6"
  );

  pinMode(
    SOIL_PIN,
    INPUT
  );

  pinMode(
    RAIN_PIN,
    INPUT
  );

  pinMode(
    FLOAT_PIN,
    INPUT
  );

  pinMode(
    PUMP_PIN,
    OUTPUT
  );

  pinMode(
    VALVE_PIN,
    OUTPUT
  );

  pinMode(
    LED_STATUS,
    OUTPUT
  );

  pinMode(
    LED_OPENING,
    OUTPUT
  );

  pinMode(
    LED_RUNNING,
    OUTPUT
  );

  pinMode(
    LED_CLOSING,
    OUTPUT
  );

  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  digitalWrite(
    PUMP_PIN,
    RELAY_OFF
  );

  digitalWrite(
    VALVE_PIN,
    RELAY_OFF
  );

  digitalWrite(
    LED_STATUS,
    HIGH
  );

  digitalWrite(
    LED_OPENING,
    LOW
  );

  digitalWrite(
    LED_RUNNING,
    LOW
  );

  digitalWrite(
    LED_CLOSING,
    LOW
  );

  digitalWrite(
    BUZZER_PIN,
    LOW
  );

  Wire.begin(
    21,
    22
  );

  lcd.init();
  lcd.backlight();

  lcd.clear();

  lcd.setCursor(
    0,
    0
  );

  lcd.print(
    "Smart Farming"
  );

  lcd.setCursor(
    0,
    1
  );

  lcd.print(
    "Starting..."
  );

  loadSettings();

  startBLE();

  delay(1000);

  refreshDisplay();

  Serial.println(
    "[SYSTEM] Ready"
  );
}

void loop() {
  processKeypad();

  automaticControl();

  checkManualRuntime();

  if (
    millis() -
    lastDisplay >=
    1000
  ) {
    lastDisplay =
      millis();

    refreshDisplay();
  }

  if (
    millis() -
    lastBleUpdate >=
    2000
  ) {
    lastBleUpdate =
      millis();

    updateBLE();
  }

  delay(20);
}