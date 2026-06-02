/*
  The Ooho II - two illuminated button test

  Wiring:
  - Button A switch: one side -> GND, other side -> Teensy pin 32
  - Button B switch: one side -> GND, other side -> Teensy pin 30
  - Optional Button A LED: Teensy pin 31 -> 330R-1k resistor -> LED +, LED - -> GND
  - Optional Button B LED: Teensy pin 29 -> 330R-1k resistor -> LED +, LED - -> GND

  Behavior:
  - Press button A: built-in LED and LED A turn on
  - Press button B: built-in LED and LED B turn on
  - Serial prints debounced press/release events
*/

constexpr int BUTTON_A_PIN = 32;
constexpr int BUTTON_B_PIN = 30;
constexpr int LED_A_PIN = 31;
constexpr int LED_B_PIN = 29;
constexpr uint32_t DEBOUNCE_MS = 30;

struct DebouncedButton {
  int pin;
  const char *name;
  bool raw = HIGH;
  bool stable = HIGH;
  uint32_t lastChangeMs = 0;
};

DebouncedButton buttonA{BUTTON_A_PIN, "A"};
DebouncedButton buttonB{BUTTON_B_PIN, "B"};

bool updateButton(DebouncedButton &button) {
  const bool nextRaw = digitalRead(button.pin);
  const uint32_t now = millis();

  if (nextRaw != button.raw) {
    button.raw = nextRaw;
    button.lastChangeMs = now;
  }

  if ((now - button.lastChangeMs) >= DEBOUNCE_MS && button.raw != button.stable) {
    button.stable = button.raw;
    Serial.print("Button ");
    Serial.print(button.name);
    Serial.println(button.stable == LOW ? " pressed" : " released");
    return true;
  }

  return false;
}

bool isPressed(const DebouncedButton &button) {
  return button.stable == LOW;
}

void setup() {
  pinMode(BUTTON_A_PIN, INPUT_PULLUP);
  pinMode(BUTTON_B_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(LED_A_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);

  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(LED_A_PIN, LOW);
  digitalWrite(LED_B_PIN, LOW);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho II two button test");
  Serial.println("Button A: pin 32 -> GND. Optional LED A: pin 31.");
  Serial.println("Button B: pin 30 -> GND. Optional LED B: pin 29.");
}

void loop() {
  updateButton(buttonA);
  updateButton(buttonB);

  const bool aPressed = isPressed(buttonA);
  const bool bPressed = isPressed(buttonB);

  digitalWrite(LED_A_PIN, aPressed ? HIGH : LOW);
  digitalWrite(LED_B_PIN, bPressed ? HIGH : LOW);
  digitalWrite(LED_BUILTIN, (aPressed || bPressed) ? HIGH : LOW);
}
