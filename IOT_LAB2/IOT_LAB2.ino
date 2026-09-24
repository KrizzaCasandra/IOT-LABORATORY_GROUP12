const int LED[8] = {13, 12, 14, 27, 26, 25, 33, 32};
const int LAMP_PIN = 4;
const int BUZZ = 15;

enum Pattern {
  P_SINGLE,
  P_CONVERGE,
  P_FILL
};

Pattern pattern = P_SINGLE;

unsigned int stepMs = 120;
unsigned long tChase = 0;
int step = 0;

void clearRow() {
  for (int i = 0; i < 8; i++) {
    digitalWrite(LED[i], LOW);
  }
}

void serviceChaser() {
  if (millis() - tChase < stepMs)
    return;

  tChase = millis(); // Continuing the code without interruption

  clearRow();

  switch (pattern) {
    case P_SINGLE:
      digitalWrite(LED[step % 8], HIGH); //Repeats the cycle
      break;

    case P_CONVERGE:
      digitalWrite(LED[step % 4], HIGH); //Repeats the cycle
      digitalWrite(LED[7 - (step % 4)], HIGH); // Repeats the cycle
      break;

    case P_FILL:
      for (int i = 0; i <= (step % 8); i++) {
        digitalWrite(LED[i], HIGH);
      }
      break;
  }

  step++;
}

void setBrightness(uint8_t level) {
  ledcWrite(LAMP_PIN, level);
}

enum Alert {
  A_NONE,
  A_CONFIRM,
  A_WARN,
  A_ALARM
};

Alert alert = A_NONE;

unsigned long tBuzz = 0;
int beep = 0;

void serviceBuzzer() {
  unsigned long now = millis();

  switch (alert) {
    case A_NONE:
      digitalWrite(BUZZ, LOW);
      break;

    case A_CONFIRM:
      if (beep == 0) {
        digitalWrite(BUZZ, HIGH);
        tBuzz = now;
        beep = 1;
      }
      else if (now - tBuzz > 120) {
        digitalWrite(BUZZ, LOW);
        alert = A_NONE;
        beep = 0;
      }
      break;

    case A_WARN:
      digitalWrite(BUZZ, (now % 2000) < 200);
      break;

    case A_ALARM:
      digitalWrite(BUZZ, (now % 200) < 100);
      break;
  }
}

enum State {
  S_IDLE,
  S_ACTIVE,
  S_WARNING,
  S_ALARM
};

State state = S_IDLE;

void applyState(State s) {
  switch (s) {
    case S_IDLE:
      pattern = P_SINGLE;
      stepMs = 400;
      setBrightness(20);
      alert = A_NONE;
      break;

    case S_ACTIVE:
      pattern = P_FILL;
      stepMs = 120;
      setBrightness(255);
      alert = A_CONFIRM;
      break;

    case S_WARNING:
      pattern = P_CONVERGE;
      stepMs = 200;
      setBrightness(120);
      alert = A_WARN;
      break;

    case S_ALARM:
      pattern = P_SINGLE;
      stepMs = 60;
      setBrightness(255);
      alert = A_ALARM;
      break;
  }

  state = s;
}

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 8; i++) {
    pinMode(LED[i], OUTPUT);
  }

  pinMode(BUZZ, OUTPUT);

  ledcAttach(LAMP_PIN, 5000, 8);

  applyState(S_IDLE);
}

void loop() {
  serviceChaser();
  serviceBuzzer();
}
