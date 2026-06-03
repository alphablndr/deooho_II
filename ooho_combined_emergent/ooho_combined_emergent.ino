/*
  The Ooho II - "emergent" build (Claude Code branch)

  Forked from ooho_combined_test (Stage 8). The original ooho_combined_test is
  the stable working build and is NOT modified. Primary development of this
  project happens in Codex; this particular build was made with Claude Code.

  What is different from ooho_combined_test:

  LOGOS (left channel, real words):
  - up to 4 overlapping streams (a crowd, not one voice);
  - denser scheduling (active interval 0.5 - 6 s);
  - ~30% of grains play reversed;
  - ~20% of grains play at 0.70x (slower/lower) or 1.40x (faster/higher);
  - longer 40 ms fades so words emerge instead of clicking in;
  - grains are loaded into PSRAM and played from RAM, so playback no longer
    streams from SD (much less SD contention).

  MELOS (right channel, collective voice):
  - real-time generative synthesis instead of a pre-rendered MELOS.WAV;
  - harmonic partials of F0 + breath, colored by the model bands;
  - the synth eases its parameters toward the model over a few seconds, so the
    collective voice drifts slowly and continuously as new voices accumulate -
    a constant ambient with no loop seam, no render pause, no crunch;
  - there is no MELOS file and no render step at all.

  Hardware (unchanged):
  - Teensy 4.1 + Teensy Audio Shield Rev D
  - MAX4466 mic preamp -> Audio Shield LINEIN_L
  - LINE OUT L -> amplifier -> internal speaker       (LOGOS)
  - LINE OUT R -> amplifier -> surface exciter/cube   (MELOS)
  - Public record button between pin 32 and GND
  - Optional status LED on pin 31 through 220-1000 ohm resistor to GND

  Critical rule (unchanged):
  - Never write to SD while recording. Recording fills RAM/PSRAM only.
  - SD writes, analysis, and model update happen after release.

  Serial debug:
  - r = start/stop recording
  - s = stop recording or stop all playback
  - t = diagnostic tone on/off
  - m = print model summary
  - g = print grain count and stream timing
  - n = trigger one LOGOS grain now
  - e = (no-op) MELOS is live-synthesized; nothing to render
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
constexpr uint16_t AUDIO_MEMORY_BLOCKS = 360;
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

constexpr float MELOS_GAIN = 0.48f;
constexpr float LOGOS_STREAM_GAIN = 0.33f;
constexpr uint32_t LOGOS_FADE_MS = 40;
constexpr uint32_t AUDIO_FEED_MIN_US = 2300;

// MELOS live synthesis
constexpr float MELOS_PARAM_EASE = 1.0f / (4.0f * SAMPLE_RATE);   // ~4 s ease toward model
constexpr float MELOS_ENV_INC = 1.0f / (28.0f * SAMPLE_RATE);     // ~28 s envelope cycle
constexpr float MELOS_PARTIAL_MAX_HZ = 8000.0f;
constexpr uint16_t MELOS_MAX_PARTIALS = 24;
constexpr uint16_t MELOS_PARTIALS_PER_VOICE = 16;
constexpr uint16_t MELOS_SINE_SIZE = 2048;
constexpr uint8_t MELOS_VOICE_COUNT = 3;
constexpr uint8_t MELOS_MODE_COUNT = 5;
constexpr uint32_t MELOS_MODE_MIN_MS = 20000;
constexpr uint32_t MELOS_MODE_MAX_MS = 90000;
constexpr uint32_t MELOS_CONSTRICTION_MIN_MS = 12000;
constexpr uint32_t MELOS_CONSTRICTION_MAX_MS = 45000;
constexpr uint32_t MELOS_CONSTRICTION_EVENT_MIN_MS = 80;
constexpr uint32_t MELOS_CONSTRICTION_EVENT_MAX_MS = 320;
constexpr float MELOS_TRACT_EASE = 1.0f / (10.0f * SAMPLE_RATE);  // ~10 s vowel morph
constexpr float MELOS_VOICE_EASE = 1.0f / (8.0f * SAMPLE_RATE);   // ~8 s choir morph

constexpr uint8_t LOGOS_STREAM_COUNT = 4;
constexpr uint32_t LOGOS_STREAM_1_MIN_INTERVAL_MS = 4000;
constexpr uint32_t LOGOS_STREAM_1_MAX_INTERVAL_MS = 9000;
constexpr uint32_t LOGOS_DORMANT_MIN_INTERVAL_MS = 60000;
constexpr uint32_t LOGOS_DORMANT_MAX_INTERVAL_MS = 120000;
constexpr uint32_t LOGOS_ACTIVE_MIN_INTERVAL_MS = 500;
constexpr uint32_t LOGOS_ACTIVE_MAX_INTERVAL_MS = 6000;
constexpr uint32_t LOGOS_STATE_MIN_MS = 30000;
constexpr uint32_t LOGOS_STATE_MAX_MS = 90000;
constexpr uint8_t LOGOS_REPEAT_CHANCE_PERCENT = 25;
constexpr uint32_t LOGOS_REPEAT_MIN_OFFSET_MS = 80;
constexpr uint32_t LOGOS_REPEAT_MAX_OFFSET_MS = 200;
constexpr uint32_t LOGOS_WINDOW_SAMPLES = 661;      // about 15 ms at 44.1 kHz
constexpr uint32_t LOGOS_GAP_TOLERANCE_MS = 60;
constexpr uint32_t LOGOS_MIN_DURATION_MS = 120;
constexpr uint32_t LOGOS_MAX_DURATION_MS = 1100;
constexpr uint32_t LOGOS_SPLIT_DURATION_MS = 700;
constexpr uint32_t LOGOS_SPLIT_GAP_MS = 120;
constexpr float LOGOS_MIN_RMS_THRESHOLD = 0.008f;
constexpr float LOGOS_MAX_RMS_THRESHOLD = 0.090f;
constexpr float LOGOS_WINDOW_PEAK_FACTOR = 0.18f;

// LOGOS reverse / rate variation
constexpr uint8_t LOGOS_REVERSE_PERCENT = 30;
constexpr uint8_t LOGOS_RATE_PERCENT = 20;
constexpr float LOGOS_RATE_SLOW = 0.70f;
constexpr float LOGOS_RATE_FAST = 1.40f;
constexpr uint32_t LOGOS_MAX_GRAIN_SAMPLES = ((LOGOS_MAX_DURATION_MS + 50) * SAMPLE_RATE) / 1000UL;
constexpr uint32_t LOGOS_MICRO_MIN_MS = 60;
constexpr uint32_t LOGOS_MICRO_MAX_MS = 220;
constexpr uint32_t LOGOS_BREATH_PULSE_MS = 920;
constexpr uint32_t LOGOS_PULSE_JITTER_MS = 120;
constexpr uint8_t LOGOS_MICRO_STREAM1_PERCENT = 12;
constexpr uint8_t LOGOS_MICRO_OTHER_PERCENT = 68;
constexpr uint8_t LOGOS_MUSICAL_RATE_PERCENT = 38;

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

extern "C" uint8_t external_psram_size;

AudioInputI2S i2sIn;
AudioRecordQueue recordQueue;
AudioPlayQueue logosQueue1;
AudioPlayQueue logosQueue2;
AudioPlayQueue logosQueue3;
AudioPlayQueue logosQueue4;
AudioPlayQueue melosQueue;
AudioSynthWaveform diagnosticTone;
AudioMixer4 logosMixer;
AudioMixer4 leftMixer;
AudioMixer4 rightMixer;
AudioOutputI2S i2sOut;

AudioConnection patchCord1(i2sIn, 0, recordQueue, 0);
AudioConnection patchCord2(logosQueue1, 0, logosMixer, 0);
AudioConnection patchCord3(logosQueue2, 0, logosMixer, 1);
AudioConnection patchCord4(logosQueue3, 0, logosMixer, 2);
AudioConnection patchCord5(logosQueue4, 0, logosMixer, 3);
AudioConnection patchCord6(logosMixer, 0, leftMixer, 0);
AudioConnection patchCord7(diagnosticTone, 0, leftMixer, 3);
AudioConnection patchCord8(melosQueue, 0, rightMixer, 0);
AudioConnection patchCord9(diagnosticTone, 0, rightMixer, 1);
AudioConnection patchCord10(leftMixer, 0, i2sOut, 0);
AudioConnection patchCord11(rightMixer, 0, i2sOut, 1);
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
  int16_t *buffer = nullptr;     // PSRAM grain buffer
  uint32_t bufferSamples = 0;    // valid source samples in buffer
  bool granular = false;
  bool reversed = false;
  float rate = 1.0f;             // 1.0 normal, 0.70 slow, 1.40 fast
  float readPos = 0.0f;          // fractional read index into buffer
  GrainInfo currentGrain;
  bool playing = false;
  bool activeMode = false;
  bool repeatEligible = true;
  bool queuedFreshAudio = false;
  bool blockReady = false;       // a filled block is waiting to be queued
  uint32_t nextEventMs = 0;
  uint32_t stateSwitchMs = 0;
  uint32_t lastFeedUs = 0;
  uint32_t samplesRemaining = 0; // output samples left
  uint32_t samplesTotal = 0;     // output samples total
  uint32_t samplesPlayed = 0;    // output samples played
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
EXTMEM int16_t logosGrainBuffers[LOGOS_STREAM_COUNT][LOGOS_MAX_GRAIN_SAMPLES];
uint8_t *activeRecordBuffer = internalRecordBuffer;
size_t activeRecordCapacity = INTERNAL_RECORD_BYTES;
uint32_t activeRecordSeconds = INTERNAL_RECORD_SECONDS;
size_t recordBytesUsed = 0;
uint32_t recordingStartedMs = 0;
bool recordOverflow = false;
bool usingPsramBuffer = false;

LogosStream logosStreams[LOGOS_STREAM_COUNT] = {
  {&logosQueue1}, {&logosQueue2}, {&logosQueue3}, {&logosQueue4}
};

// MELOS live synth state
bool melosQueuedFreshAudio = false;
bool melosBlockReady = false;   // a synthesized block is waiting to be queued
uint32_t melosLastFeedUs = 0;
float melosMainPhase = 0.0f;
float melosDrift = 0.0f;
float melosDriftTarget = 0.0f;
float melosBreathFast = 0.0f;
float melosBreathSlow = 0.0f;
float melosEnvPhase = 0.0f;
float melosF0 = DEFAULT_F0_HZ;
float melosVoicedRatio = DEFAULT_VOICED_RATIO;
float melosBandWeight[BAND_COUNT];
float melosVowelWeight[BAND_COUNT];
float melosTargetVowelWeight[BAND_COUNT];
float melosSmoothedEnv[ENV_BINS];
float melosVoicePhase[MELOS_VOICE_COUNT];
float melosVoiceRatio[MELOS_VOICE_COUNT];
float melosTargetVoiceRatio[MELOS_VOICE_COUNT];
float melosVoiceLevel[MELOS_VOICE_COUNT];
float melosTargetVoiceLevel[MELOS_VOICE_COUNT];
float melosBreathMix = 0.0f;
float melosTargetBreathMix = 0.0f;
float melosConstriction = 0.0f;
float melosConstrictionTarget = 0.0f;
uint8_t melosMode = 0;
uint32_t melosNextModeSample = 0;
uint32_t melosNextConstrictionSample = 0;
uint32_t melosConstrictionReleaseSample = 0;
uint32_t melosSampleCounter = 0;
float melosSine[MELOS_SINE_SIZE];

bool diagnosticToneOn = false;
uint32_t lastLedToggleMs = 0;
bool ledBlinkState = false;
uint32_t rngState = 0x12345678UL;

void feedMelosQueue();
void feedLogosQueue(uint8_t index);
void servicePlaybackDuringWork();
void stopLogosPlayback();
void computeMelosEnvelope();

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

bool playbackServiceAllowed() {
  return (state == IDLE || state == RECORDING || state == PROCESSING) && !diagnosticToneOn;
}

void workTick() {
  updateStatusLed();
  servicePlaybackDuringWork();
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

float softClip(float x) {
  return tanhf(x * 1.4f) / 1.4f;
}

int16_t floatToInt16(float x) {
  x = constrain(x, -1.0f, 1.0f);
  return static_cast<int16_t>(x * 32767.0f);
}

void initSineTable() {
  for (uint16_t i = 0; i < MELOS_SINE_SIZE; ++i) {
    melosSine[i] = sinf(2.0f * PI * static_cast<float>(i) / MELOS_SINE_SIZE);
  }
}

inline float sineLookup(float phase) {
  phase -= floorf(phase);
  const float fidx = phase * MELOS_SINE_SIZE;
  const uint16_t i0 = static_cast<uint16_t>(fidx) & (MELOS_SINE_SIZE - 1);
  const uint16_t i1 = (i0 + 1) & (MELOS_SINE_SIZE - 1);
  const float frac = fidx - floorf(fidx);
  return melosSine[i0] * (1.0f - frac) + melosSine[i1] * frac;
}

uint8_t bandForFreq(float f) {
  static const float edges[BAND_COUNT - 1] = {275.0f, 537.0f, 950.0f, 1600.0f, 2625.0f, 4125.0f, 6750.0f};
  for (uint8_t b = 0; b < BAND_COUNT - 1; ++b) {
    if (f < edges[b]) {
      return b;
    }
  }
  return BAND_COUNT - 1;
}

uint32_t musicalLogosIntervalMs(uint8_t index) {
  static const uint8_t pulseMultipliers[] = {1, 1, 2, 3, 4, 6};
  static const uint8_t burstMultipliers[] = {1, 1, 1, 2, 2, 3};
  const bool burstRole = index == 3;
  const uint8_t *choices = burstRole ? burstMultipliers : pulseMultipliers;
  const uint8_t choice = choices[nextRandomU32() % 6];
  int32_t interval = static_cast<int32_t>(LOGOS_BREATH_PULSE_MS) * choice;
  interval += static_cast<int32_t>(randomRangeMs(0, LOGOS_PULSE_JITTER_MS * 2UL)) - static_cast<int32_t>(LOGOS_PULSE_JITTER_MS);
  if (interval < static_cast<int32_t>(LOGOS_ACTIVE_MIN_INTERVAL_MS)) {
    interval = LOGOS_ACTIVE_MIN_INTERVAL_MS;
  }
  if (interval > static_cast<int32_t>(LOGOS_ACTIVE_MAX_INTERVAL_MS)) {
    interval = LOGOS_ACTIVE_MAX_INTERVAL_MS;
  }
  return static_cast<uint32_t>(interval);
}

float musicalLogosRate(uint8_t index) {
  static const float allRates[] = {0.50f, 0.75f, 1.3333f, 1.50f};
  static const float lowRates[] = {0.50f, 0.75f, 0.75f, 1.3333f};
  static const float brightRates[] = {0.75f, 1.3333f, 1.50f, 1.50f};
  const float *choices = allRates;
  if (index == 2) {
    choices = lowRates;
  } else if (index == 3) {
    choices = brightRates;
  }
  return choices[nextRandomU32() % 4];
}

void scheduleLogosStream(LogosStream &stream, uint8_t index) {
  if (index == 0) {
    stream.nextEventMs = millis() + randomRangeMs(LOGOS_STREAM_1_MIN_INTERVAL_MS, LOGOS_STREAM_1_MAX_INTERVAL_MS);
  } else if (stream.activeMode) {
    stream.nextEventMs = millis() + musicalLogosIntervalMs(index);
  } else {
    const uint32_t density = grainCount < 240UL ? grainCount : 240UL;
    const uint32_t shrink = density * 250UL;
    const uint32_t minDormant = LOGOS_DORMANT_MIN_INTERVAL_MS > shrink ? LOGOS_DORMANT_MIN_INTERVAL_MS - shrink : 18000UL;
    const uint32_t maxDormant = LOGOS_DORMANT_MAX_INTERVAL_MS > shrink ? LOGOS_DORMANT_MAX_INTERVAL_MS - shrink : 45000UL;
    stream.nextEventMs = millis() + randomRangeMs(minDormant, maxDormant);
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
    workTick();
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

void stopLogosPlayback() {
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    logosStreams[i].playing = false;
    logosStreams[i].queuedFreshAudio = false;
    logosStreams[i].blockReady = false;
    logosMixer.gain(i, 0.0f);
  }
}

void stopAllPlayback() {
  melosQueuedFreshAudio = false;
  melosBlockReady = false;
  rightMixer.gain(0, 0.0f);
  stopLogosPlayback();
  diagnosticTone.amplitude(0.0f);
  diagnosticToneOn = false;
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
    if (chunkMs >= LOGOS_MIN_DURATION_MS && appendGrain(filename, cursorMs, chunkMs)) {
      added++;
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
    if ((i & 0x7FFUL) == 0) {
      workTick();
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
    workTick();
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
    workTick();
  }

  if (inRegion) {
    added += appendRegionGrains(currentFilename, regionStart, regionEnd);
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
    workTick();
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
    if ((i & 0x7FFUL) == 0) {
      workTick();
    }
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
      if ((i & 0x3FFUL) == 0) {
        workTick();
      }
    }
    energies[band] = static_cast<double>(q1) * q1 + static_cast<double>(q2) * q2 - coeff * static_cast<double>(q1) * q2;
    workTick();
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

// ---------------- MELOS: real-time generative synthesis ----------------

void computeMelosEnvelope() {
  float envelopeAverage = 0.0f;
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
    melosSmoothedEnv[i] = sum / 10.0f;
    envelopeAverage += melosSmoothedEnv[i];
  }
  envelopeAverage /= ENV_BINS;
  for (uint8_t i = 0; i < ENV_BINS; ++i) {
    melosSmoothedEnv[i] = envelopeAverage * 0.82f + melosSmoothedEnv[i] * 0.18f;
  }
}

uint32_t melosRandomSamples(uint32_t minMs, uint32_t maxMs) {
  return randomRangeMs(minMs, maxMs) * (SAMPLE_RATE / 1000UL);
}

float melosModeBand(uint8_t mode, uint8_t band) {
  static const float vowelShapes[MELOS_MODE_COUNT][BAND_COUNT] = {
    // body / oo: dark, rounded, almost closed.
    {1.75f, 1.55f, 1.05f, 0.72f, 0.42f, 0.25f, 0.16f, 0.10f},
    // open / aa: chest and open mouth.
    {0.92f, 1.20f, 1.55f, 1.42f, 0.88f, 0.44f, 0.22f, 0.12f},
    // bright / ee: thin tongue-forward color.
    {0.36f, 0.52f, 0.88f, 1.18f, 1.65f, 1.25f, 0.62f, 0.24f},
    // whisper / fricative air: less body, more lip/teeth air.
    {0.20f, 0.32f, 0.52f, 0.80f, 1.05f, 1.35f, 1.25f, 0.72f},
    // nasal / narrow: middle-heavy and slightly strange.
    {0.58f, 0.95f, 1.78f, 1.62f, 0.75f, 0.34f, 0.20f, 0.12f}
  };
  return vowelShapes[mode % MELOS_MODE_COUNT][band];
}

void chooseMelosMode(bool immediate) {
  melosMode = nextRandomU32() % MELOS_MODE_COUNT;

  static const float ratios[MELOS_MODE_COUNT][MELOS_VOICE_COUNT] = {
    {0.997f, 1.000f, 1.004f},   // unison body
    {0.500f, 1.000f, 1.500f},   // low fifth / octave body
    {1.000f, 1.333f, 1.500f},   // open just chord
    {1.000f, 2.000f, 3.000f},   // breath overtones
    {0.750f, 1.000f, 1.500f}    // nasal thread
  };
  static const float levels[MELOS_MODE_COUNT][MELOS_VOICE_COUNT] = {
    {0.62f, 0.48f, 0.36f},
    {0.40f, 0.62f, 0.30f},
    {0.50f, 0.34f, 0.36f},
    {0.18f, 0.10f, 0.08f},
    {0.42f, 0.50f, 0.22f}
  };
  static const float breathMixes[MELOS_MODE_COUNT] = {0.12f, 0.18f, 0.10f, 0.58f, 0.22f};

  for (uint8_t i = 0; i < MELOS_VOICE_COUNT; ++i) {
    melosTargetVoiceRatio[i] = ratios[melosMode][i];
    melosTargetVoiceLevel[i] = levels[melosMode][i];
    if (immediate) {
      melosVoiceRatio[i] = melosTargetVoiceRatio[i];
      melosVoiceLevel[i] = melosTargetVoiceLevel[i];
      melosVoicePhase[i] = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
    }
  }
  for (uint8_t b = 0; b < BAND_COUNT; ++b) {
    melosTargetVowelWeight[b] = melosModeBand(melosMode, b);
    if (immediate) {
      melosVowelWeight[b] = melosTargetVowelWeight[b];
    }
  }
  melosTargetBreathMix = breathMixes[melosMode];
  if (immediate) {
    melosBreathMix = melosTargetBreathMix;
  }
  melosNextModeSample = melosSampleCounter + melosRandomSamples(MELOS_MODE_MIN_MS, MELOS_MODE_MAX_MS);
}

void updateMelosVocalTract() {
  if (melosSampleCounter >= melosNextModeSample) {
    chooseMelosMode(false);
  }
  if (melosSampleCounter >= melosNextConstrictionSample) {
    melosConstrictionTarget = 1.0f;
    melosConstrictionReleaseSample = melosSampleCounter + melosRandomSamples(MELOS_CONSTRICTION_EVENT_MIN_MS, MELOS_CONSTRICTION_EVENT_MAX_MS);
    melosNextConstrictionSample = melosSampleCounter + melosRandomSamples(MELOS_CONSTRICTION_MIN_MS, MELOS_CONSTRICTION_MAX_MS);
  }
  if (melosConstrictionTarget > 0.0f && melosSampleCounter >= melosConstrictionReleaseSample) {
    melosConstrictionTarget = 0.0f;
  }

  for (uint8_t i = 0; i < MELOS_VOICE_COUNT; ++i) {
    melosVoiceRatio[i] += (melosTargetVoiceRatio[i] - melosVoiceRatio[i]) * MELOS_VOICE_EASE;
    melosVoiceLevel[i] += (melosTargetVoiceLevel[i] - melosVoiceLevel[i]) * MELOS_VOICE_EASE;
  }
  for (uint8_t b = 0; b < BAND_COUNT; ++b) {
    melosVowelWeight[b] += (melosTargetVowelWeight[b] - melosVowelWeight[b]) * MELOS_TRACT_EASE;
  }
  melosBreathMix += (melosTargetBreathMix - melosBreathMix) * MELOS_TRACT_EASE;
  melosConstriction += (melosConstrictionTarget - melosConstriction) * 0.0035f;
}

void initMelosSynth() {
  melosMainPhase = 0.0f;
  melosDrift = 0.0f;
  melosDriftTarget = 0.0f;
  melosBreathFast = 0.0f;
  melosBreathSlow = 0.0f;
  melosEnvPhase = 0.0f;
  melosSampleCounter = 0;
  melosNextModeSample = 0;
  melosNextConstrictionSample = melosRandomSamples(MELOS_CONSTRICTION_MIN_MS, MELOS_CONSTRICTION_MAX_MS);
  melosConstrictionReleaseSample = 0;
  melosConstriction = 0.0f;
  melosConstrictionTarget = 0.0f;
  melosF0 = constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ);
  melosVoicedRatio = model.voicedRatio;
  for (uint8_t b = 0; b < BAND_COUNT; ++b) {
    melosBandWeight[b] = sqrtf(model.bands[b] * BAND_COUNT);
  }
  chooseMelosMode(true);
  computeMelosEnvelope();
}

float melosNextSample() {
  updateMelosVocalTract();

  // Ease working parameters toward the live model so the voice drifts slowly.
  const float targetF0 = constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ);
  melosF0 += (targetF0 - melosF0) * MELOS_PARAM_EASE;
  melosVoicedRatio += (model.voicedRatio - melosVoicedRatio) * MELOS_PARAM_EASE;
  for (uint8_t b = 0; b < BAND_COUNT; ++b) {
    const float targetWeight = sqrtf(model.bands[b] * BAND_COUNT);
    melosBandWeight[b] += (targetWeight - melosBandWeight[b]) * MELOS_PARAM_EASE;
  }

  // Slow random-walk pitch drift.
  if ((melosSampleCounter & 0x3FFUL) == 0) {
    melosDriftTarget += nextRandomGaussian(0.0012f);
    melosDriftTarget = constrain(melosDriftTarget, -0.035f, 0.035f);
  }
  melosDrift += (melosDriftTarget - melosDrift) * 0.0008f;

  // Pink-Trombone-lite: a small harmonic choir through a slowly morphing
  // archive/vowel tract. It suggests articulation without producing words.
  float voiced = 0.0f;
  float voiceNorm = 0.0f;
  for (uint8_t v = 0; v < MELOS_VOICE_COUNT; ++v) {
    const float voiceF0 = melosF0 * melosVoiceRatio[v] * (1.0f + melosDrift * (1.0f + 0.12f * v));
    melosVoicePhase[v] += voiceF0 / SAMPLE_RATE;
    melosVoicePhase[v] -= floorf(melosVoicePhase[v]);

    float voice = 0.0f;
    uint16_t partials = 0;
    for (uint16_t n = 1; n <= MELOS_PARTIALS_PER_VOICE; ++n) {
      const float f = static_cast<float>(n) * voiceF0;
      if (f >= MELOS_PARTIAL_MAX_HZ) {
        break;
      }
      const uint8_t band = bandForFreq(f);
      float tractWeight = melosBandWeight[band] * (0.70f + 0.30f * melosVowelWeight[band]);
      if (melosConstriction > 0.001f) {
        if (band <= 1) {
          tractWeight *= 1.0f - 0.34f * melosConstriction;
        } else if (band >= 4) {
          tractWeight *= 1.0f + 0.85f * melosConstriction;
        }
      }
      const float rolloff = 1.0f / sqrtf(static_cast<float>(n));
      voice += sineLookup(melosVoicePhase[v] * static_cast<float>(n)) * tractWeight * rolloff;
      partials++;
    }
    if (partials > 0) {
      voice /= sqrtf(static_cast<float>(partials));
    }
    voiced += voice * melosVoiceLevel[v];
    voiceNorm += melosVoiceLevel[v];
  }
  if (voiceNorm > 0.0f) {
    voiced /= voiceNorm;
  }

  // Breath / noise component.
  const float white = nextRandomBipolar();
  melosBreathFast = melosBreathFast * 0.88f + white * 0.12f;
  melosBreathSlow = melosBreathSlow * 0.997f + white * 0.003f;
  float breath = (melosBreathFast - melosBreathSlow) * 0.42f;
  breath *= 1.0f + melosBreathMix + melosConstriction * 0.75f;

  // Slow walk through the smoothed envelope shape.
  melosEnvPhase += MELOS_ENV_INC;
  melosEnvPhase -= floorf(melosEnvPhase);
  uint8_t envBin = static_cast<uint8_t>(melosEnvPhase * ENV_BINS);
  if (envBin >= ENV_BINS) {
    envBin = ENV_BINS - 1;
  }
  const float amp = melosSmoothedEnv[envBin] * 0.34f + 0.12f;

  const float effectiveVoiced = constrain(melosVoicedRatio * (1.0f - melosBreathMix * 0.42f - melosConstriction * 0.20f), 0.04f, 0.96f);
  const float colored = voiced * effectiveVoiced + breath * (1.0f - effectiveVoiced);
  melosSampleCounter++;
  return softClip(colored * amp * MELOS_GAIN);
}

void feedMelosQueue() {
  if (!playbackServiceAllowed()) {
    return;
  }

  // Keep the queue topped up so a full ring gives a large cushion that absorbs
  // the blocking SD read done when a LOGOS grain starts (otherwise both channels
  // underrun, which sounds like modulation / bitcrush). We hold one filled block
  // and only retry playBuffer() when the ring is full, so we never re-synthesize
  // wasted blocks and the loop always terminates at the ring boundary.
  for (;;) {
    if (!melosBlockReady) {
      int16_t *buffer = melosQueue.getBuffer();  // NON_STALLING: NULL if pool empty
      if (!buffer) {
        return;
      }
      for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        buffer[i] = floatToInt16(melosNextSample());
      }
      melosBlockReady = true;
    }
    if (melosQueue.playBuffer() != 0) {  // ring full: keep the block, retry later
      return;
    }
    melosBlockReady = false;
    if (!melosQueuedFreshAudio) {
      melosQueuedFreshAudio = true;
      rightMixer.gain(0, 1.0f);
    }
  }
}

// ---------------- LOGOS: buffered grain playback ----------------

void setLogosStreamGain(uint8_t index, float gain) {
  logosMixer.gain(index, gain);
}

bool startLogosGrain(uint8_t index, const GrainInfo &grain, uint32_t extraOffsetMs, bool repeatEligible) {
  LogosStream &stream = logosStreams[index];
  if (stream.playing || !playbackServiceAllowed()) {
    return false;
  }
  if (extraOffsetMs >= grain.durationMs) {
    return false;
  }

  FsFile file = SD.sdfs.open(grain.filename, O_RDONLY);
  if (!file) {
    Serial.print("Could not open LOGOS source ");
    Serial.println(grain.filename);
    return false;
  }

  uint32_t playbackStartMs = grain.startMs + extraOffsetMs;
  uint32_t playbackDurationMs = grain.durationMs - extraOffsetMs;
  const uint8_t microChance = index == 0 ? LOGOS_MICRO_STREAM1_PERCENT : LOGOS_MICRO_OTHER_PERCENT;
  stream.granular = playbackDurationMs > LOGOS_MICRO_MIN_MS && (nextRandomU32() % 100UL) < microChance;
  if (stream.granular) {
    uint32_t maxMicroMs = LOGOS_MICRO_MAX_MS < playbackDurationMs ? LOGOS_MICRO_MAX_MS : playbackDurationMs;
    uint32_t microDurationMs = randomRangeMs(LOGOS_MICRO_MIN_MS, maxMicroMs);
    if (microDurationMs > playbackDurationMs) {
      microDurationMs = playbackDurationMs;
    }
    const uint32_t maxOffsetMs = playbackDurationMs - microDurationMs;
    playbackStartMs += randomRangeMs(0, maxOffsetMs);
    playbackDurationMs = microDurationMs;
  }

  uint32_t srcSamples = playbackDurationMs * (SAMPLE_RATE / 1000UL);
  if (srcSamples > LOGOS_MAX_GRAIN_SAMPLES) {
    srcSamples = LOGOS_MAX_GRAIN_SAMPLES;
  }
  const uint32_t startSample = playbackStartMs * (SAMPLE_RATE / 1000UL);
  const uint32_t byteOffset = 44 + startSample * 2UL;
  if (!file.seekSet(byteOffset)) {
    Serial.println("Could not seek LOGOS grain.");
    file.close();
    return false;
  }

  // Load the whole grain into PSRAM (WAV is little-endian, Teensy is too).
  const int got = file.read(reinterpret_cast<uint8_t *>(stream.buffer), srcSamples * 2UL);
  file.close();
  if (got < 2) {
    return false;
  }
  const uint32_t loaded = static_cast<uint32_t>(got) / 2UL;
  if (loaded == 0) {
    return false;
  }

  stream.reversed = (nextRandomU32() % 100UL) < LOGOS_REVERSE_PERCENT;
  if (stream.reversed) {
    for (uint32_t a = 0, b = loaded - 1; a < b; ++a, --b) {
      const int16_t tmp = stream.buffer[a];
      stream.buffer[a] = stream.buffer[b];
      stream.buffer[b] = tmp;
    }
  }

  stream.rate = 1.0f;
  if ((nextRandomU32() % 100UL) < LOGOS_MUSICAL_RATE_PERCENT) {
    stream.rate = musicalLogosRate(index);
  }

  stream.bufferSamples = loaded;
  stream.readPos = 0.0f;
  const uint32_t outputTotal = static_cast<uint32_t>(loaded / stream.rate);
  stream.samplesTotal = outputTotal;
  stream.samplesRemaining = outputTotal;
  stream.samplesPlayed = 0;
  stream.currentGrain = grain;
  stream.currentStartMs = playbackStartMs;
  stream.currentDurationMs = playbackDurationMs;
  stream.repeatEligible = repeatEligible;
  stream.queuedFreshAudio = false;
  stream.blockReady = false;
  stream.lastFeedUs = 0;
  stream.playing = true;
  setLogosStreamGain(index, 0.0f);

  Serial.print("LOGOS ");
  Serial.print(index + 1);
  Serial.print(' ');
  Serial.print(grain.filename);
  Serial.print(stream.granular ? " micro" : " word");
  Serial.print(stream.reversed ? " REV" : " fwd");
  Serial.print(" rate=");
  Serial.print(stream.rate, 2);
  Serial.print(" @ ");
  Serial.print(stream.currentStartMs);
  Serial.print(" ms for ");
  Serial.print(stream.currentDurationMs);
  Serial.println(" ms.");
  return true;
}

void finishLogosGrain(uint8_t index) {
  LogosStream &stream = logosStreams[index];
  stream.playing = false;
  stream.queuedFreshAudio = false;
  setLogosStreamGain(index, 0.0f);

  // Only re-trigger from SD while idle, to avoid SD reads during processing.
  if (state == IDLE && stream.repeatEligible && (nextRandomU32() % 100UL) < LOGOS_REPEAT_CHANCE_PERCENT) {
    const uint32_t offset = randomRangeMs(LOGOS_REPEAT_MIN_OFFSET_MS, LOGOS_REPEAT_MAX_OFFSET_MS);
    if (stream.currentGrain.durationMs > offset + LOGOS_MIN_DURATION_MS) {
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

  const uint32_t fadeSamples = LOGOS_FADE_MS * (SAMPLE_RATE / 1000UL);
  // Top the queue up, holding one filled block and only retrying playBuffer()
  // when the ring is full (NON_STALLING). This keeps a large cushion without
  // re-synthesizing wasted blocks, and always terminates at the ring boundary.
  for (;;) {
    if (!stream.blockReady) {
      if (stream.samplesRemaining == 0) {
        break;  // grain fully produced and last block already queued
      }
      int16_t *buffer = stream.queue->getBuffer();  // NON_STALLING: NULL if pool empty
      if (!buffer) {
        return;
      }
      for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
        float sample = 0.0f;
        if (stream.samplesRemaining > 0) {
          const float pos = stream.readPos;
          uint32_t idx = static_cast<uint32_t>(pos);
          if (idx >= stream.bufferSamples) {
            idx = stream.bufferSamples - 1;
          }
          const uint32_t idx2 = (idx + 1 < stream.bufferSamples) ? idx + 1 : idx;
          const float frac = pos - floorf(pos);
          sample = (static_cast<float>(stream.buffer[idx]) * (1.0f - frac) +
                    static_cast<float>(stream.buffer[idx2]) * frac) / 32768.0f;
          stream.readPos += stream.rate;
        }

        float gain = LOGOS_STREAM_GAIN;
        if (stream.samplesPlayed < fadeSamples) {
          gain *= static_cast<float>(stream.samplesPlayed) / fadeSamples;
        }
        if (stream.samplesRemaining < fadeSamples) {
          gain *= static_cast<float>(stream.samplesRemaining) / fadeSamples;
        }
        buffer[i] = floatToInt16(sample * gain);

        if (stream.samplesRemaining > 0) {
          stream.samplesRemaining--;
          stream.samplesPlayed++;
        }
      }
      stream.blockReady = true;
    }

    if (stream.queue->playBuffer() != 0) {  // ring full: keep the block, retry later
      return;
    }
    stream.blockReady = false;
    if (!stream.queuedFreshAudio) {
      stream.queuedFreshAudio = true;
      setLogosStreamGain(index, 1.0f);
    }
  }

  if (stream.samplesRemaining == 0 && !stream.blockReady) {
    finishLogosGrain(index);
  }
}

void servicePlaybackDuringWork() {
  if (!playbackServiceAllowed()) {
    return;
  }
  feedMelosQueue();
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    feedLogosQueue(i);
  }
}

void updateIdlePlayback() {
  if (!playbackServiceAllowed()) {
    return;
  }

  servicePlaybackDuringWork();

  const uint32_t now = millis();
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    LogosStream &stream = logosStreams[i];

    if (i > 0 && static_cast<int32_t>(now - stream.stateSwitchMs) >= 0) {
      stream.activeMode = !stream.activeMode;
      scheduleLogosStateSwitch(stream);
      scheduleLogosStream(stream, i);
    }

    // New grains only start while idle (a grain start does a bulk SD read).
    if (state == IDLE && grainCount > 0 && !stream.playing && static_cast<int32_t>(now - stream.nextEventMs) >= 0) {
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

void playOneLogosNow() {
  if (state != IDLE || diagnosticToneOn || grainCount == 0 || logosStreams[0].playing) {
    Serial.println("LOGOS one-shot ignored.");
    return;
  }
  GrainInfo grain;
  if (selectRandomGrain(grain)) {
    startLogosGrain(0, grain, 0, true);
  } else {
    Serial.println("No valid LOGOS grain found.");
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
    melosQueuedFreshAudio = false;  // MELOS synth re-gates on next feed
    Serial.println("Diagnostic tone OFF.");
  }
}

void startRecording() {
  if (state != IDLE) {
    return;
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
  // MELOS is live-synthesized: no render. Refresh the smoothed envelope so the
  // synth eases toward the updated collective voice.
  computeMelosEnvelope();

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
    } else if (command == 'n' || command == 'N') {
      playOneLogosNow();
    } else if (command == 'e' || command == 'E') {
      Serial.println("MELOS is live-synthesized; nothing to render.");
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
  Serial.println("The Ooho II - emergent build (Claude Code branch)");

  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    logosStreams[i].buffer = logosGrainBuffers[i];
  }
  initSineTable();

  AudioMemory(AUDIO_MEMORY_BLOCKS);
  logosQueue1.setMaxBuffers(24);
  logosQueue2.setMaxBuffers(24);
  logosQueue3.setMaxBuffers(24);
  logosQueue4.setMaxBuffers(24);
  melosQueue.setMaxBuffers(32);
  // NON_STALLING is required: with ORIGINAL behaviour, available() reports pool
  // space (not queue space) and playBuffer() busy-waits when the queue ring is
  // full, so a "fill until available()" loop never returns. NON_STALLING makes
  // playBuffer() return 1 when the ring is full, bounding the feed loop.
  logosQueue1.setBehaviour(AudioPlayQueue::NON_STALLING);
  logosQueue2.setBehaviour(AudioPlayQueue::NON_STALLING);
  logosQueue3.setBehaviour(AudioPlayQueue::NON_STALLING);
  logosQueue4.setBehaviour(AudioPlayQueue::NON_STALLING);
  melosQueue.setBehaviour(AudioPlayQueue::NON_STALLING);
  audioShield.enable();
  audioShield.adcHighPassFilterDisable();
  audioShield.volume(STARTUP_VOLUME);
  logosMixer.gain(0, 0.0f);
  logosMixer.gain(1, 0.0f);
  logosMixer.gain(2, 0.0f);
  logosMixer.gain(3, 0.0f);
  leftMixer.gain(0, 1.0f);   // LOGOS crowd sum bus
  leftMixer.gain(1, 0.0f);
  leftMixer.gain(2, 0.0f);
  leftMixer.gain(3, 0.8f);   // diagnostic tone
  rightMixer.gain(0, 0.0f);  // MELOS synth (gated up on first fresh block)
  rightMixer.gain(1, 0.8f);  // diagnostic tone
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
  initMelosSynth();
  nextIndex = loadCounter();
  grainCount = countGrains();

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
  Serial.println("Serial: r=record toggle, s=stop, t=tone, m=model, g=grains, n=LOGOS now, e=(no render).");
  Serial.println("MELOS is real-time synthesized; LOGOS is a 4-stream PSRAM crowd with reverse/rate.");

  enterState(IDLE);
  initializeLogosSchedulers();
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
