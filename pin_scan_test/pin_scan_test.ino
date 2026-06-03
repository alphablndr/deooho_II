/*
  The Ooho pin scan test.

  Purpose:
  - Find which Teensy pin a button is actually wired to.
  - Each listed pin uses INPUT_PULLUP, so a press to GND reads LOW.
  - Built-in LED turns on when any scanned pin is LOW.

  Try pressing buttons or briefly jumpering candidate pins to GND.
*/

constexpr int SCAN_PINS[] = {28, 29, 30, 32, 33};
constexpr int PIN_COUNT = sizeof(SCAN_PINS) / sizeof(SCAN_PINS[0]);
constexpr uint32_t PRINT_INTERVAL_MS = 2000;

bool lastPressed[PIN_COUNT] = {false};
uint32_t lastPrintMs = 0;

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  for (int i = 0; i < PIN_COUNT; ++i) {
    pinMode(SCAN_PINS[i], INPUT_PULLUP);
  }

  Serial.begin(115200);
  delay(500);
  Serial.println("Ooho pin scan test");
  Serial.println("Scanned pins: 28, 29, 30, 32, 33");
  Serial.println("Pressed = pin shorted to GND.");
}

void loop() {
  bool anyPressed = false;
  for (int i = 0; i < PIN_COUNT; ++i) {
    const bool pressed = digitalRead(SCAN_PINS[i]) == LOW;
    anyPressed = anyPressed || pressed;
    if (pressed != lastPressed[i]) {
      lastPressed[i] = pressed;
      Serial.print("pin ");
      Serial.print(SCAN_PINS[i]);
      Serial.println(pressed ? " pressed" : " released");
    }
  }

  digitalWrite(LED_BUILTIN, anyPressed ? HIGH : LOW);

  if (millis() - lastPrintMs >= PRINT_INTERVAL_MS) {
    lastPrintMs = millis();
    Serial.print("state:");
    for (int i = 0; i < PIN_COUNT; ++i) {
      Serial.print(' ');
      Serial.print(SCAN_PINS[i]);
      Serial.print('=');
      Serial.print(digitalRead(SCAN_PINS[i]) == LOW ? "LOW" : "HIGH");
    }
    Serial.println();
  }

  delay(10);
}
