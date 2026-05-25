/*
  The Ooho transducer tone test.

  Wiring:
  - Button between Teensy pin 32 and GND
  - Audio Shield LINE OUT L/R/G -> amplifier -> transducer/speaker
  - Optional LED: pin 31 -> resistor -> LED anode, LED cathode -> GND

  Behavior:
  - Button pressed: tone on, LED on
  - Button released: tone off, LED off
*/

#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

constexpr int BUTTON_PIN = 32;
constexpr int STATUS_LED_PIN = 31;
constexpr float OUTPUT_VOLUME = 0.18f;
constexpr float TONE_AMPLITUDE = 0.18f;
constexpr float TONE_FREQUENCY_HZ = 440.0f;

AudioSynthWaveform testTone;
AudioOutputI2S i2sOut;
AudioConnection patchCordLeft(testTone, 0, i2sOut, 0);
AudioConnection patchCordRight(testTone, 0, i2sOut, 1);
AudioControlSGTL5000 audioShield;

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho tone button test");

  AudioMemory(20);
  audioShield.enable();
  audioShield.volume(OUTPUT_VOLUME);

  testTone.begin(WAVEFORM_SINE);
  testTone.frequency(TONE_FREQUENCY_HZ);
  testTone.amplitude(0.0f);

  Serial.println("Press button: tone ON. Release button: tone OFF.");
}

void loop() {
  const bool pressed = digitalRead(BUTTON_PIN) == LOW;
  testTone.amplitude(pressed ? TONE_AMPLITUDE : 0.0f);
  digitalWrite(LED_BUILTIN, pressed ? HIGH : LOW);
  digitalWrite(STATUS_LED_PIN, pressed ? HIGH : LOW);

  static bool lastPressed = false;
  if (pressed != lastPressed) {
    lastPressed = pressed;
    Serial.println(pressed ? "tone on" : "tone off");
  }

  delay(10);
}
