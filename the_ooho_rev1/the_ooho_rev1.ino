/*
  The Ooho II - Field v1 LOGOS/MELOS firmware

  Hardware:
  - Teensy 4.1 + Teensy Audio Shield Rev D
  - MAX4466 mic preamp -> Audio Shield LINEIN_L
  - LINE OUT L -> amplifier -> internal speaker       (LOGOS)
  - LINE OUT R -> amplifier -> surface exciter/cube   (MELOS)
  - Public record button between pin 32 and GND
  - Optional status LED on pin 31 through 220-1000 ohm resistor to GND

  Critical rule:
  - Never write to SD while recording. Recording fills RAM/PSRAM only.
  - SD writes, analysis, model update, and MELOS rendering happen after release.

  Serial debug:
  - r = start/stop recording
  - s = stop recording or stop all playback
  - t = diagnostic tone on/off
  - m = print model summary
  - g = print grain count and next LOGOS timing
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
constexpr uint16_t AUDIO_MEMORY_BLOCKS = 220;
constexpr size_t SD_WRITE_CHUNK_BYTES = 4096;
constexpr float STARTUP_VOLUME = 0.18f;
constexpr uint8_t LINE_IN_LEVEL = 7;
constexpr bool USE_MAX4466_LINEIN = true;

constexpr uint32_t INTERNAL_RECORD_SECONDS = 3;
constexpr uint32_t FIELD_RECORD_SECONDS = 30;
constexpr uint32_t MAX_RECORD_MS = FIELD_RECORD_SECONDS * 1000UL;
constexpr size_t INTERNAL_RECORD_BYTES = SAMPLE_RATE * 2UL * INTERNAL_RECORD_SECONDS;
constexpr size_t FIELD_RECORD_BYTES = SAMPLE_RATE * 2UL * FIELD_RECORD_SECONDS;

constexpr uint32_t MODEL_MAGIC = 0x4F4F484FUL;  // "OOHO"
constexpr uint16_t MODEL_VERSION = 1;
constexpr uint8_t ENV_BINS = 64;
constexpr uint8_t BAND_COUNT = 8;
constexpr float MODEL_WEIGHT = 0.10f;
constexpr float DEFAULT_F0_HZ = 160.0f;
constexpr float DEFAULT_VOICED_RATIO = 0.5f;
constexpr float DEFAULT_RMS = 0.08f;

constexpr uint32_t MELOS_SECONDS = 30;
constexpr uint32_t MELOS_SAMPLES = SAMPLE_RATE * MELOS_SECONDS;
constexpr float MELOS_GAIN = 0.60f;
constexpr float LOGOS_STREAM_GAIN = 0.28f;
constexpr uint32_t MELOS_FADE_MS = 400;
constexpr uint32_t LOGOS_FADE_MS = 8;

constexpr uint8_t LOGOS_STREAM_COUNT = 3;
constexpr uint32_t LOGOS_STREAM_1_MIN_INTERVAL_MS = 45000;
constexpr uint32_t LOGOS_STREAM_1_MAX_INTERVAL_MS = 90000;
constexpr uint32_t LOGOS_DORMANT_MIN_INTERVAL_MS = 60000;
constexpr uint32_t LOGOS_DORMANT_MAX_INTERVAL_MS = 120000;
constexpr uint32_t LOGOS_ACTIVE_MIN_INTERVAL_MS = 2000;
constexpr uint32_t LOGOS_ACTIVE_MAX_INTERVAL_MS = 8000;
constexpr uint32_t LOGOS_STATE_MIN_MS = 30000;
constexpr uint32_t LOGOS_STATE_MAX_MS = 90000;
constexpr uint8_t LOGOS_REPEAT_CHANCE_PERCENT = 25;
constexpr uint32_t LOGOS_REPEAT_MIN_OFFSET_MS = 80;
constexpr uint32_t LOGOS_REPEAT_MAX_OFFSET_MS = 200;
constexpr uint32_t LOGOS_WINDOW_SAMPLES = 661;      // about 15 ms at 44.1 kHz
constexpr uint32_t LOGOS_GAP_TOLERANCE_MS = 60;
constexpr uint32_t LOGOS_MIN_DURATION_MS = 120;
constexpr uint32_t LOGOS_MAX_DURATION_MS = 1100;
constexpr float LOGOS_RMS_THRESHOLD = 0.016f;

constexpr uint32_t ANALYSIS_RATE = 11025;
constexpr uint16_t F0_FRAME_SAMPLES = 1024;
constexpr uint16_t F0_HOP_SAMPLES = 512;
constexpr float F0_RMS_THRESHOLD = 0.012f;
constexpr float F0_CONFIDENCE_THRESHOLD = 0.28f;
constexpr float F0_MIN_HZ = 70.0f;
constexpr float F0_MAX_HZ = 350.0f;
constexpr uint16_t F0_MIN_LAG = static_cast<uint16_t>(ANALYSIS_RATE / F0_MAX_HZ);
constexpr uint16_t F0_MAX_LAG = static_cast<uint16_t>(ANALYSIS_RATE / F0_MIN_HZ);
constexpr uint16_t MAX_F0_VALUES = 256;

const char COUNTER_FILE[] = "COUNTER.TXT";
const char GRAINS_FILE[] = "GRAINS.CSV";
const char MODEL_FILE[] = "MODEL.BIN";
const char MODEL_TEMP_FILE[] = "MODEL.TMP";
const char MELOS_FILE[] = "MELOS.WAV";
const char MELOS_TEMP_FILE[] = "MTEMP.WAV";

extern "C" uint8_t external_psram_size;

AudioInputI2S i2sIn;
AudioRecordQueue recordQueue;
AudioPlayQueue logosQueue1;
AudioPlayQueue logosQueue2;
AudioPlayQueue logosQueue3;
AudioPlayQueue melosQueue;
AudioSynthWaveform diagnosticTone;
AudioMixer4 leftMixer;
AudioMixer4 rightMixer;
AudioOutputI2S i2sOut;

AudioConnection patchCord1(i2sIn, 0, recordQueue, 0);
AudioConnection patchCord2(logosQueue1, 0, leftMixer, 0);
AudioConnection patchCord3(logosQueue2, 0, leftMixer, 1);
AudioConnection patchCord4(logosQueue3, 0, leftMixer, 2);
AudioConnection patchCord5(diagnosticTone, 0, leftMixer, 3);
AudioConnection patchCord6(melosQueue, 0, rightMixer, 0);
AudioConnection patchCord7(diagnosticTone, 0, rightMixer, 1);
AudioConnection patchCord8(leftMixer, 0, i2sOut, 0);
AudioConnection patchCord9(rightMixer, 0, i2sOut, 1);
AudioControlSGTL5000 audioShield;

enum DeviceState {
  IDLE,
  RECORDING,
  PROCESSING,
  ERROR_STATE
};

struct Model {
  uint32_t magic;
  uint16_t version;
  uint32_t voiceCount;
  float meanLogF0;
  float voicedRatio;
  float meanRms;
  float envelope[ENV_BINS];
  float bands[BAND_COUNT];
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

struct GrainInfo {
  char filename[13];
  uint32_t startMs;
  uint32_t durationMs;
};

struct LogosStream {
  AudioPlayQueue *queue;
  FsFile file;
  GrainInfo currentGrain;
  bool playing = false;
  bool activeMode = false;
  bool repeatEligible = true;
  bool queuedFreshAudio = false;
  uint32_t nextEventMs = 0;
  uint32_t stateSwitchMs = 0;
  uint32_t samplesRemaining = 0;
  uint32_t samplesTotal = 0;
  uint32_t samplesPlayed = 0;
  uint32_t currentStartMs = 0;
  uint32_t currentDurationMs = 0;
};

DeviceState state = IDLE;
DebouncedButton recordButton{RECORD_BUTTON_PIN, "record"};
Model model;
uint16_t nextIndex = 1;
uint32_t grainCount = 0;

char currentFilename[13] = "W0000.WAV";
char tempFilename[13] = "T0000.WAV";
DMAMEM uint8_t internalRecordBuffer[INTERNAL_RECORD_BYTES];
EXTMEM uint8_t psramRecordBuffer[FIELD_RECORD_BYTES];
uint8_t *activeRecordBuffer = internalRecordBuffer;
size_t activeRecordCapacity = INTERNAL_RECORD_BYTES;
uint32_t activeRecordSeconds = INTERNAL_RECORD_SECONDS;
size_t recordBytesUsed = 0;
uint32_t recordingStartedMs = 0;
bool recordOverflow = false;
bool usingPsramBuffer = false;

LogosStream logosStreams[LOGOS_STREAM_COUNT] = {
  {&logosQueue1},
  {&logosQueue2},
  {&logosQueue3}
};
FsFile melosFile;
bool melosPlaying = false;
bool melosQueuedFreshAudio = false;
uint32_t melosSamplesTotal = 0;

bool diagnosticToneOn = false;
uint32_t lastLedToggleMs = 0;
bool ledBlinkState = false;
uint32_t rngState = 0x12345678UL;

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
  } else if (state == PROCESSING) {
    if (now - lastLedToggleMs >= 400) {
      ledBlinkState = !ledBlinkState;
      lastLedToggleMs = now;
    }
    setStatusLed(ledBlinkState);
  } else if (state == ERROR_STATE) {
    if (now - lastLedToggleMs >= 120) {
      ledBlinkState = !ledBlinkState;
      lastLedToggleMs = now;
    }
    setStatusLed(ledBlinkState);
  } else {
    setStatusLed(false);
  }
}

void enterState(DeviceState nextState) {
  state = nextState;
  lastLedToggleMs = millis();
  ledBlinkState = false;
  updateStatusLed();
}

uint32_t nextRandomU32() {
  rngState = rngState * 1664525UL + 1013904223UL;
  return rngState;
}

float nextRandomBipolar() {
  return (static_cast<float>(nextRandomU32() & 0xFFFFUL) / 32767.5f) - 1.0f;
}

float nextRandomGaussian(float sigma) {
  float sum = 0.0f;
  for (uint8_t i = 0; i < 6; ++i) {
    sum += static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
  }
  return (sum - 3.0f) * sigma;
}

uint32_t randomRangeMs(uint32_t minMs, uint32_t maxMs) {
  if (maxMs <= minMs) {
    return minMs;
  }
  return minMs + (nextRandomU32() % (maxMs - minMs + 1));
}

void scheduleLogosStream(LogosStream &stream, uint8_t index) {
  if (index == 0) {
    stream.nextEventMs = millis() + randomRangeMs(LOGOS_STREAM_1_MIN_INTERVAL_MS, LOGOS_STREAM_1_MAX_INTERVAL_MS);
  } else if (stream.activeMode) {
    stream.nextEventMs = millis() + randomRangeMs(LOGOS_ACTIVE_MIN_INTERVAL_MS, LOGOS_ACTIVE_MAX_INTERVAL_MS);
  } else {
    stream.nextEventMs = millis() + randomRangeMs(LOGOS_DORMANT_MIN_INTERVAL_MS, LOGOS_DORMANT_MAX_INTERVAL_MS);
  }
}

void scheduleLogosStateSwitch(LogosStream &stream) {
  stream.stateSwitchMs = millis() + randomRangeMs(LOGOS_STATE_MIN_MS, LOGOS_STATE_MAX_MS);
}

void initializeLogosSchedulers() {
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    logosStreams[i].playing = false;
    logosStreams[i].activeMode = i == 0;
    logosStreams[i].repeatEligible = true;
    scheduleLogosStream(logosStreams[i], i);
    if (i > 0) {
      scheduleLogosStateSwitch(logosStreams[i]);
    }
  }
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

uint16_t loadCounter() {
  const uint16_t scannedNext = scanNextIndex();
  FsFile file = SD.sdfs.open(COUNTER_FILE, O_RDONLY);
  if (!file) {
    return scannedNext;
  }

  char text[12] = {0};
  const int readBytes = file.read(text, sizeof(text) - 1);
  file.close();
  if (readBytes <= 0) {
    return scannedNext;
  }

  const uint16_t value = static_cast<uint16_t>(atoi(text));
  if (value < 1 || value > 9999) {
    return scannedNext;
  }
  return value > scannedNext ? value : scannedNext;
}

bool saveCounter(uint16_t value) {
  FsFile file = SD.sdfs.open(COUNTER_FILE, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    Serial.println("Could not open COUNTER.TXT.");
    return false;
  }
  file.print(value);
  file.println();
  const bool ok = file.sync();
  file.close();
  return ok;
}

void initDefaultModel() {
  model.magic = MODEL_MAGIC;
  model.version = MODEL_VERSION;
  model.voiceCount = 0;
  model.meanLogF0 = logf(DEFAULT_F0_HZ);
  model.voicedRatio = DEFAULT_VOICED_RATIO;
  model.meanRms = DEFAULT_RMS;
  for (uint8_t i = 0; i < ENV_BINS; ++i) {
    const float phase = static_cast<float>(i) / static_cast<float>(ENV_BINS - 1);
    model.envelope[i] = 0.35f + 0.25f * sinf(phase * PI);
  }
  for (uint8_t i = 0; i < BAND_COUNT; ++i) {
    model.bands[i] = 1.0f / BAND_COUNT;
  }
}

bool modelLooksValid(const Model &candidate) {
  if (candidate.magic != MODEL_MAGIC || candidate.version != MODEL_VERSION) {
    return false;
  }
  if (!isfinite(candidate.meanLogF0) || !isfinite(candidate.voicedRatio) || !isfinite(candidate.meanRms)) {
    return false;
  }
  if (candidate.voicedRatio < 0.0f || candidate.voicedRatio > 1.0f) {
    return false;
  }
  return true;
}

void loadModel() {
  FsFile file = SD.sdfs.open(MODEL_FILE, O_RDONLY);
  if (!file) {
    initDefaultModel();
    Serial.println("MODEL.BIN missing; using default collective model.");
    return;
  }

  Model candidate;
  const int readBytes = file.read(&candidate, sizeof(candidate));
  file.close();
  if (readBytes == static_cast<int>(sizeof(candidate)) && modelLooksValid(candidate)) {
    model = candidate;
    Serial.println("MODEL.BIN loaded.");
  } else {
    initDefaultModel();
    Serial.println("MODEL.BIN corrupt; using default collective model.");
  }
}

bool saveModel() {
  if (SD.exists(MODEL_TEMP_FILE)) {
    SD.remove(MODEL_TEMP_FILE);
  }
  FsFile file = SD.sdfs.open(MODEL_TEMP_FILE, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    Serial.println("Could not open MODEL.TMP.");
    return false;
  }
  const bool ok = writeExact(file, &model, sizeof(model)) && file.sync();
  file.close();
  if (!ok) {
    return false;
  }
  if (SD.exists(MODEL_FILE)) {
    SD.remove(MODEL_FILE);
  }
  if (!SD.rename(MODEL_TEMP_FILE, MODEL_FILE)) {
    Serial.println("Could not rename MODEL.TMP to MODEL.BIN.");
    return false;
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

void stopAllPlayback() {
  if (melosFile) {
    melosFile.close();
  }
  melosPlaying = false;
  melosQueuedFreshAudio = false;
  rightMixer.gain(0, 0.0f);

  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    if (logosStreams[i].file) {
      logosStreams[i].file.close();
    }
    logosStreams[i].playing = false;
    logosStreams[i].queuedFreshAudio = false;
  }
  leftMixer.gain(0, 0.0f);
  leftMixer.gain(1, 0.0f);
  leftMixer.gain(2, 0.0f);
  diagnosticTone.amplitude(0.0f);
  diagnosticToneOn = false;
}

void startMelosLoop() {
  if (state != IDLE || diagnosticToneOn) {
    return;
  }
  if (melosPlaying || !SD.exists(MELOS_FILE)) {
    return;
  }

  melosFile = SD.sdfs.open(MELOS_FILE, O_RDONLY);
  if (!melosFile) {
    Serial.println("Could not open MELOS.WAV.");
    return;
  }

  const uint32_t fileBytes = melosFile.size();
  if (fileBytes <= 44) {
    Serial.println("MELOS.WAV is empty or corrupt.");
    melosFile.close();
    return;
  }

  melosSamplesTotal = (fileBytes - 44) / 2UL;
  const uint32_t maxStartSample = melosSamplesTotal * 6UL / 10UL;
  const uint32_t startSample = maxStartSample > 0 ? nextRandomU32() % maxStartSample : 0;
  const uint32_t byteOffset = 44 + startSample * 2UL;
  if (!melosFile.seekSet(byteOffset)) {
    Serial.println("Could not seek MELOS.WAV.");
    melosFile.close();
    return;
  }

  melosPlaying = true;
  melosQueuedFreshAudio = false;
  rightMixer.gain(0, 0.0f);
  Serial.print("MELOS start sample: ");
  Serial.println(startSample);
}

void feedMelosQueue() {
  if (!melosPlaying) {
    return;
  }

  while (melosQueue.available()) {
    int16_t *buffer = melosQueue.getBuffer();
    if (!buffer) {
      return;
    }

    for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
      uint8_t bytes[2];
      if (melosFile.read(bytes, 2) != 2) {
        if (!melosFile.seekSet(44) || melosFile.read(bytes, 2) != 2) {
          buffer[i] = 0;
          melosPlaying = false;
          rightMixer.gain(0, 0.0f);
          continue;
        }
      }
      const int16_t sample = static_cast<int16_t>((bytes[1] << 8) | bytes[0]);
      buffer[i] = floatToInt16((sample / 32768.0f) * MELOS_GAIN);
    }
    melosQueue.playBuffer();
    if (!melosQueuedFreshAudio) {
      melosQueuedFreshAudio = true;
      rightMixer.gain(0, 1.0f);
    }
  }
}

bool saveRawRecording() {
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

  Serial.print("Saved raw archive ");
  Serial.print(currentFilename);
  Serial.print(" (");
  Serial.print(recordBytesUsed);
  Serial.println(" bytes audio).");
  return true;
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
  char copy[48];
  strncpy(copy, line, sizeof(copy));
  copy[sizeof(copy) - 1] = '\0';

  char *name = strtok(copy, ",");
  char *start = strtok(NULL, ",");
  char *duration = strtok(NULL, ",");
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

  char line[48];
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

void appendLogosGrains() {
  const int16_t *samples = reinterpret_cast<const int16_t *>(activeRecordBuffer);
  const uint32_t totalSamples = recordBytesUsed / 2;
  if (totalSamples < LOGOS_WINDOW_SAMPLES) {
    return;
  }

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
    const bool voiced = rms > LOGOS_RMS_THRESHOLD;
    const uint32_t windowEnd = start + LOGOS_WINDOW_SAMPLES;

    if (voiced) {
      if (!inRegion) {
        regionStart = start;
        inRegion = true;
      } else if (start > lastVoicedEnd + gapToleranceSamples) {
        const uint32_t durationMs = (regionEnd - regionStart) * 1000UL / SAMPLE_RATE;
        if (durationMs >= LOGOS_MIN_DURATION_MS && durationMs <= LOGOS_MAX_DURATION_MS) {
          appendGrain(currentFilename, regionStart * 1000UL / SAMPLE_RATE, durationMs);
          added++;
        }
        regionStart = start;
      }
      regionEnd = windowEnd;
      lastVoicedEnd = windowEnd;
    } else if (inRegion && start > lastVoicedEnd + gapToleranceSamples) {
      const uint32_t durationMs = (regionEnd - regionStart) * 1000UL / SAMPLE_RATE;
      if (durationMs >= LOGOS_MIN_DURATION_MS && durationMs <= LOGOS_MAX_DURATION_MS) {
        appendGrain(currentFilename, regionStart * 1000UL / SAMPLE_RATE, durationMs);
        added++;
      }
      inRegion = false;
    }
  }

  if (inRegion) {
    const uint32_t durationMs = (regionEnd - regionStart) * 1000UL / SAMPLE_RATE;
    if (durationMs >= LOGOS_MIN_DURATION_MS && durationMs <= LOGOS_MAX_DURATION_MS) {
      appendGrain(currentFilename, regionStart * 1000UL / SAMPLE_RATE, durationMs);
      added++;
    }
  }

  grainCount += added;
  Serial.print("LOGOS grains added: ");
  Serial.println(added);
}

int compareFloats(const void *a, const void *b) {
  const float fa = *static_cast<const float *>(a);
  const float fb = *static_cast<const float *>(b);
  return (fa > fb) - (fa < fb);
}

float estimateMedianF0(float &voicedRatio) {
  const int16_t *source = reinterpret_cast<const int16_t *>(activeRecordBuffer);
  const uint32_t sourceSamples = recordBytesUsed / 2;
  const uint32_t decimatedSamples = sourceSamples / 4;
  if (decimatedSamples < F0_FRAME_SAMPLES) {
    voicedRatio = 0.0f;
    return 0.0f;
  }

  float acceptedF0[MAX_F0_VALUES];
  uint16_t acceptedCount = 0;
  uint32_t totalFrames = 0;
  uint32_t voicedFrames = 0;

  for (uint32_t frameStart = 0; frameStart + F0_FRAME_SAMPLES <= decimatedSamples; frameStart += F0_HOP_SAMPLES) {
    totalFrames++;

    double energy = 0.0;
    for (uint16_t i = 0; i < F0_FRAME_SAMPLES; ++i) {
      const float s = source[(frameStart + i) * 4] / 32768.0f;
      energy += static_cast<double>(s) * s;
    }
    const float rms = sqrtf(energy / F0_FRAME_SAMPLES);
    if (rms < F0_RMS_THRESHOLD) {
      continue;
    }
    voicedFrames++;

    float bestCorr = 0.0f;
    uint16_t bestLag = F0_MIN_LAG;
    for (uint16_t lag = F0_MIN_LAG; lag <= F0_MAX_LAG; ++lag) {
      double cross = 0.0;
      double e1 = 0.0;
      double e2 = 0.0;
      const uint16_t count = F0_FRAME_SAMPLES - lag;
      for (uint16_t i = 0; i < count; ++i) {
        const float a = source[(frameStart + i) * 4] / 32768.0f;
        const float b = source[(frameStart + i + lag) * 4] / 32768.0f;
        cross += static_cast<double>(a) * b;
        e1 += static_cast<double>(a) * a;
        e2 += static_cast<double>(b) * b;
      }
      const float denom = sqrtf(e1 * e2) + 1.0e-9f;
      const float corr = cross / denom;
      if (corr > bestCorr) {
        bestCorr = corr;
        bestLag = lag;
      }
    }

    if (bestCorr > F0_CONFIDENCE_THRESHOLD && acceptedCount < MAX_F0_VALUES) {
      acceptedF0[acceptedCount++] = static_cast<float>(ANALYSIS_RATE) / bestLag;
    }
    updateStatusLed();
  }

  voicedRatio = totalFrames > 0 ? static_cast<float>(voicedFrames) / totalFrames : 0.0f;
  if (acceptedCount == 0) {
    return 0.0f;
  }

  qsort(acceptedF0, acceptedCount, sizeof(float), compareFloats);
  return acceptedF0[acceptedCount / 2];
}

void analyzeEnvelopeAndRms(float envelopeOut[ENV_BINS], float &rmsOut) {
  const int16_t *samples = reinterpret_cast<const int16_t *>(activeRecordBuffer);
  const uint32_t totalSamples = recordBytesUsed / 2;
  double totalEnergy = 0.0;
  double binEnergy[ENV_BINS] = {0.0};
  uint32_t binCounts[ENV_BINS] = {0};

  for (uint32_t i = 0; i < totalSamples; ++i) {
    const float s = samples[i] / 32768.0f;
    const double e = static_cast<double>(s) * s;
    totalEnergy += e;
    uint32_t bin = (static_cast<uint64_t>(i) * ENV_BINS) / totalSamples;
    if (bin >= ENV_BINS) {
      bin = ENV_BINS - 1;
    }
    binEnergy[bin] += e;
    binCounts[bin]++;
  }

  rmsOut = totalSamples > 0 ? sqrtf(totalEnergy / totalSamples) : 0.0f;
  float maxEnv = 0.0001f;
  for (uint8_t i = 0; i < ENV_BINS; ++i) {
    envelopeOut[i] = binCounts[i] > 0 ? sqrtf(binEnergy[i] / binCounts[i]) : 0.0f;
    if (envelopeOut[i] > maxEnv) {
      maxEnv = envelopeOut[i];
    }
  }
  for (uint8_t i = 0; i < ENV_BINS; ++i) {
    envelopeOut[i] = envelopeOut[i] / maxEnv;
  }
}

void analyzeBands(float bandsOut[BAND_COUNT]) {
  const int16_t *samples = reinterpret_cast<const int16_t *>(activeRecordBuffer);
  const uint32_t totalSamples = recordBytesUsed / 2;
  const float centers[BAND_COUNT] = {175.0f, 375.0f, 700.0f, 1200.0f, 2000.0f, 3250.0f, 5000.0f, 8500.0f};
  double energies[BAND_COUNT] = {0.0};

  uint32_t stride = totalSamples / 4096UL;
  if (stride < 1) {
    stride = 1;
  }
  const uint32_t count = totalSamples / stride;
  if (count < 32) {
    for (uint8_t i = 0; i < BAND_COUNT; ++i) {
      bandsOut[i] = 1.0f / BAND_COUNT;
    }
    return;
  }

  for (uint8_t band = 0; band < BAND_COUNT; ++band) {
    const float omega = 2.0f * PI * centers[band] * stride / SAMPLE_RATE;
    const float coeff = 2.0f * cosf(omega);
    float q0 = 0.0f;
    float q1 = 0.0f;
    float q2 = 0.0f;
    for (uint32_t i = 0; i < count; ++i) {
      const float s = samples[i * stride] / 32768.0f;
      q0 = coeff * q1 - q2 + s;
      q2 = q1;
      q1 = q0;
    }
    energies[band] = static_cast<double>(q1) * q1 + static_cast<double>(q2) * q2 - coeff * static_cast<double>(q1) * q2;
  }

  double sum = 0.0;
  for (uint8_t i = 0; i < BAND_COUNT; ++i) {
    if (energies[i] < 0.0) {
      energies[i] = 0.0;
    }
    sum += energies[i];
  }
  if (sum <= 0.0) {
    for (uint8_t i = 0; i < BAND_COUNT; ++i) {
      bandsOut[i] = 1.0f / BAND_COUNT;
    }
    return;
  }
  for (uint8_t i = 0; i < BAND_COUNT; ++i) {
    bandsOut[i] = energies[i] / sum;
  }
}

void updateCollectiveModel() {
  float incomingEnvelope[ENV_BINS];
  float incomingBands[BAND_COUNT];
  float incomingRms = 0.0f;
  float incomingVoicedRatio = 0.0f;

  analyzeEnvelopeAndRms(incomingEnvelope, incomingRms);
  analyzeBands(incomingBands);
  const float medianF0 = estimateMedianF0(incomingVoicedRatio);

  model.voiceCount++;
  model.meanRms = model.meanRms * (1.0f - MODEL_WEIGHT) + incomingRms * MODEL_WEIGHT;
  model.voicedRatio = constrain(model.voicedRatio * (1.0f - MODEL_WEIGHT) + incomingVoicedRatio * MODEL_WEIGHT, 0.0f, 1.0f);
  if (medianF0 > 0.0f && isfinite(medianF0)) {
    model.meanLogF0 = model.meanLogF0 * (1.0f - MODEL_WEIGHT) + logf(medianF0) * MODEL_WEIGHT;
  }
  for (uint8_t i = 0; i < ENV_BINS; ++i) {
    model.envelope[i] = model.envelope[i] * (1.0f - MODEL_WEIGHT) + incomingEnvelope[i] * MODEL_WEIGHT;
  }
  for (uint8_t i = 0; i < BAND_COUNT; ++i) {
    model.bands[i] = model.bands[i] * (1.0f - MODEL_WEIGHT) + incomingBands[i] * MODEL_WEIGHT;
  }

  Serial.print("Model update: voices=");
  Serial.print(model.voiceCount);
  Serial.print(", medianF0=");
  Serial.print(medianF0);
  Serial.print(", voicedRatio=");
  Serial.print(model.voicedRatio);
  Serial.print(", rms=");
  Serial.println(model.meanRms, 5);
}

float softClip(float x) {
  return tanhf(x * 1.4f) / 1.4f;
}

int16_t floatToInt16(float x) {
  x = constrain(x, -1.0f, 1.0f);
  return static_cast<int16_t>(x * 32767.0f);
}

bool renderMelosWav() {
  if (SD.exists(MELOS_TEMP_FILE)) {
    SD.remove(MELOS_TEMP_FILE);
  }

  FsFile file = SD.sdfs.open(MELOS_TEMP_FILE, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    Serial.println("Could not open MTEMP.WAV.");
    return false;
  }

  const uint32_t dataBytes = MELOS_SAMPLES * 2UL;
  if (!file.preAllocate(44 + dataBytes)) {
    Serial.println("Warning: MELOS preAllocate failed.");
  }
  if (!writeWavHeader(file, dataBytes)) {
    file.close();
    return false;
  }

  const float centers[BAND_COUNT] = {175.0f, 375.0f, 700.0f, 1200.0f, 2000.0f, 3250.0f, 5000.0f, 8500.0f};
  float smoothedEnvelope[ENV_BINS];
  for (uint8_t i = 0; i < ENV_BINS; ++i) {
    float sum = 0.0f;
    for (int8_t offset = -4; offset <= 5; ++offset) {
      int16_t index = static_cast<int16_t>(i) + offset;
      while (index < 0) {
        index += ENV_BINS;
      }
      while (index >= ENV_BINS) {
        index -= ENV_BINS;
      }
      sum += model.envelope[index];
    }
    smoothedEnvelope[i] = sum / 10.0f;
  }

  float phases[BAND_COUNT] = {0.0f};
  float smoothNoise[BAND_COUNT] = {0.0f};
  int16_t block[256];
  size_t blockUsed = 0;
  const float f0 = constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ);
  float mainPhase = 0.0f;
  float drift = 0.0f;
  const uint32_t fadeSamples = MELOS_FADE_MS * (SAMPLE_RATE / 1000UL);

  for (uint32_t i = 0; i < MELOS_SAMPLES; ++i) {
    const float normalized = static_cast<float>(i) / static_cast<float>(MELOS_SAMPLES - 1);
    uint32_t envBin = normalized * ENV_BINS;
    if (envBin >= ENV_BINS) {
      envBin = ENV_BINS - 1;
    }
    float amp = smoothedEnvelope[envBin] * 0.65f + 0.08f;
    if (i < fadeSamples) {
      amp *= static_cast<float>(i) / fadeSamples;
    } else if (MELOS_SAMPLES - i < fadeSamples) {
      amp *= static_cast<float>(MELOS_SAMPLES - i) / fadeSamples;
    }

    drift += nextRandomGaussian(0.0003f);
    drift = constrain(drift, -0.04f, 0.04f);
    mainPhase += f0 * (1.0f + drift) / SAMPLE_RATE;
    mainPhase -= floorf(mainPhase);
    const float saw = (mainPhase * 2.0f) - 1.0f;
    const float triangle = 4.0f * fabsf(mainPhase - 0.5f) - 1.0f;
    const float voiced = 0.55f * saw + 0.45f * triangle;

    float colored = 0.0f;
    for (uint8_t band = 0; band < BAND_COUNT; ++band) {
      phases[band] += 2.0f * PI * centers[band] / SAMPLE_RATE;
      if (phases[band] > 2.0f * PI) {
        phases[band] -= 2.0f * PI;
      }
      smoothNoise[band] = smoothNoise[band] * 0.992f + nextRandomBipolar() * 0.008f;
      const float harmonic = sinf(phases[band]) * voiced;
      const float breath = smoothNoise[band];
      colored += (harmonic * model.voicedRatio + breath * (1.0f - model.voicedRatio)) * sqrtf(model.bands[band] * BAND_COUNT);
    }
    colored /= BAND_COUNT;

    const float sample = softClip(colored * amp * MELOS_GAIN);
    block[blockUsed++] = floatToInt16(sample);
    if (blockUsed == 256) {
      if (!writeExact(file, block, sizeof(block))) {
        file.close();
        return false;
      }
      blockUsed = 0;
      updateStatusLed();
    }
  }

  if (blockUsed > 0) {
    if (!writeExact(file, block, blockUsed * sizeof(int16_t))) {
      file.close();
      return false;
    }
  }

  const bool ok = file.truncate(44 + dataBytes) && file.sync();
  file.close();
  if (!ok) {
    Serial.println("MELOS.WAV finalize failed.");
    return false;
  }
  if (SD.exists(MELOS_FILE)) {
    SD.remove(MELOS_FILE);
  }
  if (!SD.rename(MELOS_TEMP_FILE, MELOS_FILE)) {
    Serial.println("Could not rename MTEMP.WAV to MELOS.WAV.");
    return false;
  }
  Serial.println("Rendered MELOS.WAV.");
  return true;
}

void setLogosStreamGain(uint8_t index, float gain) {
  leftMixer.gain(index, gain);
}

bool startLogosGrain(uint8_t index, const GrainInfo &grain, uint32_t extraOffsetMs, bool repeatEligible) {
  LogosStream &stream = logosStreams[index];
  if (stream.playing || state != IDLE || diagnosticToneOn) {
    return false;
  }
  stream.file = SD.sdfs.open(grain.filename, O_RDONLY);
  if (!stream.file) {
    Serial.print("Could not open LOGOS source ");
    Serial.println(grain.filename);
    return false;
  }

  if (extraOffsetMs >= grain.durationMs) {
    stream.file.close();
    return false;
  }

  const uint32_t playbackDurationMs = grain.durationMs - extraOffsetMs;
  const uint32_t startSample = (grain.startMs + extraOffsetMs) * (SAMPLE_RATE / 1000UL);
  const uint32_t byteOffset = 44 + startSample * 2UL;
  if (!stream.file.seekSet(byteOffset)) {
    Serial.println("Could not seek LOGOS grain.");
    stream.file.close();
    return false;
  }

  stream.currentGrain = grain;
  stream.currentStartMs = grain.startMs + extraOffsetMs;
  stream.currentDurationMs = playbackDurationMs;
  stream.samplesTotal = playbackDurationMs * (SAMPLE_RATE / 1000UL);
  stream.samplesRemaining = stream.samplesTotal;
  stream.samplesPlayed = 0;
  stream.repeatEligible = repeatEligible;
  stream.queuedFreshAudio = false;
  stream.playing = true;
  setLogosStreamGain(index, 0.0f);
  Serial.print("LOGOS ");
  Serial.print(index + 1);
  Serial.print(' ');
  Serial.print(grain.filename);
  Serial.print(" @ ");
  Serial.print(stream.currentStartMs);
  Serial.print(" ms for ");
  Serial.print(stream.currentDurationMs);
  Serial.println(" ms.");
  return true;
}

void finishLogosGrain(uint8_t index) {
  LogosStream &stream = logosStreams[index];
  if (stream.file) {
    stream.file.close();
  }
  stream.playing = false;
  stream.queuedFreshAudio = false;
  setLogosStreamGain(index, 0.0f);

  if (stream.repeatEligible && (nextRandomU32() % 100UL) < LOGOS_REPEAT_CHANCE_PERCENT) {
    const uint32_t offset = randomRangeMs(LOGOS_REPEAT_MIN_OFFSET_MS, LOGOS_REPEAT_MAX_OFFSET_MS);
    if (offset < stream.currentGrain.durationMs) {
      startLogosGrain(index, stream.currentGrain, offset, false);
      return;
    }
  }

  scheduleLogosStream(stream, index);
}

void feedLogosQueue(uint8_t index) {
  LogosStream &stream = logosStreams[index];
  if (!stream.playing) {
    return;
  }

  while (stream.samplesRemaining > 0 && stream.queue->available()) {
    int16_t *buffer = stream.queue->getBuffer();
    if (!buffer) {
      return;
    }

    for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
      int16_t sample = 0;
      if (stream.samplesRemaining > 0) {
        uint8_t bytes[2];
        if (stream.file.read(bytes, 2) == 2) {
          sample = static_cast<int16_t>((bytes[1] << 8) | bytes[0]);
        } else {
          stream.samplesRemaining = 0;
        }
      }

      const uint32_t fadeSamples = LOGOS_FADE_MS * (SAMPLE_RATE / 1000UL);
      float gain = LOGOS_STREAM_GAIN;
      if (stream.samplesPlayed < fadeSamples) {
        gain *= static_cast<float>(stream.samplesPlayed) / fadeSamples;
      }
      if (stream.samplesRemaining < fadeSamples) {
        gain *= static_cast<float>(stream.samplesRemaining) / fadeSamples;
      }
      buffer[i] = floatToInt16((sample / 32768.0f) * gain);

      if (stream.samplesRemaining > 0) {
        stream.samplesRemaining--;
        stream.samplesPlayed++;
      }
    }
    stream.queue->playBuffer();
    if (!stream.queuedFreshAudio) {
      stream.queuedFreshAudio = true;
      setLogosStreamGain(index, 1.0f);
    }
  }

  if (stream.samplesRemaining == 0) {
    finishLogosGrain(index);
  }
}

void updateIdlePlayback() {
  if (state != IDLE || diagnosticToneOn) {
    return;
  }

  startMelosLoop();
  feedMelosQueue();

  const uint32_t now = millis();
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    LogosStream &stream = logosStreams[i];
    feedLogosQueue(i);

    if (i > 0 && static_cast<int32_t>(now - stream.stateSwitchMs) >= 0) {
      stream.activeMode = !stream.activeMode;
      scheduleLogosStateSwitch(stream);
      scheduleLogosStream(stream, i);
    }

    if (grainCount > 0 && !stream.playing && static_cast<int32_t>(now - stream.nextEventMs) >= 0) {
      GrainInfo grain;
      if (selectRandomGrain(grain)) {
        startLogosGrain(i, grain, 0, true);
      } else {
        scheduleLogosStream(stream, i);
      }
    }
  }
}

void printModel() {
  Serial.println("MODEL");
  Serial.print("  voiceCount: ");
  Serial.println(model.voiceCount);
  Serial.print("  f0 Hz: ");
  Serial.println(expf(model.meanLogF0), 2);
  Serial.print("  voicedRatio: ");
  Serial.println(model.voicedRatio, 4);
  Serial.print("  meanRms: ");
  Serial.println(model.meanRms, 5);
}

void printGrains() {
  grainCount = countGrains();
  Serial.print("GRAINS count: ");
  Serial.println(grainCount);
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    const LogosStream &stream = logosStreams[i];
    const int32_t nextRemaining = static_cast<int32_t>(stream.nextEventMs - millis());
    Serial.print("LOGOS ");
    Serial.print(i + 1);
    Serial.print(stream.playing ? " playing, " : " idle, ");
    if (i == 0) {
      Serial.print("always, ");
    } else {
      Serial.print(stream.activeMode ? "active, " : "dormant, ");
    }
    Serial.print("next in ms: ");
    Serial.println(nextRemaining > 0 ? nextRemaining : 0);
  }
}

void toggleDiagnosticTone() {
  if (state == RECORDING || state == PROCESSING) {
    Serial.println("Tone ignored while recording/processing.");
    return;
  }
  if (!diagnosticToneOn) {
    stopAllPlayback();
    diagnosticTone.frequency(440.0f);
    diagnosticTone.amplitude(0.12f);
    diagnosticToneOn = true;
    Serial.println("Diagnostic tone ON.");
  } else {
    diagnosticTone.amplitude(0.0f);
    diagnosticToneOn = false;
    initializeLogosSchedulers();
    startMelosLoop();
    Serial.println("Diagnostic tone OFF.");
  }
}

void startRecording() {
  if (state != IDLE) {
    return;
  }
  stopAllPlayback();
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

bool processRecording() {
  enterState(PROCESSING);

  if (recordBytesUsed == 0) {
    Serial.println("Empty recording ignored.");
    return true;
  }
  if (!saveRawRecording()) {
    return false;
  }

  appendLogosGrains();
  updateCollectiveModel();
  if (!saveModel()) {
    return false;
  }
  if (!renderMelosWav()) {
    return false;
  }

  if (nextIndex < 9999) {
    nextIndex++;
    if (!saveCounter(nextIndex)) {
      return false;
    }
  } else {
    Serial.println("Recording index exhausted at W9999.WAV.");
    return false;
  }

  Serial.print("Audio CPU max: ");
  Serial.print(AudioProcessorUsageMax());
  Serial.print("%, memory max blocks: ");
  Serial.println(AudioMemoryUsageMax());
  return true;
}

void stopRecording() {
  recordQueue.end();
  drainRecordQueue();
  Serial.print("Recording stopped: ");
  Serial.print(recordBytesUsed);
  Serial.print("/");
  Serial.print(activeRecordCapacity);
  Serial.println(recordOverflow ? " bytes, overflow clipped." : " bytes.");

  if (!processRecording()) {
    enterState(ERROR_STATE);
    return;
  }

  enterState(IDLE);
  grainCount = countGrains();
  initializeLogosSchedulers();
  startMelosLoop();
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
    } else if (command == 's' || command == 'S') {
      if (state == RECORDING) {
        stopRecording();
      } else {
        stopAllPlayback();
        if (state != ERROR_STATE) {
          enterState(IDLE);
        }
        Serial.println("Stopped playback.");
      }
    } else if (command == 't' || command == 'T') {
      toggleDiagnosticTone();
    } else if (command == 'm' || command == 'M') {
      printModel();
    } else if (command == 'g' || command == 'G') {
      printGrains();
    }
  }
}

void handleRecordButton() {
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
  Serial.println("The Ooho II Field v1 LOGOS/MELOS firmware");

  AudioMemory(AUDIO_MEMORY_BLOCKS);
  logosQueue1.setMaxBuffers(18);
  logosQueue2.setMaxBuffers(18);
  logosQueue3.setMaxBuffers(18);
  melosQueue.setMaxBuffers(24);
  audioShield.enable();
  audioShield.adcHighPassFilterDisable();
  audioShield.volume(STARTUP_VOLUME);
  leftMixer.gain(0, 0.0f);
  leftMixer.gain(1, 0.0f);
  leftMixer.gain(2, 0.0f);
  leftMixer.gain(3, 0.8f);
  rightMixer.gain(0, 0.0f);
  rightMixer.gain(1, 0.8f);
  diagnosticTone.begin(WAVEFORM_SINE);
  diagnosticTone.amplitude(0.0f);

  if (USE_MAX4466_LINEIN) {
    audioShield.inputSelect(AUDIO_INPUT_LINEIN);
    audioShield.lineInLevel(LINE_IN_LEVEL);
    Serial.println("Input: Audio Shield LINEIN_L for MAX4466 output.");
  } else {
    audioShield.inputSelect(AUDIO_INPUT_MIC);
    audioShield.micGain(20);
    Serial.println("Input: Audio Shield MIC for a raw electret capsule, not MAX4466.");
  }

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("SD init failed. Use the Teensy 4.1 built-in microSD slot.");
    enterState(ERROR_STATE);
    return;
  }

  if (external_psram_size > 0) {
    activeRecordBuffer = psramRecordBuffer;
    activeRecordCapacity = min(static_cast<size_t>(external_psram_size) * 1024UL * 1024UL, FIELD_RECORD_BYTES);
    activeRecordSeconds = activeRecordCapacity / (SAMPLE_RATE * 2UL);
    usingPsramBuffer = true;
  } else {
    Serial.println("Warning: PSRAM not detected; internal RAM fallback is for bench testing only.");
  }

  rngState ^= micros();
  rngState ^= static_cast<uint32_t>(analogRead(A0)) << 16;
  loadModel();
  nextIndex = loadCounter();
  grainCount = countGrains();

  if (!SD.exists(MELOS_FILE)) {
    Serial.println("MELOS.WAV missing; rendering neutral default.");
    if (!renderMelosWav()) {
      enterState(ERROR_STATE);
      return;
    }
  }

  Serial.print("Record buffer: ");
  Serial.print(activeRecordCapacity);
  Serial.print(" bytes, max ");
  Serial.print(activeRecordSeconds);
  Serial.println(usingPsramBuffer ? " sec in PSRAM." : " sec in internal RAM fallback.");
  Serial.print("Next recording: ");
  makeFilename(currentFilename, nextIndex);
  Serial.println(currentFilename);
  Serial.print("LOGOS grains: ");
  Serial.println(grainCount);
  Serial.println("Record button pin 32: hold to record, release to process.");
  Serial.println("Serial: r=record toggle, s=stop, t=tone, m=model, g=grains.");

  enterState(IDLE);
  initializeLogosSchedulers();
  startMelosLoop();
}

void loop() {
  handleSerial();
  updateButton(recordButton);
  handleRecordButton();

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

  updateIdlePlayback();
  updateStatusLed();
}
