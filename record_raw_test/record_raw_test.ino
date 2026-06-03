/*
  The Ooho record raw test.

  Hardware:
  - Teensy 4.1 + Teensy Audio Shield Rev D
  - MAX4466 output -> Audio Shield LINEIN_L
  - Button between pin 32 and GND
  - Optional external LED on pin 31 through resistor to GND
  - microSD card in built-in Teensy 4.1 slot

  Behavior:
  - Hold button: record mono 44.1 kHz 16-bit audio to PSRAM/RAM only.
  - Release button: save Wxxxx.WAV to SD, then append LOGOS candidates to GRAINS.CSV.
  - Max record time: 30 seconds.
  - Serial: r = record toggle, p = play latest full WAV, s = stop.
*/

#include <Audio.h>
#include <SD.h>
#include <SerialFlash.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>

constexpr int RECORD_BUTTON_PIN = 32;
constexpr int STATUS_LED_PIN = 31;
constexpr bool USE_EXTERNAL_STATUS_LED = true;
constexpr uint32_t DEBOUNCE_MS = 35;
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr uint16_t AUDIO_MEMORY_BLOCKS = 200;
constexpr size_t SD_WRITE_CHUNK_BYTES = 4096;
constexpr float STARTUP_VOLUME = 0.18f;
constexpr uint8_t LINE_IN_LEVEL = 7;
constexpr uint32_t INTERNAL_RECORD_SECONDS = 3;
constexpr uint32_t FIELD_RECORD_SECONDS = 30;
constexpr uint32_t MAX_RECORD_MS = FIELD_RECORD_SECONDS * 1000UL;
constexpr size_t INTERNAL_RECORD_BYTES = SAMPLE_RATE * 2UL * INTERNAL_RECORD_SECONDS;
constexpr size_t FIELD_RECORD_BYTES = SAMPLE_RATE * 2UL * FIELD_RECORD_SECONDS;

constexpr uint32_t LOGOS_WINDOW_SAMPLES = 661;
constexpr uint32_t LOGOS_GAP_TOLERANCE_MS = 60;
constexpr uint32_t LOGOS_MIN_DURATION_MS = 120;
constexpr uint32_t LOGOS_MAX_DURATION_MS = 1100;
constexpr uint32_t LOGOS_SPLIT_DURATION_MS = 700;
constexpr uint32_t LOGOS_SPLIT_GAP_MS = 120;
constexpr float LOGOS_MIN_RMS_THRESHOLD = 0.008f;
constexpr float LOGOS_MAX_RMS_THRESHOLD = 0.120f;
constexpr float LOGOS_WINDOW_PEAK_FACTOR = 0.28f;

const char GRAINS_FILE[] = "GRAINS.CSV";

extern "C" uint8_t external_psram_size;

AudioInputI2S i2sIn;
AudioRecordQueue recordQueue;
AudioPlaySdWav playWav;
AudioOutputI2S i2sOut;
AudioConnection patchCord1(i2sIn, 0, recordQueue, 0);
AudioConnection patchCord2(playWav, 0, i2sOut, 0);
AudioConnection patchCord3(playWav, 0, i2sOut, 1);
AudioControlSGTL5000 audioShield;

enum TestState {
  IDLE,
  RECORDING,
  SAVING,
  PLAYING,
  ERROR_STATE
};

struct DebouncedButton {
  int pin;
  const char *name;
  bool raw = HIGH;
  bool stable = HIGH;
  bool pressedEdge = false;
  bool releasedEdge = false;
  uint32_t lastChangeMs = 0;
};

TestState state = IDLE;
DebouncedButton recordButton{RECORD_BUTTON_PIN, "record"};
uint16_t nextIndex = 1;
char currentFilename[13] = "W0000.WAV";
char tempFilename[13] = "T0000.WAV";
char lastFilename[13] = "";

DMAMEM uint8_t internalRecordBuffer[INTERNAL_RECORD_BYTES];
EXTMEM uint8_t psramRecordBuffer[FIELD_RECORD_BYTES];
uint8_t *activeRecordBuffer = internalRecordBuffer;
size_t activeRecordCapacity = INTERNAL_RECORD_BYTES;
uint32_t activeRecordSeconds = INTERNAL_RECORD_SECONDS;
size_t recordBytesUsed = 0;
uint32_t recordingStartedMs = 0;
bool recordOverflow = false;
bool usingPsramBuffer = false;
uint32_t lastLedToggleMs = 0;
bool ledBlinkState = false;

void setStatusLed(bool on) {
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
  if (USE_EXTERNAL_STATUS_LED) {
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
  }
}

void updateStatusLed() {
  const uint32_t now = millis();
  if (state == RECORDING) {
    setStatusLed(true);
  } else if (state == SAVING) {
    if (now - lastLedToggleMs >= 300) {
      lastLedToggleMs = now;
      ledBlinkState = !ledBlinkState;
    }
    setStatusLed(ledBlinkState);
  } else if (state == ERROR_STATE) {
    if (now - lastLedToggleMs >= 120) {
      lastLedToggleMs = now;
      ledBlinkState = !ledBlinkState;
    }
    setStatusLed(ledBlinkState);
  } else {
    setStatusLed(false);
  }
}

void enterState(TestState nextState) {
  state = nextState;
  lastLedToggleMs = millis();
  ledBlinkState = false;
  updateStatusLed();
}

void writeLe16(uint8_t *buffer, uint16_t value) {
  buffer[0] = value & 0xFF;
  buffer[1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t *buffer, uint32_t value) {
  buffer[0] = value & 0xFF;
  buffer[1] = (value >> 8) & 0xFF;
  buffer[2] = (value >> 16) & 0xFF;
  buffer[3] = (value >> 24) & 0xFF;
}

bool writeExact(FsFile &file, const void *data, size_t bytes) {
  const uint8_t *ptr = static_cast<const uint8_t *>(data);
  size_t written = 0;
  while (written < bytes) {
    const size_t chunk = min(SD_WRITE_CHUNK_BYTES, bytes - written);
    const int result = file.write(ptr + written, chunk);
    if (result != static_cast<int>(chunk)) {
      Serial.println("SD write failed or incomplete.");
      return false;
    }
    written += chunk;
    updateStatusLed();
  }
  return true;
}

bool writeWavHeader(FsFile &file, uint32_t dataBytes) {
  uint8_t header[44] = {0};
  const uint16_t channels = 1;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate = SAMPLE_RATE * channels * bitsPerSample / 8;
  const uint16_t blockAlign = channels * bitsPerSample / 8;

  memcpy(header + 0, "RIFF", 4);
  writeLe32(header + 4, 36 + dataBytes);
  memcpy(header + 8, "WAVE", 4);
  memcpy(header + 12, "fmt ", 4);
  writeLe32(header + 16, 16);
  writeLe16(header + 20, 1);
  writeLe16(header + 22, channels);
  writeLe32(header + 24, SAMPLE_RATE);
  writeLe32(header + 28, byteRate);
  writeLe16(header + 32, blockAlign);
  writeLe16(header + 34, bitsPerSample);
  memcpy(header + 36, "data", 4);
  writeLe32(header + 40, dataBytes);

  return writeExact(file, header, sizeof(header));
}

void makeFilename(char *buffer, uint16_t index) {
  snprintf(buffer, 13, "W%04u.WAV", index);
}

void makeTempFilename(char *buffer, uint16_t index) {
  snprintf(buffer, 13, "T%04u.WAV", index);
}

uint16_t scanNextIndex() {
  char name[13];
  for (uint16_t i = 1; i < 10000; ++i) {
    makeFilename(name, i);
    if (!SD.exists(name)) {
      return i;
    }
  }
  return 9999;
}

uint16_t findLatestIndex() {
  char name[13];
  for (int i = 9999; i >= 1; --i) {
    makeFilename(name, static_cast<uint16_t>(i));
    if (SD.exists(name)) {
      return static_cast<uint16_t>(i);
    }
  }
  return 0;
}

void updateButton(DebouncedButton &button) {
  button.pressedEdge = false;
  button.releasedEdge = false;

  const bool raw = digitalRead(button.pin);
  const uint32_t now = millis();
  if (raw != button.raw) {
    button.raw = raw;
    button.lastChangeMs = now;
  }

  if ((now - button.lastChangeMs) > DEBOUNCE_MS && raw != button.stable) {
    button.stable = raw;
    button.pressedEdge = button.stable == LOW;
    button.releasedEdge = button.stable == HIGH;
    Serial.print("Button ");
    Serial.print(button.name);
    Serial.println(button.pressedEdge ? " pressed" : " released");
  }
}

void drainRecordQueue() {
  while (recordQueue.available() > 0) {
    const uint8_t *block = reinterpret_cast<const uint8_t *>(recordQueue.readBuffer());
    if (recordBytesUsed + 256 <= activeRecordCapacity) {
      memcpy(activeRecordBuffer + recordBytesUsed, block, 256);
      recordBytesUsed += 256;
    } else {
      recordOverflow = true;
    }
    recordQueue.freeBuffer();
  }
}

bool appendGrain(const char *filename, uint32_t startMs, uint32_t durationMs) {
  FsFile file = SD.sdfs.open(GRAINS_FILE, O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    Serial.println("Could not open GRAINS.CSV.");
    return false;
  }
  file.print(filename);
  file.print(',');
  file.print(startMs);
  file.print(',');
  file.println(durationMs);
  const bool ok = file.sync();
  file.close();
  return ok;
}

uint32_t appendRegionGrains(const char *filename, uint32_t startSample, uint32_t endSample) {
  if (endSample <= startSample) {
    return 0;
  }

  const uint32_t startMs = startSample * 1000UL / SAMPLE_RATE;
  const uint32_t durationMs = (endSample - startSample) * 1000UL / SAMPLE_RATE;
  if (durationMs < LOGOS_MIN_DURATION_MS) {
    return 0;
  }

  if (durationMs <= LOGOS_MAX_DURATION_MS) {
    return appendGrain(filename, startMs, durationMs) ? 1 : 0;
  }

  uint32_t added = 0;
  uint32_t cursorMs = startMs;
  const uint32_t endMs = startMs + durationMs;
  while (cursorMs + LOGOS_MIN_DURATION_MS <= endMs) {
    const uint32_t remainingMs = endMs - cursorMs;
    const uint32_t chunkMs = remainingMs < LOGOS_SPLIT_DURATION_MS ? remainingMs : LOGOS_SPLIT_DURATION_MS;
    if (chunkMs >= LOGOS_MIN_DURATION_MS) {
      if (appendGrain(filename, cursorMs, chunkMs)) {
        added++;
      }
    }
    if (remainingMs <= LOGOS_SPLIT_DURATION_MS + LOGOS_SPLIT_GAP_MS) {
      break;
    }
    cursorMs += LOGOS_SPLIT_DURATION_MS + LOGOS_SPLIT_GAP_MS;
  }
  return added;
}

void appendLogosGrains() {
  const int16_t *samples = reinterpret_cast<const int16_t *>(activeRecordBuffer);
  const uint32_t totalSamples = recordBytesUsed / 2;
  if (totalSamples < LOGOS_WINDOW_SAMPLES) {
    Serial.println("LOGOS grains added: 0");
    return;
  }

  double totalEnergy = 0.0;
  float peak = 0.0f;
  float maxWindowRms = 0.0f;
  uint32_t windowCount = 0;

  for (uint32_t i = 0; i < totalSamples; ++i) {
    const float s = samples[i] / 32768.0f;
    const float absS = fabsf(s);
    totalEnergy += static_cast<double>(s) * s;
    if (absS > peak) {
      peak = absS;
    }
  }

  for (uint32_t start = 0; start + LOGOS_WINDOW_SAMPLES <= totalSamples; start += LOGOS_WINDOW_SAMPLES) {
    double sumSquares = 0.0;
    for (uint32_t i = 0; i < LOGOS_WINDOW_SAMPLES; ++i) {
      const float s = samples[start + i] / 32768.0f;
      sumSquares += static_cast<double>(s) * s;
    }
    const float windowRms = sqrtf(sumSquares / LOGOS_WINDOW_SAMPLES);
    if (windowRms > maxWindowRms) {
      maxWindowRms = windowRms;
    }
    windowCount++;
  }

  float adaptiveThreshold = maxWindowRms * LOGOS_WINDOW_PEAK_FACTOR;
  adaptiveThreshold = constrain(adaptiveThreshold, LOGOS_MIN_RMS_THRESHOLD, LOGOS_MAX_RMS_THRESHOLD);

  const float overallRms = sqrtf(totalEnergy / totalSamples);
  Serial.print("LOGOS analysis: duration_ms=");
  Serial.print(totalSamples * 1000UL / SAMPLE_RATE);
  Serial.print(", rms=");
  Serial.print(overallRms, 5);
  Serial.print(", peak=");
  Serial.print(peak, 5);
  Serial.print(", max_window_rms=");
  Serial.print(maxWindowRms, 5);
  Serial.print(", threshold=");
  Serial.print(adaptiveThreshold, 5);
  Serial.print(", windows=");
  Serial.println(windowCount);

  const uint32_t gapToleranceSamples = LOGOS_GAP_TOLERANCE_MS * (SAMPLE_RATE / 1000UL);
  uint32_t regionStart = 0;
  uint32_t regionEnd = 0;
  uint32_t lastVoicedEnd = 0;
  bool inRegion = false;
  uint32_t added = 0;

  for (uint32_t start = 0; start + LOGOS_WINDOW_SAMPLES <= totalSamples; start += LOGOS_WINDOW_SAMPLES) {
    double sumSquares = 0.0;
    for (uint32_t i = 0; i < LOGOS_WINDOW_SAMPLES; ++i) {
      const float s = samples[start + i] / 32768.0f;
      sumSquares += static_cast<double>(s) * s;
    }
    const float rms = sqrtf(sumSquares / LOGOS_WINDOW_SAMPLES);
    const bool voiced = rms > adaptiveThreshold;
    const uint32_t windowEnd = start + LOGOS_WINDOW_SAMPLES;

    if (voiced) {
      if (!inRegion) {
        regionStart = start;
        inRegion = true;
      } else if (start > lastVoicedEnd + gapToleranceSamples) {
        added += appendRegionGrains(currentFilename, regionStart, regionEnd);
        regionStart = start;
      }
      regionEnd = windowEnd;
      lastVoicedEnd = windowEnd;
    } else if (inRegion && start > lastVoicedEnd + gapToleranceSamples) {
      added += appendRegionGrains(currentFilename, regionStart, regionEnd);
      inRegion = false;
    }
  }

  if (inRegion) {
    added += appendRegionGrains(currentFilename, regionStart, regionEnd);
  }

  Serial.print("LOGOS grains added: ");
  Serial.println(added);
}

bool saveRecording() {
  makeFilename(currentFilename, nextIndex);
  makeTempFilename(tempFilename, nextIndex);

  if (SD.exists(tempFilename)) {
    SD.remove(tempFilename);
  }
  if (SD.exists(currentFilename)) {
    Serial.print("Refusing to overwrite existing ");
    Serial.println(currentFilename);
    return false;
  }

  FsFile file = SD.sdfs.open(tempFilename, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    Serial.print("Could not open ");
    Serial.println(tempFilename);
    return false;
  }

  const uint32_t wavBytes = 44 + recordBytesUsed;
  if (!file.preAllocate(wavBytes)) {
    Serial.println("Warning: raw WAV preAllocate failed.");
  }

  const bool ok = writeWavHeader(file, recordBytesUsed) &&
                  writeExact(file, activeRecordBuffer, recordBytesUsed) &&
                  file.truncate(wavBytes) &&
                  file.sync();
  file.close();
  if (!ok) {
    Serial.println("Raw WAV save failed.");
    return false;
  }

  if (!SD.rename(tempFilename, currentFilename)) {
    Serial.print("Could not rename ");
    Serial.print(tempFilename);
    Serial.print(" to ");
    Serial.println(currentFilename);
    return false;
  }

  strncpy(lastFilename, currentFilename, sizeof(lastFilename));
  lastFilename[sizeof(lastFilename) - 1] = '\0';
  Serial.print("Saved ");
  Serial.print(lastFilename);
  Serial.print(" (");
  Serial.print(recordBytesUsed);
  Serial.println(" bytes audio).");
  return true;
}

void stopPlayback() {
  if (playWav.isPlaying()) {
    playWav.stop();
  }
  enterState(IDLE);
  Serial.println("Playback stopped.");
}

void startPlayback() {
  if (!lastFilename[0]) {
    Serial.println("No latest WAV to play.");
    return;
  }
  if (playWav.isPlaying()) {
    playWav.stop();
    delay(5);
  }
  Serial.print("Playing ");
  Serial.println(lastFilename);
  playWav.play(lastFilename);
  delay(10);
  enterState(PLAYING);
}

void startRecording() {
  if (state != IDLE) {
    return;
  }
  if (playWav.isPlaying()) {
    playWav.stop();
    delay(5);
  }
  recordBytesUsed = 0;
  recordingStartedMs = millis();
  recordOverflow = false;
  AudioProcessorUsageMaxReset();
  AudioMemoryUsageMaxReset();
  recordQueue.begin();
  enterState(RECORDING);
  Serial.print("Recording to ");
  Serial.print(usingPsramBuffer ? "PSRAM" : "internal RAM fallback");
  Serial.print(", max ");
  Serial.print(activeRecordSeconds);
  Serial.println(" sec.");
}

void stopRecording() {
  recordQueue.end();
  drainRecordQueue();
  enterState(SAVING);

  Serial.print("Recording stopped: ");
  Serial.print(recordBytesUsed);
  Serial.print("/");
  Serial.print(activeRecordCapacity);
  Serial.println(recordOverflow ? " bytes, overflow clipped." : " bytes.");

  if (recordBytesUsed == 0) {
    Serial.println("Empty recording ignored.");
    enterState(IDLE);
    return;
  }
  if (!saveRecording()) {
    enterState(ERROR_STATE);
    return;
  }
  appendLogosGrains();

  if (nextIndex < 9999) {
    nextIndex++;
  }

  Serial.print("Audio CPU max: ");
  Serial.print(AudioProcessorUsageMax());
  Serial.print("%, memory max blocks: ");
  Serial.println(AudioMemoryUsageMax());
  enterState(IDLE);
}

void handleSerial() {
  while (Serial.available() > 0) {
    const char command = Serial.read();
    if (command == 'r' || command == 'R') {
      if (state == RECORDING) {
        stopRecording();
      } else if (state == IDLE) {
        startRecording();
      }
    } else if (command == 'p' || command == 'P') {
      startPlayback();
    } else if (command == 's' || command == 'S') {
      if (state == RECORDING) {
        stopRecording();
      } else {
        stopPlayback();
      }
    }
  }
}

void handleButton() {
  if (recordButton.pressedEdge && state == IDLE) {
    startRecording();
  }
  if (recordButton.releasedEdge && state == RECORDING) {
    stopRecording();
  }
}

void setup() {
  pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  if (USE_EXTERNAL_STATUS_LED) {
    pinMode(STATUS_LED_PIN, OUTPUT);
  }
  setStatusLed(false);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho record raw test");

  AudioMemory(AUDIO_MEMORY_BLOCKS);
  audioShield.enable();
  audioShield.adcHighPassFilterDisable();
  audioShield.volume(STARTUP_VOLUME);
  audioShield.inputSelect(AUDIO_INPUT_LINEIN);
  audioShield.lineInLevel(LINE_IN_LEVEL);

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("SD init failed. Use the Teensy 4.1 built-in microSD slot.");
    enterState(ERROR_STATE);
    return;
  }
  Serial.println("SD init PASS.");

  if (external_psram_size > 0) {
    activeRecordBuffer = psramRecordBuffer;
    activeRecordCapacity = min(static_cast<size_t>(external_psram_size) * 1024UL * 1024UL, FIELD_RECORD_BYTES);
    activeRecordSeconds = activeRecordCapacity / (SAMPLE_RATE * 2UL);
    usingPsramBuffer = true;
  } else {
    Serial.println("Warning: PSRAM not detected; internal RAM fallback is for bench testing only.");
  }

  nextIndex = scanNextIndex();
  const uint16_t latestIndex = findLatestIndex();
  if (latestIndex > 0) {
    makeFilename(lastFilename, latestIndex);
  }

  Serial.print("Record buffer: ");
  Serial.print(activeRecordCapacity);
  Serial.print(" bytes, max ");
  Serial.print(activeRecordSeconds);
  Serial.println(usingPsramBuffer ? " sec in PSRAM." : " sec in internal RAM fallback.");
  Serial.print("Next recording: ");
  makeFilename(currentFilename, nextIndex);
  Serial.println(currentFilename);
  Serial.print("Latest playback: ");
  Serial.println(lastFilename[0] ? lastFilename : "(none)");
  Serial.println("Hold pin 32 button to record; release to save WAV.");
  Serial.println("Serial: r=record toggle, p=play latest, s=stop.");
}

void loop() {
  handleSerial();
  updateButton(recordButton);
  handleButton();

  if (state == RECORDING) {
    drainRecordQueue();
    if (millis() - recordingStartedMs >= MAX_RECORD_MS) {
      Serial.println("Max recording time reached; auto-stopping recording.");
      stopRecording();
    } else if (recordBytesUsed >= activeRecordCapacity) {
      recordOverflow = true;
      stopRecording();
    }
  }

  if (state == PLAYING && !playWav.isPlaying()) {
    enterState(IDLE);
    Serial.println("Playback finished.");
  }

  updateStatusLed();
}
