#include <Keypad.h>
const byte ROWS = 4, COLS = 4;
char keys[ROWS][COLS] = {
{'1','2','3','A'},
{'4','5','6','B'},
{'7','8','9','C'},
{'*','0','#','D'}
};
byte rowPins[ROWS] = { 13, 12, 14, 27 };
byte colPins[COLS] = { 26, 25, 33, 32 };
Keypad pad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
void loop() {
char k = pad.getKey(); // non-blocking, debounced
if (k) Serial.println(k);
}