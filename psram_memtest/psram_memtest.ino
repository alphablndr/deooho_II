/*
  Minimal Teensy 4.1 PSRAM test for The Ooho.

  Expected with one APS6404L PSRAM chip:
    external_psram_size = 8 MB
    all tests pass
*/

#include <Arduino.h>

extern "C" uint8_t external_psram_size;

constexpr size_t TEST_BYTES = 8UL * 1024UL * 1024UL;
EXTMEM uint32_t psramWords[TEST_BYTES / sizeof(uint32_t)];

uint32_t patternForIndex(size_t index) {
  return 0xA5A50000UL ^ static_cast<uint32_t>(index * 2654435761UL);
}

bool verifyPattern(const char *label, uint32_t (*pattern)(size_t)) {
  Serial.print(label);
  Serial.println(": write");

  const size_t wordsToTest = (static_cast<size_t>(external_psram_size) * 1024UL * 1024UL) / sizeof(uint32_t);
  for (size_t i = 0; i < wordsToTest; ++i) {
    psramWords[i] = pattern(i);
  }

  Serial.print(label);
  Serial.println(": verify");
  for (size_t i = 0; i < wordsToTest; ++i) {
    const uint32_t expected = pattern(i);
    const uint32_t actual = psramWords[i];
    if (actual != expected) {
      Serial.print("FAIL at word ");
      Serial.print(i);
      Serial.print(": expected 0x");
      Serial.print(expected, HEX);
      Serial.print(", got 0x");
      Serial.println(actual, HEX);
      return false;
    }
  }

  Serial.print(label);
  Serial.println(": PASS");
  return true;
}

uint32_t allZero(size_t) {
  return 0x00000000UL;
}

uint32_t allOnes(size_t) {
  return 0xFFFFFFFFUL;
}

uint32_t walkingPattern(size_t index) {
  return patternForIndex(index);
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println("Teensy 4.1 PSRAM memtest");
  Serial.print("external_psram_size = ");
  Serial.print(external_psram_size);
  Serial.println(" MB");

  if (external_psram_size == 0) {
    Serial.println("FAIL: PSRAM not detected. Check chip orientation, small pads, solder bridges, cold joints.");
    return;
  }

  if (external_psram_size > 8) {
    Serial.println("Note: more than 8 MB detected; this test only allocates 8 MB but will use detected size if it fits.");
  }

  const bool ok =
    verifyPattern("zeros", allZero) &&
    verifyPattern("ones", allOnes) &&
    verifyPattern("walking", walkingPattern);

  Serial.println(ok ? "PSRAM TEST PASS" : "PSRAM TEST FAIL");
}

void loop() {
}
