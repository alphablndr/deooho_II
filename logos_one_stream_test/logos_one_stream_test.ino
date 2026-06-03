/*
  The Ooho LOGOS one-stream playback test.

  Hardware:
  - Teensy 4.1 + Teensy Audio Shield Rev D
  - LINE OUT L -> amplifier -> internal speaker
  - Button between pin 32 and GND
  - Optional external LED on pin 31 through resistor to GND
  - microSD card in built-in Teensy 4.1 slot

  Behavior:
  - Reads GRAINS.CSV rows: filename,start_ms,duration_ms
  - Manually seeks into mono 44.1 kHz 16-bit WAV files and feeds AudioPlayQueue
  - Plays LOGOS grains on left channel only
  - Auto test interval is intentionally short: 4-9 seconds
  - Button press or Serial n = play one random grain immediately
  - Serial: g = grain count, n = next grain, s = stop current grain, a = auto on/off
*/

#include <Audio.h>
#include <SD.h>
#include <SerialFlash.h>
#include <SPI.h>
#include <Wire.h>

constexpr int BUTTON_PIN = 32;
constexpr int STATUS_LED_PIN = 31;
constexpr bool USE_EXTERNAL_STATUS_LED = true;
constexpr uint32_t DEBOUNCE_MS = 35;
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr float OUTPUT_VOLUME = 0.18f;
constexpr float LOGOS_GAIN = 0.28f;
constexpr uint32_t LOGOS_FADE_MS = 8;
constexpr uint32_t AUTO_MIN_INTERVAL_MS = 4000;
constexpr uint32_t AUTO_MAX_INTERVAL_MS = 9000;

const char GRAINS_FILE[] = "GRAINS.CSV";

AudioPlayQueue logosQueue;
AudioOutputI2S i2sOut;
AudioConnection patchCordLeft(logosQueue, 0, i2sOut, 0);
AudioControlSGTL5000 audioShield;

struct GrainInfo {
  char filename[13];
  uint32_t startMs;
  uint32_t durationMs;
};

struct DebouncedButton {
  bool raw = HIGH;
  bool stable = HIGH;
  bool pressedEdge = false;
  uint32_t lastChangeMs = 0;
};

FsFile grainFile;
DebouncedButton button;
GrainInfo currentGrain;
uint32_t grainCount = 0;
uint32_t rngState = 0xC0FFEE42UL;
uint32_t nextAutoMs = 0;
uint32_t samplesRemaining = 0;
uint32_t samplesTotal = 0;
uint32_t samplesPlayed = 0;
bool playing = false;
bool autoEnabled = true;

uint32_t nextRandomU32() {
  rngState = rngState * 1664525UL + 1013904223UL;
  return rngState;
}

uint32_t randomRangeMs(uint32_t minMs, uint32_t maxMs) {
  if (maxMs <= minMs) {
    return minMs;
  }
  return minMs + (nextRandomU32() % (maxMs - minMs + 1));
}

int16_t floatToInt16(float value) {
  if (value > 1.0f) {
    value = 1.0f;
  } else if (value < -1.0f) {
    value = -1.0f;
  }
  return static_cast<int16_t>(value * 32767.0f);
}

void setStatusLed(bool on) {
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
  if (USE_EXTERNAL_STATUS_LED) {
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
  }
}

void scheduleNextAuto() {
  nextAutoMs = millis() + randomRangeMs(AUTO_MIN_INTERVAL_MS, AUTO_MAX_INTERVAL_MS);
}

void updateButton() {
  button.pressedEdge = false;
  const bool raw = digitalRead(BUTTON_PIN);
  const uint32_t now = millis();
  if (raw != button.raw) {
    button.raw = raw;
    button.lastChangeMs = now;
  }
  if ((now - button.lastChangeMs) > DEBOUNCE_MS && raw != button.stable) {
    button.stable = raw;
    button.pressedEdge = button.stable == LOW;
    Serial.println(button.pressedEdge ? "button: next grain" : "button: released");
  }
}

uint32_t countGrains() {
  FsFile file = SD.sdfs.open(GRAINS_FILE, O_RDONLY);
  if (!file) {
    return 0;
  }

  uint32_t count = 0;
  bool hasChars = false;
  while (file.available()) {
    const int c = file.read();
    if (c == '\n') {
      if (hasChars) {
        count++;
      }
      hasChars = false;
    } else if (c != '\r') {
      hasChars = true;
    }
  }
  if (hasChars) {
    count++;
  }
  file.close();
  return count;
}

bool readCsvLine(FsFile &file, char *line, size_t maxLen) {
  size_t used = 0;
  while (file.available()) {
    const int c = file.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      break;
    }
    if (used + 1 < maxLen) {
      line[used++] = static_cast<char>(c);
    }
  }
  line[used] = '\0';
  return used > 0;
}

bool parseGrainLine(const char *line, GrainInfo &grain) {
  char copy[52];
  strncpy(copy, line, sizeof(copy));
  copy[sizeof(copy) - 1] = '\0';

  char *name = strtok(copy, ",");
  char *start = strtok(nullptr, ",");
  char *duration = strtok(nullptr, ",");
  if (!name || !start || !duration) {
    return false;
  }

  strncpy(grain.filename, name, sizeof(grain.filename));
  grain.filename[sizeof(grain.filename) - 1] = '\0';
  grain.startMs = static_cast<uint32_t>(atol(start));
  grain.durationMs = static_cast<uint32_t>(atol(duration));
  return grain.durationMs > 0;
}

bool selectRandomGrain(GrainInfo &grain) {
  grainCount = countGrains();
  if (grainCount == 0) {
    return false;
  }

  const uint32_t target = nextRandomU32() % grainCount;
  FsFile file = SD.sdfs.open(GRAINS_FILE, O_RDONLY);
  if (!file) {
    return false;
  }

  char line[52];
  uint32_t index = 0;
  bool found = false;
  while (readCsvLine(file, line, sizeof(line))) {
    if (index == target) {
      found = parseGrainLine(line, grain);
      break;
    }
    index++;
  }
  file.close();
  return found;
}

void stopGrain() {
  if (grainFile) {
    grainFile.close();
  }
  playing = false;
  samplesRemaining = 0;
  samplesTotal = 0;
  samplesPlayed = 0;
  setStatusLed(false);
}

bool startGrain(const GrainInfo &grain) {
  stopGrain();

  grainFile = SD.sdfs.open(grain.filename, O_RDONLY);
  if (!grainFile) {
    Serial.print("Could not open ");
    Serial.println(grain.filename);
    scheduleNextAuto();
    return false;
  }

  const uint32_t startSample = grain.startMs * (SAMPLE_RATE / 1000UL);
  const uint32_t byteOffset = 44 + startSample * 2UL;
  if (!grainFile.seekSet(byteOffset)) {
    Serial.println("Could not seek WAV grain.");
    stopGrain();
    scheduleNextAuto();
    return false;
  }

  currentGrain = grain;
  samplesTotal = grain.durationMs * (SAMPLE_RATE / 1000UL);
  samplesRemaining = samplesTotal;
  samplesPlayed = 0;
  playing = true;
  setStatusLed(true);

  Serial.print("LOGOS ");
  Serial.print(grain.filename);
  Serial.print(" @ ");
  Serial.print(grain.startMs);
  Serial.print(" ms for ");
  Serial.print(grain.durationMs);
  Serial.println(" ms");
  return true;
}

bool playRandomGrain() {
  GrainInfo grain;
  if (!selectRandomGrain(grain)) {
    Serial.println("No valid grains in GRAINS.CSV.");
    scheduleNextAuto();
    return false;
  }
  return startGrain(grain);
}

void finishGrain() {
  Serial.println("LOGOS grain finished.");
  stopGrain();
  scheduleNextAuto();
}

void feedQueue() {
  if (!playing) {
    return;
  }

  while (samplesRemaining > 0 && logosQueue.available()) {
    int16_t *buffer = logosQueue.getBuffer();
    if (!buffer) {
      return;
    }

    for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
      int16_t sample = 0;
      if (samplesRemaining > 0) {
        uint8_t bytes[2];
        if (grainFile.read(bytes, 2) == 2) {
          sample = static_cast<int16_t>((bytes[1] << 8) | bytes[0]);
        } else {
          samplesRemaining = 0;
        }
      }

      const uint32_t fadeSamples = LOGOS_FADE_MS * (SAMPLE_RATE / 1000UL);
      float gain = LOGOS_GAIN;
      if (fadeSamples > 0 && samplesPlayed < fadeSamples) {
        gain *= static_cast<float>(samplesPlayed) / fadeSamples;
      }
      if (fadeSamples > 0 && samplesRemaining < fadeSamples) {
        gain *= static_cast<float>(samplesRemaining) / fadeSamples;
      }
      buffer[i] = floatToInt16((sample / 32768.0f) * gain);

      if (samplesRemaining > 0) {
        samplesRemaining--;
        samplesPlayed++;
      }
    }
    logosQueue.playBuffer();
  }

  if (samplesRemaining == 0) {
    finishGrain();
  }
}

void printStatus() {
  grainCount = countGrains();
  Serial.print("GRAINS count: ");
  Serial.println(grainCount);
  Serial.print("auto: ");
  Serial.println(autoEnabled ? "on" : "off");
  Serial.print("playing: ");
  Serial.println(playing ? "yes" : "no");
  if (autoEnabled && !playing) {
    const int32_t remaining = static_cast<int32_t>(nextAutoMs - millis());
    Serial.print("next auto in ms: ");
    Serial.println(remaining > 0 ? remaining : 0);
  }
}

void handleSerial() {
  while (Serial.available() > 0) {
    const char command = Serial.read();
    if (command == 'g' || command == 'G') {
      printStatus();
    } else if (command == 'n' || command == 'N') {
      playRandomGrain();
    } else if (command == 's' || command == 'S') {
      stopGrain();
      scheduleNextAuto();
      Serial.println("Stopped current grain.");
    } else if (command == 'a' || command == 'A') {
      autoEnabled = !autoEnabled;
      scheduleNextAuto();
      Serial.println(autoEnabled ? "Auto playback ON." : "Auto playback OFF.");
    }
  }
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  if (USE_EXTERNAL_STATUS_LED) {
    pinMode(STATUS_LED_PIN, OUTPUT);
  }
  setStatusLed(false);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho LOGOS one-stream playback test");

  AudioMemory(80);
  audioShield.enable();
  audioShield.volume(OUTPUT_VOLUME);

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("SD init failed. Use the Teensy 4.1 built-in microSD slot.");
    while (true) {
      setStatusLed(true);
      delay(80);
      setStatusLed(false);
      delay(80);
    }
  }

  rngState ^= micros();
  rngState ^= static_cast<uint32_t>(analogRead(A0)) << 16;
  grainCount = countGrains();
  scheduleNextAuto();

  Serial.println("SD init PASS.");
  printStatus();
  Serial.println("Serial: g=count/status, n=next grain, s=stop, a=auto toggle.");
  Serial.println("Button pin 32 also triggers next random LOGOS grain.");
}

void loop() {
  handleSerial();
  updateButton();
  if (button.pressedEdge) {
    playRandomGrain();
  }

  feedQueue();

  if (autoEnabled && !playing && grainCount > 0 && static_cast<int32_t>(millis() - nextAutoMs) >= 0) {
    playRandomGrain();
  }
}
