/*
  The Ooho simple random archive.

  Hardware:
  - Teensy 4.1 + Teensy Audio Shield Rev D
  - MAX4466 output -> Audio Shield LINEIN_L
  - Record button between pin 32 and GND
  - Playback button between pin 30 and GND
  - Optional external LED on pin 31 through resistor to GND
  - Optional playback LED on pin 29 through resistor to GND
  - microSD card in built-in Teensy 4.1 slot

  Behavior:
  - Hold button: record mono 44.1 kHz 16-bit audio to PSRAM/RAM only.
  - Release button: save Wxxxx.WAV to SD.
  - Press playback button: randomly choose one saved Wxxxx.WAV from the whole archive and play it.
  - Max record time: 30 seconds.
  - Serial: r = record toggle, p = random playback, s = stop.
*/

#include <Audio.h>
#include <SD.h>
#include <SerialFlash.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>

constexpr int RECORD_BUTTON_PIN = 32;
constexpr int PLAY_BUTTON_PIN = 30;
constexpr int STATUS_LED_PIN = 31;
constexpr int PLAYBACK_LED_PIN = 29;
constexpr bool USE_EXTERNAL_STATUS_LED = true;
constexpr bool USE_EXTERNAL_PLAYBACK_LED = true;
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
constexpr float INPUT_HIGHPASS_HZ = 85.0f;
constexpr float INPUT_LOWPASS_HZ = 7600.0f;
constexpr float NOISE_FLOOR_THRESHOLD = 0.012f;
constexpr float NOISE_FLOOR_GAIN = 0.42f;
constexpr float COMPRESSOR_THRESHOLD = 0.20f;
constexpr float COMPRESSOR_RATIO = 2.2f;
constexpr float MAKEUP_GAIN = 1.16f;
constexpr float SOFT_LIMIT = 0.94f;
constexpr uint32_t MIN_SAVE_MS = 500;
constexpr float MIN_SAVE_RMS = 0.006f;
constexpr float MIN_SAVE_PEAK = 0.025f;
constexpr float NORMALIZE_TARGET_RMS = 0.115f;
constexpr float NORMALIZE_TARGET_PEAK = 0.82f;
constexpr float NORMALIZE_MAX_GAIN = 3.2f;
constexpr float NORMALIZE_MIN_GAIN = 0.62f;
constexpr float PLAYBACK_GAIN = 1.35f;

extern "C" uint8_t external_psram_size;

AudioInputI2S i2sIn;
AudioRecordQueue recordQueue;
AudioPlaySdWav playWav;
AudioAmplifier playbackAmpL;
AudioAmplifier playbackAmpR;
AudioOutputI2S i2sOut;
AudioConnection patchCord1(i2sIn, 0, recordQueue, 0);
AudioConnection patchCord2(playWav, 0, playbackAmpL, 0);
AudioConnection patchCord3(playWav, 0, playbackAmpR, 0);
AudioConnection patchCord4(playbackAmpL, 0, i2sOut, 0);
AudioConnection patchCord5(playbackAmpR, 0, i2sOut, 1);
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

struct RecordingStats {
  float rms = 0.0f;
  float peak = 0.0f;
  float dc = 0.0f;
  uint32_t durationMs = 0;
};

TestState state = IDLE;
DebouncedButton recordButton{RECORD_BUTTON_PIN, "record"};
DebouncedButton playButton{PLAY_BUTTON_PIN, "play"};
uint16_t nextIndex = 1;
char currentFilename[13] = "W0000.WAV";
char tempFilename[13] = "T0000.WAV";
char lastFilename[13] = "";
char playbackFilename[13] = "";
char lastPlayedFilename[13] = "";

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

void setPlaybackLed(bool on) {
  if (USE_EXTERNAL_PLAYBACK_LED) {
    digitalWrite(PLAYBACK_LED_PIN, on ? HIGH : LOW);
  }
}

void updateStatusLed() {
  const uint32_t now = millis();
  if (state == RECORDING) {
    setStatusLed(true);
    setPlaybackLed(false);
  } else if (state == SAVING) {
    if (now - lastLedToggleMs >= 300) {
      lastLedToggleMs = now;
      ledBlinkState = !ledBlinkState;
    }
    setStatusLed(ledBlinkState);
    setPlaybackLed(false);
  } else if (state == PLAYING) {
    setStatusLed(false);
    setPlaybackLed(true);
  } else if (state == ERROR_STATE) {
    if (now - lastLedToggleMs >= 120) {
      lastLedToggleMs = now;
      ledBlinkState = !ledBlinkState;
    }
    setStatusLed(ledBlinkState);
    setPlaybackLed(ledBlinkState);
  } else {
    setStatusLed(false);
    setPlaybackLed(false);
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

uint16_t countArchivedRecordings() {
  char name[13];
  uint16_t count = 0;
  for (uint16_t i = 1; i < 10000; ++i) {
    makeFilename(name, i);
    if (SD.exists(name)) {
      count++;
    }
  }
  return count;
}

bool archivedRecordingByOrdinal(uint16_t target, char *buffer) {
  char name[13];
  for (uint16_t i = 1; i < 10000; ++i) {
    makeFilename(name, i);
    if (!SD.exists(name)) {
      continue;
    }
    if (target == 0) {
      strncpy(buffer, name, 13);
      buffer[12] = '\0';
      return true;
    }
    target--;
  }

  buffer[0] = '\0';
  return false;
}

bool chooseRandomRecording(char *buffer) {
  const uint16_t count = countArchivedRecordings();
  if (count == 0) {
    buffer[0] = '\0';
    return false;
  }

  uint16_t target = static_cast<uint16_t>(random(count));
  if (!archivedRecordingByOrdinal(target, buffer)) {
    return false;
  }

  if (count > 1 && strcmp(buffer, lastPlayedFilename) == 0) {
    target = (target + 1) % count;
    return archivedRecordingByOrdinal(target, buffer);
  }

  return true;
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

float clampAudio(float value) {
  if (value > SOFT_LIMIT) {
    return SOFT_LIMIT;
  }
  if (value < -SOFT_LIMIT) {
    return -SOFT_LIMIT;
  }
  return value;
}

RecordingStats analyzeRecording() {
  const int16_t *samples = reinterpret_cast<const int16_t *>(activeRecordBuffer);
  const uint32_t totalSamples = recordBytesUsed / 2;
  RecordingStats stats;
  stats.durationMs = totalSamples * 1000UL / SAMPLE_RATE;
  if (totalSamples == 0) {
    return stats;
  }

  double sum = 0.0;
  double sumSquares = 0.0;
  float peak = 0.0f;
  for (uint32_t i = 0; i < totalSamples; ++i) {
    const float x = samples[i] / 32768.0f;
    sum += x;
    sumSquares += static_cast<double>(x) * x;
    peak = max(peak, fabsf(x));
  }

  stats.dc = sum / totalSamples;
  stats.rms = sqrtf(sumSquares / totalSamples);
  stats.peak = peak;
  return stats;
}

bool recordingIsWorthSaving(const RecordingStats &stats) {
  if (stats.durationMs < MIN_SAVE_MS) {
    Serial.print("Recording ignored: too short (");
    Serial.print(stats.durationMs);
    Serial.println(" ms).");
    return false;
  }
  if (stats.rms < MIN_SAVE_RMS && stats.peak < MIN_SAVE_PEAK) {
    Serial.print("Recording ignored: too quiet/noisy only, rms=");
    Serial.print(stats.rms, 5);
    Serial.print(", peak=");
    Serial.println(stats.peak, 5);
    return false;
  }
  return true;
}

RecordingStats normalizeRecording(const RecordingStats &stats) {
  int16_t *samples = reinterpret_cast<int16_t *>(activeRecordBuffer);
  const uint32_t totalSamples = recordBytesUsed / 2;
  if (totalSamples == 0 || stats.rms <= 0.0f || stats.peak <= 0.0f) {
    return stats;
  }

  float gain = NORMALIZE_TARGET_RMS / stats.rms;
  gain = min(gain, NORMALIZE_TARGET_PEAK / stats.peak);
  gain = constrain(gain, NORMALIZE_MIN_GAIN, NORMALIZE_MAX_GAIN);

  if (fabsf(gain - 1.0f) < 0.03f) {
    Serial.println("Normalize gain: 1.00");
    return stats;
  }

  for (uint32_t i = 0; i < totalSamples; ++i) {
    const float x = samples[i] / 32768.0f;
    const float y = clampAudio(x * gain);
    samples[i] = static_cast<int16_t>(roundf(y * 32767.0f));
  }

  const RecordingStats normalized = analyzeRecording();
  Serial.print("Normalize gain: ");
  Serial.print(gain, 2);
  Serial.print(", rms=");
  Serial.print(stats.rms, 5);
  Serial.print(" -> ");
  Serial.print(normalized.rms, 5);
  Serial.print(", peak=");
  Serial.print(stats.peak, 5);
  Serial.print(" -> ");
  Serial.println(normalized.peak, 5);
  return normalized;
}

void postProcessRecording() {
  int16_t *samples = reinterpret_cast<int16_t *>(activeRecordBuffer);
  const uint32_t totalSamples = recordBytesUsed / 2;
  if (totalSamples == 0) {
    return;
  }

  double rawSum = 0.0;
  float rawPeak = 0.0f;
  for (uint32_t i = 0; i < totalSamples; ++i) {
    const float x = samples[i] / 32768.0f;
    rawSum += x;
    rawPeak = max(rawPeak, fabsf(x));
  }

  const float twoPi = 6.28318530718f;
  const float highpassAlpha = expf(-twoPi * INPUT_HIGHPASS_HZ / SAMPLE_RATE);
  const float lowpassAlpha = 1.0f - expf(-twoPi * INPUT_LOWPASS_HZ / SAMPLE_RATE);
  const float envAttack = 1.0f - expf(-1.0f / (0.006f * SAMPLE_RATE));
  const float envRelease = 1.0f - expf(-1.0f / (0.140f * SAMPLE_RATE));

  float prevInput = 0.0f;
  float highpassed = 0.0f;
  float lowpassed = 0.0f;
  float envelope = 0.0f;
  float processedPeak = 0.0f;

  for (uint32_t i = 0; i < totalSamples; ++i) {
    const float input = samples[i] / 32768.0f;

    highpassed = highpassAlpha * (highpassed + input - prevInput);
    prevInput = input;

    lowpassed += lowpassAlpha * (highpassed - lowpassed);
    float y = lowpassed;

    const float absY = fabsf(y);
    const float envCoef = absY > envelope ? envAttack : envRelease;
    envelope += envCoef * (absY - envelope);

    if (envelope < NOISE_FLOOR_THRESHOLD) {
      const float floorMix = envelope / NOISE_FLOOR_THRESHOLD;
      const float gateGain = NOISE_FLOOR_GAIN + (1.0f - NOISE_FLOOR_GAIN) * floorMix;
      y *= gateGain;
    }

    const float level = fabsf(y);
    if (level > COMPRESSOR_THRESHOLD) {
      const float compressedLevel = COMPRESSOR_THRESHOLD + (level - COMPRESSOR_THRESHOLD) / COMPRESSOR_RATIO;
      y *= compressedLevel / level;
    }

    y = clampAudio(y * MAKEUP_GAIN);
    processedPeak = max(processedPeak, fabsf(y));
    samples[i] = static_cast<int16_t>(roundf(y * 32767.0f));
  }

  Serial.print("Voice cleanup: dc=");
  Serial.print(rawSum / totalSamples, 6);
  Serial.print(", peak_in=");
  Serial.print(rawPeak, 5);
  Serial.print(", peak_out=");
  Serial.println(processedPeak, 5);
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
  if (!chooseRandomRecording(playbackFilename)) {
    Serial.println("No archived WAV files to play.");
    return;
  }
  if (playWav.isPlaying()) {
    playWav.stop();
    delay(5);
  }
  Serial.print("Playing ");
  Serial.println(playbackFilename);
  if (playWav.play(playbackFilename)) {
    strncpy(lastPlayedFilename, playbackFilename, sizeof(lastPlayedFilename));
    lastPlayedFilename[sizeof(lastPlayedFilename) - 1] = '\0';
    delay(10);
    enterState(PLAYING);
  } else {
    Serial.println("Playback start failed.");
    enterState(IDLE);
  }
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
  postProcessRecording();
  RecordingStats stats = analyzeRecording();
  if (!recordingIsWorthSaving(stats)) {
    enterState(IDLE);
    return;
  }
  stats = normalizeRecording(stats);
  Serial.print("Recording accepted: duration_ms=");
  Serial.print(stats.durationMs);
  Serial.print(", rms=");
  Serial.print(stats.rms, 5);
  Serial.print(", peak=");
  Serial.println(stats.peak, 5);
  if (!saveRecording()) {
    enterState(ERROR_STATE);
    return;
  }

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
  if (playButton.pressedEdge && (state == IDLE || state == PLAYING)) {
    startPlayback();
  }
}

void setup() {
  pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);
  pinMode(PLAY_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  if (USE_EXTERNAL_STATUS_LED) {
    pinMode(STATUS_LED_PIN, OUTPUT);
  }
  if (USE_EXTERNAL_PLAYBACK_LED) {
    pinMode(PLAYBACK_LED_PIN, OUTPUT);
  }
  setStatusLed(false);
  setPlaybackLed(false);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho simple random archive");

  AudioMemory(AUDIO_MEMORY_BLOCKS);
  audioShield.enable();
  audioShield.adcHighPassFilterEnable();
  audioShield.volume(STARTUP_VOLUME);
  audioShield.inputSelect(AUDIO_INPUT_LINEIN);
  audioShield.lineInLevel(LINE_IN_LEVEL);
  playbackAmpL.gain(PLAYBACK_GAIN);
  playbackAmpR.gain(PLAYBACK_GAIN);

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
  randomSeed(micros() ^ (static_cast<uint32_t>(analogRead(A0)) << 16));

  Serial.print("Record buffer: ");
  Serial.print(activeRecordCapacity);
  Serial.print(" bytes, max ");
  Serial.print(activeRecordSeconds);
  Serial.println(usingPsramBuffer ? " sec in PSRAM." : " sec in internal RAM fallback.");
  Serial.print("Next recording: ");
  makeFilename(currentFilename, nextIndex);
  Serial.println(currentFilename);
  Serial.print("Latest archive file: ");
  Serial.println(lastFilename[0] ? lastFilename : "(none)");
  Serial.println("Hold pin 32 button to record; release to save WAV.");
  Serial.println("Press pin 30 button to play a random archived WAV.");
  Serial.println("Serial: r=record toggle, p=random play, s=stop.");
}

void loop() {
  handleSerial();
  updateButton(recordButton);
  updateButton(playButton);
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
