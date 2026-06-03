/*
  The Ooho SD smoke test.

  Hardware:
  - microSD card in the built-in Teensy 4.1 SD slot.

  Behavior:
  - Initializes BUILTIN_SDCARD.
  - Writes TEST.TXT.
  - Reads it back.
  - Deletes it.
  - Built-in LED stays on when the test passes, blinks fast on failure.
*/

#include <SD.h>
#include <SPI.h>

const char TEST_FILE[] = "TEST.TXT";
const char TEST_TEXT[] = "OOHO SD smoke test";

bool passed = false;
uint32_t lastBlinkMs = 0;
bool ledState = false;

void fail(const char *message) {
  Serial.print("FAIL: ");
  Serial.println(message);
  passed = false;
}

bool writeTestFile() {
  if (SD.exists(TEST_FILE)) {
    SD.remove(TEST_FILE);
  }

  FsFile file = SD.sdfs.open(TEST_FILE, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    fail("could not create TEST.TXT");
    return false;
  }

  file.println(TEST_TEXT);
  const bool ok = file.sync();
  file.close();
  if (!ok) {
    fail("could not sync TEST.TXT");
    return false;
  }

  return true;
}

bool readTestFile() {
  FsFile file = SD.sdfs.open(TEST_FILE, O_RDONLY);
  if (!file) {
    fail("could not open TEST.TXT for read");
    return false;
  }

  char buffer[40] = {0};
  const int bytesRead = file.read(buffer, sizeof(buffer) - 1);
  file.close();
  if (bytesRead <= 0) {
    fail("TEST.TXT read was empty");
    return false;
  }

  if (strstr(buffer, TEST_TEXT) == nullptr) {
    Serial.print("Read back: ");
    Serial.println(buffer);
    fail("TEST.TXT contents did not match");
    return false;
  }

  return true;
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho SD smoke test");

  if (!SD.begin(BUILTIN_SDCARD)) {
    fail("SD.begin(BUILTIN_SDCARD) failed");
    return;
  }
  Serial.println("SD init PASS.");

  if (!writeTestFile()) {
    return;
  }
  Serial.println("Write PASS.");

  if (!readTestFile()) {
    return;
  }
  Serial.println("Read PASS.");

  if (!SD.remove(TEST_FILE)) {
    fail("could not delete TEST.TXT");
    return;
  }
  Serial.println("Delete PASS.");

  passed = true;
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("SD SMOKE TEST PASS");
}

void loop() {
  if (passed) {
    digitalWrite(LED_BUILTIN, HIGH);
    return;
  }

  if (millis() - lastBlinkMs >= 120) {
    lastBlinkMs = millis();
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
  }
}
