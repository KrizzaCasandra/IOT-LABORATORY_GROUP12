const unsigned long UNLOCK_MS = 3000; // relay energised, then released
const unsigned long LOCKOUT_MS = 15000;
const int MAX_FAILS = 3;
int fails = 0;
unsigned long unlockUntil = 0, lockoutUntil = 0;
void checkCode() {
if (millis() < lockoutUntil) { applyState(S_ALARM); return; }
if (strcmp(entry, CODE) == 0) {
fails = 0;
unlockUntil = millis() + UNLOCK_MS;
digitalWrite(LOCK_PIN, HIGH);
lcd.clear(); lcd.print("GRANTED");
applyState(S_ACTIVE);
} else {
fails++;
lcd.clear(); lcd.print("DENIED");
applyState(fails >= MAX_FAILS ? S_ALARM : S_WARNING);
if (fails >= MAX_FAILS) lockoutUntil = millis() + LOCKOUT_MS;
}
entryLen = 0; entry[0] = 0;
}
void serviceLock() { // release on time, always
if (unlockUntil && millis() > unlockUntil) {
digitalWrite(LOCK_PIN, LOW);
unlockUntil = 0;
applyState(S_IDLE);
promptEntry();
}
}