/*
  The Ooho button + LED hardware test.

  Wiring:
  - Button between Teensy pin 32 and GND
  - External LED: pin 31 -> 220-1000 ohm resistor -> LED anode, LED cathode -> GND

  Behavior:
  - Press button: built-in LED and external LED turn on.
  - Release button: both LEDs turn off.
*/

constexpr int BUTTON_PIN = 32;
constexpr int STATUS_LED_PIN = 31;

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);

  Serial.begin(115200);
  delay(500);
  Serial.println("Button + LED test");
  Serial.println("Press button: LEDs ON. Release button: LEDs OFF.");
}

void loop() {
  const bool pressed = digitalRead(BUTTON_PIN) == LOW;
  digitalWrite(LED_BUILTIN, pressed ? HIGH : LOW);
  digitalWrite(STATUS_LED_PIN, pressed ? HIGH : LOW);

  static bool lastPressed = false;
  if (pressed != lastPressed) {
    lastPressed = pressed;
    Serial.println(pressed ? "button pressed" : "button released");
  }

  delay(10);
}
