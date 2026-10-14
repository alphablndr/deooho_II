/*
  SPIKE (2026-10-14): play lab renders through the real LOGOS speaker.

  Plays ARC.WAV / AWAKE.WAV / ASLEEP.WAV from the built-in SD card on the
  left line out (the LOGOS speaker), looping. Record button (pin 32): next
  file. Volume as in the voicebody build. 44.1 kHz 16-bit WAV only.

  Not the exhibition firmware: reflash ooho_combined_voicebody afterwards.
*/
#include <Audio.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>

constexpr int BUTTON_PIN = 32;
constexpr float VOLUME = 0.18f;  // voicebody STARTUP_VOLUME
const char *const FILES[] = {"ARC.WAV", "AWAKE.WAV", "ASLEEP.WAV"};
constexpr uint8_t FILE_COUNT = sizeof(FILES) / sizeof(FILES[0]);

AudioPlaySdWav player;
AudioOutputI2S i2sOut;
AudioConnection toSpeaker(player, 0, i2sOut, 0);  // left = LOGOS speaker only
AudioControlSGTL5000 shield;

uint8_t current = 0;
bool lastButton = HIGH;
uint32_t lastChangeMs = 0;

void playCurrent() {
  Serial.print("Playing ");
  Serial.println(FILES[current]);
  if (!player.play(FILES[current])) {
    Serial.println("  could not open it");
  }
  delay(25);  // let the header parse before isPlaying() is checked
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  AudioMemory(16);
  shield.enable();
  shield.volume(VOLUME);
  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("SD init failed.");
  }
  playCurrent();
}

void loop() {
  const bool button = digitalRead(BUTTON_PIN);
  if (button != lastButton && millis() - lastChangeMs > 40) {
    lastChangeMs = millis();
    lastButton = button;
    if (button == LOW) {
      player.stop();
      current = (current + 1) % FILE_COUNT;
      playCurrent();
    }
  }
  if (!player.isPlaying()) {
    playCurrent();  // loop the file
  }
}
