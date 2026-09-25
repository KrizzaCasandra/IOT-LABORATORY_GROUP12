const char* CODE = "2468";
const int CLEN = 4;
char entry[CLEN + 1] = "";
int entryLen = 0;
void promptEntry() {
lcd.clear();
lcd.setCursor(0, 0); lcd.print("ENTER CODE:");
lcd.setCursor(0, 1);
for (int i = 0; i < entryLen; i++) lcd.print('*'); // masked
}
void handleKey(char k) {
if (k == '*') { // clear
entryLen = 0; entry[0] = 0; promptEntry(); return;
}
if (k == '#') { checkCode(); return; } // submit
if (entryLen < CLEN && k >= '0' && k <= '9') {
entry[entryLen++] = k;
entry[entryLen] = 0;
promptEntry();
}
}