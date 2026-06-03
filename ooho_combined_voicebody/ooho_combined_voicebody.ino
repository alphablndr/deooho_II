/*
  The Ooho II - "coupled" build (Codex fork from emergent v3)

  Forked from ooho_combined_emergent v3. The saved v3 snapshot is
  ooho_combined_emergent_v3 and remains available as the uncoupled reference.

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
  - Pink-Trombone-lite vocal modes plus MELOS/LOGOS coupling: LOGOS grains
    excite the tract, briefly opening, stabilizing, or constricting MELOS;
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
constexpr uint16_t MODEL_VERSION = 2;
constexpr uint8_t ENV_BINS = 64;
constexpr uint8_t BAND_COUNT = 8;
constexpr float MODEL_WEIGHT = 0.10f;
constexpr float DEFAULT_F0_HZ = 160.0f;
constexpr float DEFAULT_VOICED_RATIO = 0.5f;
constexpr float DEFAULT_RMS = 0.08f;

constexpr float MELOS_GAIN = 0.525f;          // +25% from close-LOGOS balance
constexpr float LOGOS_STREAM_GAIN = 0.60f;    // +25%, then compressed below
constexpr float LOGOS_COMP_THRESHOLD = 0.18f;
constexpr float LOGOS_COMP_RATIO = 3.0f;
constexpr float LOGOS_COMP_MAKEUP = 1.30f;
constexpr uint32_t LOGOS_FADE_MS = 24;
constexpr uint32_t AUDIO_FEED_MIN_US = 2300;
// Ease the density-mode bus gain so sparse/present/swarm changes don't step
// the LOGOS level (that step was an audible click every 18-56 s).
constexpr float LOGOS_DENSITY_GAIN_EASE = 1.0f / (0.18f * SAMPLE_RATE);  // ~180 ms

// MELOS live synthesis
constexpr float MELOS_PARAM_EASE = 1.0f / (4.0f * SAMPLE_RATE);   // ~4 s ease toward model
constexpr float MELOS_ENV_INC = 1.0f / (28.0f * SAMPLE_RATE);     // ~28 s envelope cycle
constexpr float MELOS_PARTIAL_MAX_HZ = 8000.0f;
constexpr uint16_t MELOS_MAX_PARTIALS = 24;
constexpr uint16_t MELOS_PARTIALS_PER_VOICE = 16;
constexpr uint16_t MELOS_SINE_SIZE = 2048;
constexpr uint8_t MELOS_VOICE_COUNT = 6;
constexpr uint8_t MELOS_MODE_COUNT = 5;

// MELOS collective vocal body (variant A): source-formant-filter engine.
constexpr uint16_t MELOS_SOURCE_HARMONICS = 14;   // glottal buzz harmonics per voice
constexpr float MELOS_SPREAD_MAX_SEMITONES = 7.0f; // widest bloom at max archive spread
constexpr float MELOS_DETUNE_CENTS = 11.0f;        // per-voice humanizing detune
constexpr float MELOS_CONSONANCE_PULL = 0.20f;     // pull toward unison/octave/fifth
constexpr float MELOS_VOWEL_EASE = 1.0f / (3.0f * SAMPLE_RATE);   // ~3 s vowel/detune morph
constexpr float MELOS_F0_EASE = 1.0f / (4.0f * SAMPLE_RATE);      // ~4 s pitch center ease
constexpr uint32_t MELOS_BREATH_MIN_MS = 4000;
constexpr uint32_t MELOS_BREATH_MAX_MS = 8000;
constexpr float MELOS_BREATH_EASE = 1.0f / (3.0f * SAMPLE_RATE);  // ~3 s glide, no steps
constexpr float MELOS_MAKEUP = 2.2f;               // post-formant makeup before soft clip
constexpr uint32_t MELOS_MODE_MIN_MS = 20000;
constexpr uint32_t MELOS_MODE_MAX_MS = 90000;
constexpr uint32_t MELOS_CONSTRICTION_MIN_MS = 12000;
constexpr uint32_t MELOS_CONSTRICTION_MAX_MS = 45000;
constexpr uint32_t MELOS_CONSTRICTION_EVENT_MIN_MS = 80;
constexpr uint32_t MELOS_CONSTRICTION_EVENT_MAX_MS = 320;
constexpr float MELOS_TRACT_EASE = 1.0f / (10.0f * SAMPLE_RATE);  // ~10 s vowel morph
constexpr float MELOS_VOICE_EASE = 1.0f / (8.0f * SAMPLE_RATE);   // ~8 s choir morph
constexpr float MELOS_LOGOS_OPEN_DECAY = 1.0f / (2.4f * SAMPLE_RATE);
constexpr float MELOS_LOGOS_CONSTRICT_DECAY = 1.0f / (1.1f * SAMPLE_RATE);
constexpr float MELOS_LOGOS_STABILIZE_DECAY = 1.0f / (3.2f * SAMPLE_RATE);
constexpr float MELOS_LOGOS_DENSITY_EASE = 1.0f / (1.5f * SAMPLE_RATE);
constexpr float MELOS_LOGOS_COUPLING = 1.25f;
constexpr float MELOS_SIDECHAIN_DUCK = 0.34f;
constexpr float MELOS_SIDECHAIN_OPEN = 0.46f;
constexpr float MELOS_LEARN_DECAY = 1.0f / (18.0f * SAMPLE_RATE);

constexpr uint8_t LOGOS_STREAM_COUNT = 4;
constexpr uint32_t LOGOS_STREAM_1_MIN_INTERVAL_MS = 1800;
constexpr uint32_t LOGOS_STREAM_1_MAX_INTERVAL_MS = 5200;
constexpr uint32_t LOGOS_DORMANT_MIN_INTERVAL_MS = 18000;
constexpr uint32_t LOGOS_DORMANT_MAX_INTERVAL_MS = 52000;
constexpr uint32_t LOGOS_ACTIVE_MIN_INTERVAL_MS = 500;
constexpr uint32_t LOGOS_ACTIVE_MAX_INTERVAL_MS = 6000;
constexpr uint32_t LOGOS_STATE_MIN_MS = 18000;
constexpr uint32_t LOGOS_STATE_MAX_MS = 62000;
constexpr uint8_t LOGOS_REPEAT_CHANCE_PERCENT = 34;
constexpr uint32_t LOGOS_REPEAT_MIN_OFFSET_MS = 80;
constexpr uint32_t LOGOS_REPEAT_MAX_OFFSET_MS = 200;
constexpr uint32_t LOGOS_WINDOW_SAMPLES = 661;      // about 15 ms at 44.1 kHz
constexpr uint32_t LOGOS_GAP_TOLERANCE_MS = 60;
constexpr uint32_t LOGOS_MIN_DURATION_MS = 120;
constexpr uint32_t LOGOS_MAX_DURATION_MS = 1800;
constexpr uint32_t LOGOS_SPLIT_DURATION_MS = 1200;
constexpr uint32_t LOGOS_SPLIT_GAP_MS = 160;
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
constexpr uint8_t LOGOS_MICRO_STREAM1_PERCENT = 5;
constexpr uint8_t LOGOS_MICRO_OTHER_PERCENT = 68;
constexpr uint8_t LOGOS_MUSICAL_RATE_PERCENT = 38;
// Chance any grain (any stream) plays as a whole forward intelligible word;
// every other grain is deconstructed (cloud/micro). Raise for more words.
constexpr uint8_t LOGOS_WORD_PERCENT = 25;
constexpr uint8_t LOGOS_MAX_CLOUD_VOICES = 8;
constexpr uint8_t LOGOS_RECENT_HISTORY = 12;
constexpr uint8_t LOGOS_CLOUD_STREAM1_PERCENT = 12;
constexpr uint8_t LOGOS_CLOUD_BODY_PERCENT = 48;
constexpr uint8_t LOGOS_CLOUD_SYLLABLE_PERCENT = 68;
constexpr uint8_t LOGOS_CLOUD_SPARK_PERCENT = 78;
constexpr uint32_t LOGOS_CLOUD_MIN_DELAY_MS = 0;
constexpr uint32_t LOGOS_CLOUD_MAX_DELAY_MS = 520;
constexpr uint32_t LOGOS_SPARK_MIN_MS = 40;
constexpr uint32_t LOGOS_SPARK_MAX_MS = 130;
constexpr uint32_t LOGOS_DUST_MIN_MS = 35;
constexpr uint32_t LOGOS_DUST_MAX_MS = 170;
constexpr uint8_t LOGOS_MODE_SPARSE = 0;
constexpr uint8_t LOGOS_MODE_PRESENT = 1;
constexpr uint8_t LOGOS_MODE_SWARM = 2;
constexpr uint32_t LOGOS_DENSITY_MIN_MS = 18000;
constexpr uint32_t LOGOS_DENSITY_MAX_MS = 56000;
constexpr uint8_t LOGOS_RECENT_BIAS_PERCENT = 42;
constexpr uint8_t LOGOS_WAVESET_BOUNDARIES = 96;
constexpr uint8_t LOGOS_WAVESET_MIN_GROUPS = 2;
constexpr uint8_t LOGOS_WAVESET_MAX_GROUPS = 9;

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
  float meanLogF0Sq;   // running mean of logF0^2 -> archive pitch spread (variance)
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

struct LogosVoice {
  bool active = false;
  bool reversed = false;
  uint32_t delaySamples = 0;
  uint32_t sourceStart = 0;
  uint32_t sourceEnd = 0;
  uint32_t samplesRemaining = 0;
  uint32_t samplesTotal = 0;
  uint32_t samplesPlayed = 0;
  float readPos = 0.0f;
  float rate = 1.0f;
};

struct LogosStream {
  AudioPlayQueue *queue;
  int16_t *buffer = nullptr;     // PSRAM grain buffer
  uint32_t bufferSamples = 0;    // valid source samples in buffer
  bool granular = false;
  bool cloud = false;
  uint8_t cloudPattern = 0;
  bool reversed = false;
  float rate = 1.0f;             // 1.0 normal, 0.70 slow, 1.40 fast
  float readPos = 0.0f;          // fractional read index into buffer
  LogosVoice voices[LOGOS_MAX_CLOUD_VOICES];
  uint8_t voiceCount = 0;
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

struct WavesetIndex {
  uint32_t boundary[LOGOS_WAVESET_BOUNDARIES];
  uint8_t count = 0;
};

DeviceState state = IDLE;
DebouncedButton recordButton{RECORD_BUTTON_PIN, "record"};
Model model;
uint16_t nextIndex = 1;
uint32_t grainCount = 0;
uint32_t recentGrainIndexes[LOGOS_RECENT_HISTORY];
uint8_t recentGrainWrite = 0;
uint8_t recentGrainUsed = 0;
uint8_t logosDensityMode = LOGOS_MODE_PRESENT;
uint32_t logosDensitySwitchMs = 0;
float logosDensityGainCur = 1.0f;  // smoothed bus gain, eased toward the mode target

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
float melosLogosOpen = 0.0f;
float melosLogosConstrict = 0.0f;
float melosLogosStabilize = 0.0f;
float melosLogosDensity = 0.0f;
float melosLogosEnvelope = 0.0f;
float melosLearnAmount = 0.0f;
float melosLearnF0 = DEFAULT_F0_HZ;
float melosLearnBreath = 0.0f;
uint8_t melosLearnBand = 2;
uint8_t melosMode = 0;
uint32_t melosNextModeSample = 0;
uint32_t melosNextConstrictionSample = 0;
uint32_t melosConstrictionReleaseSample = 0;
uint32_t melosSampleCounter = 0;
float melosSine[MELOS_SINE_SIZE];

// MELOS collective vocal body engine (source-formant-filter)
float melosVoiceF0[MELOS_VOICE_COUNT];
float melosVoiceOffset[MELOS_VOICE_COUNT];        // current detune, semitones
float melosVoiceOffsetTarget[MELOS_VOICE_COUNT];
float melosVoiceJitter[MELOS_VOICE_COUNT];        // constant humanizing detune, cents
float melosVoiceDrift[MELOS_VOICE_COUNT];         // slow per-voice random walk, semitones
float melosVowelPos = 0.4f;                       // 0=oo, 0.5=aa, 1=ee
float melosFormantFreq[3];                        // eased F1/F2/F3
float melosFormantA1[3];                          // TPT SVF coefficients
float melosFormantA2[3];
float melosFormantA3[3];
float melosFormantS1[3];                          // SVF integrator states
float melosFormantS2[3];
float melosTenseness = 0.6f;                      // glottal tone vs breath
float melosAspiration = 0.2f;                     // noise amount
float melosBreathArc = 0.4f;                      // breathing amplitude
float melosBreathArcTarget = 0.6f;
uint32_t melosBreathNextSample = 0;

bool diagnosticToneOn = false;
uint32_t lastLedToggleMs = 0;
bool ledBlinkState = false;
uint32_t rngState = 0x12345678UL;

void feedMelosQueue();
void feedLogosQueue(uint8_t index);
void servicePlaybackDuringWork();
void stopLogosPlayback();
void computeMelosEnvelope();
uint8_t activeLogosStreamCount();
void triggerMelosLearnedGesture(float incomingF0, float incomingVoicedRatio, const float incomingBands[BAND_COUNT]);
uint8_t dominantBandIndex(const float bands[BAND_COUNT]);

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

float compressLogos(float x) {
  const float sign = x < 0.0f ? -1.0f : 1.0f;
  float a = fabsf(x);
  if (a > LOGOS_COMP_THRESHOLD) {
    a = LOGOS_COMP_THRESHOLD + (a - LOGOS_COMP_THRESHOLD) / LOGOS_COMP_RATIO;
  }
  return softClip(sign * a * LOGOS_COMP_MAKEUP);
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
  static const uint8_t pulseMultipliers[] = {1, 1, 1, 2, 2, 3};
  static const uint8_t burstMultipliers[] = {1, 1, 1, 1, 2, 2};
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

const char *logosDensityModeName() {
  if (logosDensityMode == LOGOS_MODE_SPARSE) {
    return "sparse";
  }
  if (logosDensityMode == LOGOS_MODE_SWARM) {
    return "swarm";
  }
  return "present";
}

void chooseLogosDensityMode() {
  const uint8_t r = nextRandomU32() % 100UL;
  if (r < 24) {
    logosDensityMode = LOGOS_MODE_SPARSE;
  } else if (r < 68) {
    logosDensityMode = LOGOS_MODE_PRESENT;
  } else {
    logosDensityMode = LOGOS_MODE_SWARM;
  }
  logosDensitySwitchMs = millis() + randomRangeMs(LOGOS_DENSITY_MIN_MS, LOGOS_DENSITY_MAX_MS);
  Serial.print("LOGOS density mode: ");
  Serial.println(logosDensityModeName());
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

uint8_t logosMicroChance(uint8_t index) {
  static const uint8_t chances[LOGOS_STREAM_COUNT] = {4, 34, 64, 78};
  uint8_t chance = chances[index < LOGOS_STREAM_COUNT ? index : 0];
  if (logosDensityMode == LOGOS_MODE_SWARM && index > 0) {
    chance = chance + 10 > 95 ? 95 : chance + 10;
  } else if (logosDensityMode == LOGOS_MODE_SPARSE) {
    chance /= 2;
  }
  return chance;
}

uint8_t logosCloudChance(uint8_t index) {
  static const uint8_t chances[LOGOS_STREAM_COUNT] = {
    LOGOS_CLOUD_STREAM1_PERCENT,
    LOGOS_CLOUD_BODY_PERCENT,
    LOGOS_CLOUD_SYLLABLE_PERCENT,
    LOGOS_CLOUD_SPARK_PERCENT
  };
  uint8_t chance = chances[index < LOGOS_STREAM_COUNT ? index : 0];
  if (logosDensityMode == LOGOS_MODE_SWARM) {
    chance = chance + 24 > 96 ? 96 : chance + 24;
  } else if (logosDensityMode == LOGOS_MODE_SPARSE) {
    chance = chance > 18 ? chance - 18 : 0;
  }
  return chance;
}

float logosRoleRate(uint8_t index) {
  static const float wordRates[] = {1.0f, 1.0f, 1.0f, 0.75f, 1.3333f};
  static const float bodyRates[] = {0.50f, 0.6667f, 0.75f, 0.75f, 1.0f};
  static const float syllableRates[] = {0.75f, 1.0f, 1.0f, 1.3333f, 1.50f};
  static const float sparkRates[] = {1.0f, 1.3333f, 1.50f, 1.50f, 2.0f};
  const float *choices = wordRates;
  if (index == 1) {
    choices = bodyRates;
  } else if (index == 2) {
    choices = syllableRates;
  } else if (index == 3) {
    choices = sparkRates;
  }
  return choices[nextRandomU32() % 5];
}

float logosArpRate(uint8_t step) {
  static const float arpRates[] = {0.50f, 0.75f, 1.0f, 1.3333f, 1.50f, 2.0f};
  return arpRates[step % 6];
}

uint8_t chooseLogosCloudPattern(uint8_t index) {
  const uint8_t r = nextRandomU32() % 100UL;
  const int8_t wavesetBoost = logosDensityMode == LOGOS_MODE_SWARM ? 10 : 0;
  if (index == 1) {
    return r < 26 + wavesetBoost ? 4 : (r < 48 ? 0 : (r < 70 ? 2 : (r < 88 ? 3 : 1)));
  }
  if (index == 2) {
    return r < 22 + wavesetBoost ? 4 : (r < 46 ? 1 : (r < 66 ? 0 : (r < 86 ? 3 : 2)));
  }
  if (index == 3) {
    return r < 16 + wavesetBoost ? 4 : (r < 46 ? 3 : (r < 68 ? 1 : (r < 88 ? 0 : 2)));
  }
  return r < 16 ? 4 : (r < 44 ? 0 : (r < 68 ? 2 : (r < 86 ? 3 : 1)));
}

const char *logosCloudPatternName(uint8_t pattern) {
  if (pattern == 0) {
    return "stutter";
  }
  if (pattern == 1) {
    return "arp";
  }
  if (pattern == 2) {
    return "scrub";
  }
  if (pattern == 4) {
    return "waveset";
  }
  return "scatter";
}

uint32_t logosRoleDurationMs(uint8_t index, uint32_t availableMs) {
  uint32_t minMs = LOGOS_MICRO_MIN_MS;
  uint32_t maxMs = LOGOS_MICRO_MAX_MS;
  if (index == 0) {
    minMs = availableMs < 180UL ? availableMs : 180UL;
    maxMs = availableMs;
  } else if (index == 1) {
    minMs = 120;
    maxMs = availableMs < 420UL ? availableMs : 420UL;
  } else if (index == 2) {
    minMs = LOGOS_DUST_MIN_MS;
    maxMs = availableMs < LOGOS_DUST_MAX_MS ? availableMs : LOGOS_DUST_MAX_MS;
  } else {
    minMs = LOGOS_SPARK_MIN_MS;
    maxMs = availableMs < LOGOS_SPARK_MAX_MS ? availableMs : LOGOS_SPARK_MAX_MS;
  }
  if (maxMs < minMs) {
    minMs = maxMs;
  }
  return randomRangeMs(minMs, maxMs);
}

void scheduleLogosStream(LogosStream &stream, uint8_t index) {
  if (index == 0) {
    if (logosDensityMode == LOGOS_MODE_SWARM) {
      stream.nextEventMs = millis() + randomRangeMs(520, 1900);
    } else if (logosDensityMode == LOGOS_MODE_SPARSE) {
      stream.nextEventMs = millis() + randomRangeMs(4200, 11500);
    } else {
      stream.nextEventMs = millis() + randomRangeMs(LOGOS_STREAM_1_MIN_INTERVAL_MS, LOGOS_STREAM_1_MAX_INTERVAL_MS);
    }
  } else if (stream.activeMode || logosDensityMode == LOGOS_MODE_SWARM) {
    if (logosDensityMode == LOGOS_MODE_SWARM) {
      stream.nextEventMs = millis() + randomRangeMs(280, 1700);
    } else if (logosDensityMode == LOGOS_MODE_SPARSE) {
      stream.nextEventMs = millis() + randomRangeMs(2600, 8500);
    } else {
      stream.nextEventMs = millis() + musicalLogosIntervalMs(index);
    }
  } else {
    const uint32_t density = grainCount < 240UL ? grainCount : 240UL;
    const uint32_t shrink = density * 120UL;
    uint32_t minDormant = LOGOS_DORMANT_MIN_INTERVAL_MS > shrink ? LOGOS_DORMANT_MIN_INTERVAL_MS - shrink : 7000UL;
    uint32_t maxDormant = LOGOS_DORMANT_MAX_INTERVAL_MS > shrink ? LOGOS_DORMANT_MAX_INTERVAL_MS - shrink : 18000UL;
    if (logosDensityMode == LOGOS_MODE_SPARSE) {
      minDormant = 30000;
      maxDormant = 90000;
    }
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
  model.meanLogF0Sq = model.meanLogF0 * model.meanLogF0;  // zero spread at start
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

bool isRecentGrainIndex(uint32_t index) {
  for (uint8_t i = 0; i < recentGrainUsed; ++i) {
    if (recentGrainIndexes[i] == index) {
      return true;
    }
  }
  return false;
}

void rememberGrainIndex(uint32_t index) {
  recentGrainIndexes[recentGrainWrite] = index;
  recentGrainWrite = (recentGrainWrite + 1) % LOGOS_RECENT_HISTORY;
  if (recentGrainUsed < LOGOS_RECENT_HISTORY) {
    recentGrainUsed++;
  }
}

bool isRecentRecordingFilename(const char *filename) {
  if (nextIndex <= 1 || filename[0] != 'W') {
    return false;
  }
  const uint16_t fileIndex = static_cast<uint16_t>(atoi(filename + 1));
  const uint16_t latest = nextIndex - 1;
  const uint16_t window = logosDensityMode == LOGOS_MODE_SWARM ? 4 : 2;
  return fileIndex <= latest && fileIndex + window > latest;
}

bool selectRecentRecordingGrain(GrainInfo &grain) {
  FsFile file = SD.sdfs.open(GRAINS_FILE, O_RDONLY);
  if (!file) {
    return false;
  }

  char line[48];
  GrainInfo candidate;
  uint32_t index = 0;
  uint32_t matches = 0;
  uint32_t chosenIndex = 0;
  bool found = false;
  while (readCsvLine(file, line, sizeof(line))) {
    if (parseGrainLine(line, candidate) && isRecentRecordingFilename(candidate.filename) && !isRecentGrainIndex(index)) {
      matches++;
      if ((nextRandomU32() % matches) == 0) {
        grain = candidate;
        chosenIndex = index;
        found = true;
      }
    }
    index++;
  }
  file.close();
  if (found) {
    rememberGrainIndex(chosenIndex);
  }
  return found;
}

bool selectRandomGrain(GrainInfo &grain) {
  grainCount = countGrains();
  if (grainCount == 0) {
    return false;
  }

  uint8_t recentBias = LOGOS_RECENT_BIAS_PERCENT;
  if (logosDensityMode == LOGOS_MODE_SWARM) {
    recentBias = 62;
  } else if (logosDensityMode == LOGOS_MODE_SPARSE) {
    recentBias = 24;
  }
  if ((nextRandomU32() % 100UL) < recentBias && selectRecentRecordingGrain(grain)) {
    return true;
  }

  uint32_t target = nextRandomU32() % grainCount;
  if (grainCount > LOGOS_RECENT_HISTORY) {
    for (uint8_t attempt = 0; attempt < 10 && isRecentGrainIndex(target); ++attempt) {
      target = nextRandomU32() % grainCount;
    }
  }
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
  if (found) {
    rememberGrainIndex(target);
  }
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
    const float lf = logf(medianF0);
    model.meanLogF0 = model.meanLogF0 * (1.0f - MODEL_WEIGHT) + lf * MODEL_WEIGHT;
    model.meanLogF0Sq = model.meanLogF0Sq * (1.0f - MODEL_WEIGHT) + lf * lf * MODEL_WEIGHT;
  }
  for (uint8_t i = 0; i < ENV_BINS; ++i) {
    model.envelope[i] = model.envelope[i] * (1.0f - MODEL_WEIGHT) + incomingEnvelope[i] * MODEL_WEIGHT;
  }
  for (uint8_t i = 0; i < BAND_COUNT; ++i) {
    model.bands[i] = model.bands[i] * (1.0f - MODEL_WEIGHT) + incomingBands[i] * MODEL_WEIGHT;
  }
  triggerMelosLearnedGesture(medianF0, incomingVoicedRatio, incomingBands);

  Serial.print("Model update: voices=");
  Serial.print(model.voiceCount);
  Serial.print(", medianF0=");
  Serial.print(medianF0);
  Serial.print(", voicedRatio=");
  Serial.print(model.voicedRatio);
  Serial.print(", rms=");
  Serial.println(model.meanRms, 5);
}

// ---------------- MELOS: collective vocal body (source-formant-filter) ----------------

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

uint8_t dominantBandIndex(const float bands[BAND_COUNT]) {
  uint8_t best = 0;
  float bestValue = bands[0];
  for (uint8_t b = 1; b < BAND_COUNT; ++b) {
    if (bands[b] > bestValue) {
      bestValue = bands[b];
      best = b;
    }
  }
  return best;
}

// Post-recording "chew": briefly foreground the new voice, then dissolve.
void triggerMelosLearnedGesture(float incomingF0, float incomingVoicedRatio, const float incomingBands[BAND_COUNT]) {
  melosLearnAmount = 1.0f;
  melosLearnF0 = incomingF0 > 0.0f && isfinite(incomingF0) ? constrain(incomingF0, F0_MIN_HZ, F0_MAX_HZ) : constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ);
  melosLearnBreath = constrain(1.0f - incomingVoicedRatio, 0.0f, 1.0f);
  melosLearnBand = dominantBandIndex(incomingBands);
  melosLogosOpen = constrain(melosLogosOpen + 0.45f, 0.0f, 1.0f);
  melosLogosStabilize = constrain(melosLogosStabilize + 0.35f, 0.0f, 1.0f);
  Serial.print("MELOS learned gesture: f0=");
  Serial.print(melosLearnF0, 2);
  Serial.print(" Hz, breath=");
  Serial.print(melosLearnBreath, 3);
  Serial.print(", dominantBand=");
  Serial.println(melosLearnBand);
}

// Vowel position 0=oo(dark) .. 0.5=aa(open) .. 1=ee(bright), from the band centroid.
float melosVowelPosition() {
  float num = 0.0f;
  float den = 0.0f;
  for (uint8_t b = 0; b < BAND_COUNT; ++b) {
    const float w = model.bands[b] > 0.0f ? model.bands[b] : 0.0f;
    num += static_cast<float>(b) * w;
    den += w;
  }
  const float centroid = den > 0.0f ? num / den : 3.5f;  // 0..7
  return constrain(centroid / static_cast<float>(BAND_COUNT - 1), 0.0f, 1.0f);
}

void melosFormantTargets(float pos, float &f1, float &f2, float &f3) {
  // oo (0) -> aa (0.5) -> ee (1)
  if (pos < 0.5f) {
    const float t = pos / 0.5f;
    f1 = 300.0f + (700.0f - 300.0f) * t;
    f2 = 870.0f + (1220.0f - 870.0f) * t;
  } else {
    const float t = (pos - 0.5f) / 0.5f;
    f1 = 700.0f + (270.0f - 700.0f) * t;
    f2 = 1220.0f + (2300.0f - 1220.0f) * t;
  }
  f3 = 2700.0f;
}

float melosNearestConsonant(float semis) {
  static const float consonant[5] = {-12.0f, -7.0f, 0.0f, 7.0f, 12.0f};
  float best = consonant[0];
  float bestDist = fabsf(semis - consonant[0]);
  for (uint8_t i = 1; i < 5; ++i) {
    const float d = fabsf(semis - consonant[i]);
    if (d < bestDist) {
      bestDist = d;
      best = consonant[i];
    }
  }
  return best;
}

void melosComputeFormantCoeffs() {
  static const float formantQ[3] = {6.0f, 8.0f, 9.0f};
  for (uint8_t k = 0; k < 3; ++k) {
    const float g = tanf(PI * melosFormantFreq[k] / SAMPLE_RATE);
    const float kq = 1.0f / formantQ[k];
    const float a1 = 1.0f / (1.0f + g * (g + kq));
    melosFormantA1[k] = a1;
    melosFormantA2[k] = g * a1;
    melosFormantA3[k] = g * melosFormantA2[k];
  }
}

// Per-block update: slow easing of pitch swarm, vowel/formants, breath, coupling.
void melosBlockUpdate() {
  const float blk = static_cast<float>(AUDIO_BLOCK_SAMPLES);

  // Pitch center, biased toward the just-recorded voice during the chew gesture.
  const float collectiveF0 = constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ);
  const float targetF0 = collectiveF0 * (1.0f - melosLearnAmount * 0.42f) + melosLearnF0 * (melosLearnAmount * 0.42f);
  melosF0 += (targetF0 - melosF0) * (MELOS_F0_EASE * blk);
  melosVoicedRatio += (model.voicedRatio - melosVoicedRatio) * (MELOS_PARAM_EASE * blk);
  melosLearnAmount += (0.0f - melosLearnAmount) * (MELOS_LEARN_DECAY * blk);

  // Archive pitch spread -> swarm width (semitones).
  float variance = model.meanLogF0Sq - model.meanLogF0 * model.meanLogF0;
  if (variance < 0.0f) {
    variance = 0.0f;
  }
  float spreadSemis = sqrtf(variance) * (12.0f / 0.6931472f);  // log-units -> semitones
  spreadSemis += melosLogosDensity * 2.0f;                     // LOGOS swarm blurs pitch
  spreadSemis *= (1.0f - melosLogosStabilize * 0.6f);          // clear words tighten unison
  spreadSemis = constrain(spreadSemis, 0.0f, MELOS_SPREAD_MAX_SEMITONES);

  static const float pattern[MELOS_VOICE_COUNT] = {-1.0f, -0.55f, -0.2f, 0.2f, 0.55f, 1.0f};
  for (uint8_t v = 0; v < MELOS_VOICE_COUNT; ++v) {
    float target = spreadSemis * pattern[v];
    const float c = melosNearestConsonant(target);
    target = target * (1.0f - MELOS_CONSONANCE_PULL) + c * MELOS_CONSONANCE_PULL;
    melosVoiceOffsetTarget[v] = target;
    melosVoiceOffset[v] += (target - melosVoiceOffset[v]) * (MELOS_VOWEL_EASE * blk);
    melosVoiceDrift[v] += nextRandomGaussian(0.0006f);
    melosVoiceDrift[v] = constrain(melosVoiceDrift[v], -0.08f, 0.08f);
    const float semis = melosVoiceOffset[v] + melosVoiceJitter[v] * 0.01f + melosVoiceDrift[v];
    melosVoiceF0[v] = melosF0 * expf(semis * (0.6931472f / 12.0f));
  }

  // Vowel from the archive; LOGOS events open it toward aa.
  float vowelTarget = melosVowelPosition();
  const float openAmt = constrain(melosLogosOpen + melosLogosStabilize * 0.5f + melosLogosEnvelope * 0.4f, 0.0f, 1.0f);
  vowelTarget = vowelTarget * (1.0f - openAmt * 0.5f) + 0.5f * (openAmt * 0.5f);
  melosVowelPos += (vowelTarget - melosVowelPos) * (MELOS_VOWEL_EASE * blk);

  float f1, f2, f3;
  melosFormantTargets(melosVowelPos, f1, f2, f3);
  const float constr = constrain(melosConstriction + melosLogosConstrict, 0.0f, 1.0f);
  f3 += constr * 400.0f;
  melosFormantFreq[0] += (f1 - melosFormantFreq[0]) * (MELOS_VOWEL_EASE * blk);
  melosFormantFreq[1] += (f2 - melosFormantFreq[1]) * (MELOS_VOWEL_EASE * blk);
  melosFormantFreq[2] += (f3 - melosFormantFreq[2]) * (MELOS_VOWEL_EASE * blk);
  melosComputeFormantCoeffs();

  // Tone vs breath.
  const float tenseTarget = constrain(melosVoicedRatio * (1.0f - melosLogosDensity * 0.3f) + melosLogosStabilize * 0.2f, 0.05f, 0.98f);
  melosTenseness += (tenseTarget - melosTenseness) * (MELOS_PARAM_EASE * blk);
  const float aspTarget = constrain((1.0f - melosTenseness) * 0.7f + melosLogosDensity * 0.5f + constr * 0.5f + melosLearnBreath * melosLearnAmount * 0.6f, 0.0f, 1.5f);
  melosAspiration += (aspTarget - melosAspiration) * (MELOS_PARAM_EASE * blk);

  // Breathing arc (occasional deep inhale near silence).
  if (melosSampleCounter >= melosBreathNextSample) {
    const float r = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
    const float r2 = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
    melosBreathArcTarget = r < 0.18f ? 0.06f : (0.45f + 0.55f * r2);
    melosBreathNextSample = melosSampleCounter + melosRandomSamples(MELOS_BREATH_MIN_MS, MELOS_BREATH_MAX_MS);
  }
  melosBreathArc += (melosBreathArcTarget - melosBreathArc) * (MELOS_BREATH_EASE * blk);

  // LOGOS coupling decay.
  const float activeDensity = constrain(static_cast<float>(activeLogosStreamCount()) / LOGOS_STREAM_COUNT * MELOS_LOGOS_COUPLING, 0.0f, 1.0f);
  melosLogosDensity += (activeDensity - melosLogosDensity) * (MELOS_LOGOS_DENSITY_EASE * blk);
  melosLogosOpen += (0.0f - melosLogosOpen) * (MELOS_LOGOS_OPEN_DECAY * blk);
  melosLogosConstrict += (0.0f - melosLogosConstrict) * (MELOS_LOGOS_CONSTRICT_DECAY * blk);
  melosLogosStabilize += (0.0f - melosLogosStabilize) * (MELOS_LOGOS_STABILIZE_DECAY * blk);
}

void initMelosSynth() {
  melosEnvPhase = 0.0f;
  melosSampleCounter = 0;
  melosF0 = constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ);
  melosVoicedRatio = model.voicedRatio;
  melosTenseness = constrain(model.voicedRatio, 0.1f, 0.95f);
  melosAspiration = 0.2f;
  melosVowelPos = melosVowelPosition();

  float f1, f2, f3;
  melosFormantTargets(melosVowelPos, f1, f2, f3);
  melosFormantFreq[0] = f1;
  melosFormantFreq[1] = f2;
  melosFormantFreq[2] = f3;
  melosComputeFormantCoeffs();
  for (uint8_t k = 0; k < 3; ++k) {
    melosFormantS1[k] = 0.0f;
    melosFormantS2[k] = 0.0f;
  }

  for (uint8_t v = 0; v < MELOS_VOICE_COUNT; ++v) {
    melosVoicePhase[v] = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
    melosVoiceJitter[v] = (static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f * 2.0f - 1.0f) * MELOS_DETUNE_CENTS;
    melosVoiceOffset[v] = 0.0f;
    melosVoiceOffsetTarget[v] = 0.0f;
    melosVoiceDrift[v] = 0.0f;
    melosVoiceF0[v] = melosF0;
  }

  melosBreathArc = 0.4f;
  melosBreathArcTarget = 0.6f;
  melosBreathNextSample = melosRandomSamples(MELOS_BREATH_MIN_MS, MELOS_BREATH_MAX_MS);
  computeMelosEnvelope();
}

float melosNextSample() {
  // Glottal sources: a crowd of voices near one collective pitch.
  float source = 0.0f;
  for (uint8_t v = 0; v < MELOS_VOICE_COUNT; ++v) {
    const float vf0 = melosVoiceF0[v];
    melosVoicePhase[v] += vf0 / SAMPLE_RATE;
    melosVoicePhase[v] -= floorf(melosVoicePhase[v]);

    float buzz = 0.0f;
    for (uint16_t n = 1; n <= MELOS_SOURCE_HARMONICS; ++n) {
      const float f = static_cast<float>(n) * vf0;
      if (f >= MELOS_PARTIAL_MAX_HZ) {
        break;
      }
      // Tense -> ~1/n (bright sawtooth glottis); lax -> ~1/n^2 (dark, breathy).
      const float amp = (melosTenseness + (1.0f - melosTenseness) / static_cast<float>(n)) / static_cast<float>(n);
      buzz += sineLookup(melosVoicePhase[v] * static_cast<float>(n)) * amp;
    }
    source += buzz;
  }
  source /= static_cast<float>(MELOS_VOICE_COUNT);

  // Aspiration noise shares the vocal tract -> whispered vowels when breathy.
  const float noise = nextRandomBipolar();
  const float excitation = source * melosTenseness + noise * melosAspiration * 0.5f;

  // Three parallel formant resonators (TPT state-variable bandpass) = the mouth.
  static const float formantGain[3] = {1.0f, 0.7f, 0.42f};
  float formantOut = 0.0f;
  for (uint8_t k = 0; k < 3; ++k) {
    const float v3 = excitation - melosFormantS2[k];
    const float v1 = melosFormantA1[k] * melosFormantS1[k] + melosFormantA2[k] * v3;
    const float v2 = melosFormantS2[k] + melosFormantA2[k] * melosFormantS1[k] + melosFormantA3[k] * v3;
    melosFormantS1[k] = 2.0f * v1 - melosFormantS1[k];
    melosFormantS2[k] = 2.0f * v2 - melosFormantS2[k];
    formantOut += v1 * formantGain[k];  // bandpass output
  }

  // Slow archive envelope shaping + breathing arc + LOGOS sidechain duck.
  melosEnvPhase += MELOS_ENV_INC;
  melosEnvPhase -= floorf(melosEnvPhase);
  const float envPos = melosEnvPhase * ENV_BINS;
  uint8_t envBin = static_cast<uint8_t>(envPos);
  if (envBin >= ENV_BINS) {
    envBin = ENV_BINS - 1;
  }
  const uint8_t envBinNext = (envBin + 1) % ENV_BINS;
  const float envFrac = envPos - floorf(envPos);
  const float envVal = melosSmoothedEnv[envBin] * (1.0f - envFrac) + melosSmoothedEnv[envBinNext] * envFrac;
  const float envShape = 0.7f + 0.3f * envVal;
  // Sidechain ducking removed: MELOS amplitude no longer pumps with LOGOS.
  const float amp = melosBreathArc * envShape;

  melosSampleCounter++;
  return softClip(formantOut * amp * MELOS_GAIN * MELOS_MAKEUP);
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
      melosBlockUpdate();  // slow easing + formant coeffs, once per block
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

uint8_t activeLogosStreamCount() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    if (logosStreams[i].playing) {
      count++;
    }
  }
  return count;
}

void exciteMelosFromLogos(uint8_t index, bool granular, bool reversed, float rate) {
  const float streamWeight = index == 0 ? 0.16f : 0.24f;
  melosLogosOpen += (granular ? 0.34f : streamWeight) * MELOS_LOGOS_COUPLING;
  if (reversed || granular) {
    melosLogosConstrict += (reversed ? 0.28f : 0.18f) * MELOS_LOGOS_COUPLING;
  }
  if (!granular && rate > 0.92f && rate < 1.08f) {
    melosLogosStabilize += 0.26f * MELOS_LOGOS_COUPLING;
  } else if (rate < 0.80f) {
    melosLogosStabilize += 0.12f * MELOS_LOGOS_COUPLING;
    melosLogosConstrict += 0.10f * MELOS_LOGOS_COUPLING;
  }

  melosLogosOpen = constrain(melosLogosOpen, 0.0f, 1.0f);
  melosLogosConstrict = constrain(melosLogosConstrict, 0.0f, 1.0f);
  melosLogosStabilize = constrain(melosLogosStabilize, 0.0f, 1.0f);
  // Vowel/openness now follow these coupling values directly in melosBlockUpdate;
  // no discrete vocal modes in the collective-body engine.
}

void followLogosEnvelope(float sample) {
  const float a = fabsf(sample);
  const float coeff = a > melosLogosEnvelope ? 0.018f : 0.0012f;
  melosLogosEnvelope += (a - melosLogosEnvelope) * coeff;
  melosLogosEnvelope = constrain(melosLogosEnvelope, 0.0f, 1.0f);
}

void clearLogosVoices(LogosStream &stream) {
  for (uint8_t i = 0; i < LOGOS_MAX_CLOUD_VOICES; ++i) {
    stream.voices[i].active = false;
    stream.voices[i].samplesRemaining = 0;
  }
  stream.voiceCount = 0;
}

uint32_t setupLogosVoice(LogosStream &stream, uint8_t voiceIndex, uint32_t sourceStart, uint32_t sourceSamples,
                         uint32_t delaySamples, float rate, bool reversed) {
  if (voiceIndex >= LOGOS_MAX_CLOUD_VOICES || sourceSamples == 0 || sourceStart >= stream.bufferSamples) {
    return 0;
  }
  if (sourceStart + sourceSamples > stream.bufferSamples) {
    sourceSamples = stream.bufferSamples - sourceStart;
  }
  LogosVoice &voice = stream.voices[voiceIndex];
  const float absRate = fabsf(rate) < 0.05f ? 1.0f : fabsf(rate);
  uint32_t outputSamples = static_cast<uint32_t>(sourceSamples / absRate);
  if (outputSamples == 0) {
    outputSamples = 1;
  }
  voice.active = true;
  voice.reversed = reversed;
  voice.delaySamples = delaySamples;
  voice.sourceStart = sourceStart;
  voice.sourceEnd = sourceStart + sourceSamples;
  voice.samplesRemaining = outputSamples;
  voice.samplesTotal = outputSamples;
  voice.samplesPlayed = 0;
  voice.rate = reversed ? -absRate : absRate;
  voice.readPos = reversed ? static_cast<float>(voice.sourceEnd - 1) : static_cast<float>(voice.sourceStart);
  return delaySamples + outputSamples;
}

uint8_t buildWavesetIndex(const LogosStream &stream, WavesetIndex &wavesets) {
  wavesets.count = 0;
  if (stream.bufferSamples < 8) {
    return 0;
  }
  wavesets.boundary[wavesets.count++] = 0;
  int16_t prev = stream.buffer[0];
  const uint32_t minGap = 8;
  uint32_t lastBoundary = 0;
  for (uint32_t i = 1; i + 1 < stream.bufferSamples && wavesets.count < LOGOS_WAVESET_BOUNDARIES - 1; ++i) {
    const int16_t cur = stream.buffer[i];
    const bool crossing = (prev <= 0 && cur > 0) || (prev >= 0 && cur < 0);
    if (crossing && i > lastBoundary + minGap) {
      wavesets.boundary[wavesets.count++] = i;
      lastBoundary = i;
    }
    prev = cur;
  }
  if (wavesets.boundary[wavesets.count - 1] < stream.bufferSamples - 1 && wavesets.count < LOGOS_WAVESET_BOUNDARIES) {
    wavesets.boundary[wavesets.count++] = stream.bufferSamples - 1;
  }
  return wavesets.count;
}

bool chooseWavesetGroup(const WavesetIndex &wavesets, uint32_t &sourceStart, uint32_t &sourceSamples) {
  if (wavesets.count < 4) {
    return false;
  }
  const uint8_t maxGroups = (wavesets.count - 1) < LOGOS_WAVESET_MAX_GROUPS ? (wavesets.count - 1) : LOGOS_WAVESET_MAX_GROUPS;
  const uint8_t minGroups = LOGOS_WAVESET_MIN_GROUPS < maxGroups ? LOGOS_WAVESET_MIN_GROUPS : maxGroups;
  const uint8_t groups = randomRangeMs(minGroups, maxGroups);
  const uint8_t maxStartBoundary = wavesets.count > groups + 1 ? wavesets.count - groups - 1 : 0;
  const uint8_t startBoundary = maxStartBoundary > 0 ? (nextRandomU32() % (maxStartBoundary + 1)) : 0;
  sourceStart = wavesets.boundary[startBoundary];
  const uint32_t sourceEnd = wavesets.boundary[startBoundary + groups];
  if (sourceEnd <= sourceStart + 2) {
    return false;
  }
  sourceSamples = sourceEnd - sourceStart;
  return true;
}

uint8_t logosCloudVoiceCount(uint8_t index) {
  if (logosDensityMode == LOGOS_MODE_SWARM) {
    if (index == 0) {
      return randomRangeMs(3, 6);
    }
    return randomRangeMs(6, LOGOS_MAX_CLOUD_VOICES);
  }
  if (logosDensityMode == LOGOS_MODE_SPARSE) {
    return index == 0 ? randomRangeMs(1, 2) : randomRangeMs(2, 4);
  }
  if (index == 0) {
    return randomRangeMs(2, 4);
  }
  if (index == 1) {
    return randomRangeMs(3, 6);
  }
  if (index == 2) {
    return randomRangeMs(4, 7);
  }
  return randomRangeMs(5, LOGOS_MAX_CLOUD_VOICES);
}

float logosDensityGain() {
  if (logosDensityMode == LOGOS_MODE_SWARM) {
    return 1.22f;
  }
  if (logosDensityMode == LOGOS_MODE_SPARSE) {
    return 0.86f;
  }
  return 1.0f;
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

  const uint32_t availableStartMs = grain.startMs + extraOffsetMs;
  const uint32_t availableDurationMs = grain.durationMs - extraOffsetMs;
  uint32_t playbackStartMs = availableStartMs;
  uint32_t playbackDurationMs = availableDurationMs;
  // Rare clear word (stream 1 only). Otherwise every grain is deconstructed, so
  // intelligible whole words never slip through by accident.
  const bool clearWord = (nextRandomU32() % 100UL) < LOGOS_WORD_PERCENT;
  stream.cloud = !clearWord && repeatEligible && availableDurationMs >= LOGOS_MIN_DURATION_MS &&
                 (nextRandomU32() % 100UL) < logosCloudChance(index);
  stream.granular = stream.cloud;
  if (!clearWord && !stream.cloud && playbackDurationMs > LOGOS_MICRO_MIN_MS) {
    // not a clear word -> force a short fragment, never an intelligible whole grain
    const uint32_t maxMicroMs = LOGOS_MICRO_MAX_MS < playbackDurationMs ? LOGOS_MICRO_MAX_MS : playbackDurationMs;
    uint32_t microDurationMs = randomRangeMs(LOGOS_MICRO_MIN_MS, maxMicroMs);
    const uint32_t maxOffsetMs = playbackDurationMs - microDurationMs;
    playbackStartMs += randomRangeMs(0, maxOffsetMs);
    playbackDurationMs = microDurationMs;
    stream.granular = true;
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

  stream.bufferSamples = loaded;
  clearLogosVoices(stream);
  stream.rate = 1.0f;
  stream.reversed = false;

  uint32_t outputTotal = 0;
  if (stream.cloud) {
    stream.cloudPattern = chooseLogosCloudPattern(index);
    WavesetIndex wavesets;
    const bool haveWavesets = stream.cloudPattern == 4 && buildWavesetIndex(stream, wavesets) >= 4;
    if (stream.cloudPattern == 4 && !haveWavesets) {
      stream.cloudPattern = 3;
    }
    const uint8_t desiredVoices = logosCloudVoiceCount(index);
    const uint32_t baseDurationMs = logosRoleDurationMs(index, playbackDurationMs);
    uint32_t baseSamples = baseDurationMs * (SAMPLE_RATE / 1000UL);
    if (baseSamples == 0) {
      baseSamples = 1;
    }
    if (baseSamples > loaded) {
      baseSamples = loaded;
    }
    const uint32_t maxBaseStart = loaded > baseSamples ? loaded - baseSamples : 0;
    const uint32_t baseStart = maxBaseStart > 0 ? (nextRandomU32() % (maxBaseStart + 1)) : 0;
    const bool scrubForward = (nextRandomU32() & 1UL) != 0;
    const uint32_t scrubStepSamples = randomRangeMs(20, 65) * (SAMPLE_RATE / 1000UL);
    uint32_t repeatedWaveStart = 0;
    uint32_t repeatedWaveSamples = 0;
    const bool wavesetRepeat = haveWavesets && ((nextRandomU32() % 100UL) < 42);
    if (wavesetRepeat) {
      chooseWavesetGroup(wavesets, repeatedWaveStart, repeatedWaveSamples);
    }

    for (uint8_t v = 0; v < desiredVoices && v < LOGOS_MAX_CLOUD_VOICES; ++v) {
      uint32_t sourceDurationMs = logosRoleDurationMs(index, playbackDurationMs);
      if (stream.cloudPattern == 0) {
        sourceDurationMs = baseDurationMs;
      } else if (stream.cloudPattern == 1 && index >= 2) {
        sourceDurationMs = randomRangeMs(LOGOS_DUST_MIN_MS, LOGOS_DUST_MAX_MS);
      }
      uint32_t voiceSamples = sourceDurationMs * (SAMPLE_RATE / 1000UL);
      if (voiceSamples == 0) {
        voiceSamples = 1;
      }
      if (voiceSamples > loaded) {
        voiceSamples = loaded;
      }
      const uint32_t maxStart = loaded > voiceSamples ? loaded - voiceSamples : 0;
      uint32_t sourceStart = maxStart > 0 ? (nextRandomU32() % (maxStart + 1)) : 0;
      uint32_t delayMs = v == 0 ? 0 : randomRangeMs(LOGOS_CLOUD_MIN_DELAY_MS, index == 3 ? 260UL : LOGOS_CLOUD_MAX_DELAY_MS);
      float voiceRate = logosRoleRate(index);
      if (stream.cloudPattern == 4 && haveWavesets) {
        if (wavesetRepeat && repeatedWaveSamples > 0) {
          sourceStart = repeatedWaveStart;
          voiceSamples = repeatedWaveSamples;
        } else if (!chooseWavesetGroup(wavesets, sourceStart, voiceSamples)) {
          sourceStart = maxStart > 0 ? (nextRandomU32() % (maxStart + 1)) : 0;
        }
        delayMs = v * randomRangeMs(32, 105);
        voiceRate = (nextRandomU32() % 100UL) < 70 ? 1.0f : logosRoleRate(index);
      } else if (stream.cloudPattern == 0) {
        const int32_t jitter = static_cast<int32_t>(randomRangeMs(0, 24)) - 12;
        sourceStart = baseStart;
        if (jitter > 0 && sourceStart + static_cast<uint32_t>(jitter) * (SAMPLE_RATE / 1000UL) < maxStart) {
          sourceStart += static_cast<uint32_t>(jitter) * (SAMPLE_RATE / 1000UL);
        }
        delayMs = v * randomRangeMs(62, 138);
        voiceRate = (v % 3 == 2) ? logosRoleRate(index) : 1.0f;
      } else if (stream.cloudPattern == 1) {
        voiceRate = logosArpRate(v + (nextRandomU32() % 3UL));
        delayMs = v * randomRangeMs(58, 155);
      } else if (stream.cloudPattern == 2) {
        uint32_t scrubStart = baseStart;
        const uint32_t step = scrubStepSamples * v;
        if (scrubForward) {
          scrubStart = baseStart + step;
          if (scrubStart > maxStart) {
            scrubStart = maxStart;
          }
        } else {
          scrubStart = baseStart > step ? baseStart - step : 0;
        }
        sourceStart = scrubStart;
        delayMs = v * randomRangeMs(45, 120);
        voiceRate = (nextRandomU32() & 1UL) ? 0.75f : 1.3333f;
      } else {
        sourceStart = maxStart > 0 ? (nextRandomU32() % (maxStart + 1)) : 0;
        delayMs = randomRangeMs(0, index == 3 ? 210UL : 360UL);
        voiceRate = logosRoleRate(index);
        if ((nextRandomU32() % 100UL) < 34) {
          voiceRate = logosArpRate(nextRandomU32() % 6UL);
        }
      }
      uint8_t reverseChance = index == 0 ? 12 : (index == 1 ? 56 : (index == 2 ? 38 : 30));
      if (stream.cloudPattern == 3) {
        reverseChance += 14;
      } else if (stream.cloudPattern == 4) {
        reverseChance += 22;
      }
      const bool voiceReversed = (nextRandomU32() % 100UL) < reverseChance;
      const uint32_t voiceEnd = setupLogosVoice(stream, stream.voiceCount, sourceStart, voiceSamples,
                                                delayMs * (SAMPLE_RATE / 1000UL), voiceRate, voiceReversed);
      if (voiceEnd > 0) {
        stream.rate = voiceRate;
        stream.reversed = stream.reversed || voiceReversed;
        if (voiceEnd > outputTotal) {
          outputTotal = voiceEnd;
        }
        stream.voiceCount++;
      }
    }
  } else if (clearWord) {
    // intelligible: whole grain, forward, normal speed
    stream.reversed = false;
    stream.rate = 1.0f;
    outputTotal = setupLogosVoice(stream, 0, 0, loaded, 0, stream.rate, stream.reversed);
    if (outputTotal > 0) {
      stream.voiceCount = 1;
    }
  } else {
    const uint8_t reverseChance = index == 0 ? 18 : (index == 1 ? 58 : (index == 2 ? 40 : 32));
    stream.reversed = (nextRandomU32() % 100UL) < reverseChance;
    if ((nextRandomU32() % 100UL) < LOGOS_MUSICAL_RATE_PERCENT) {
      stream.rate = logosRoleRate(index);
    }
    outputTotal = setupLogosVoice(stream, 0, 0, loaded, 0, stream.rate, stream.reversed);
    if (outputTotal > 0) {
      stream.voiceCount = 1;
    }
  }

  if (stream.voiceCount == 0 || outputTotal == 0) {
    return false;
  }

  stream.readPos = 0.0f;
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
  exciteMelosFromLogos(index, stream.granular, stream.reversed, stream.rate);
  setLogosStreamGain(index, 0.0f);

  Serial.print("LOGOS ");
  Serial.print(index + 1);
  Serial.print(' ');
  Serial.print(grain.filename);
  Serial.print(stream.cloud ? " cloud" : (stream.granular ? " micro" : " word"));
  if (stream.cloud) {
    Serial.print(" ");
    Serial.print(logosCloudPatternName(stream.cloudPattern));
    Serial.print(" voices=");
    Serial.print(stream.voiceCount);
  }
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
          for (uint8_t v = 0; v < stream.voiceCount; ++v) {
            LogosVoice &voice = stream.voices[v];
            if (!voice.active || voice.samplesRemaining == 0 || stream.samplesPlayed < voice.delaySamples) {
              continue;
            }

            float pos = voice.readPos;
            if (pos < 0.0f) {
              pos = 0.0f;
            }
            const float maxPos = static_cast<float>(stream.bufferSamples > 0 ? stream.bufferSamples - 1 : 0);
            if (pos > maxPos) {
              pos = maxPos;
            }
            uint32_t idx = static_cast<uint32_t>(pos);
            if (idx >= stream.bufferSamples) {
              idx = stream.bufferSamples - 1;
            }
            uint32_t idx2 = idx;
            if (voice.rate >= 0.0f) {
              idx2 = (idx + 1 < stream.bufferSamples) ? idx + 1 : idx;
            } else {
              idx2 = idx > 0 ? idx - 1 : idx;
            }
            const float frac = pos - floorf(pos);
            float voiceSample = (static_cast<float>(stream.buffer[idx]) * (1.0f - frac) +
                                 static_cast<float>(stream.buffer[idx2]) * frac) / 32768.0f;
            voice.readPos += voice.rate;

            float voiceGain = 1.0f;
            if (voice.samplesPlayed < fadeSamples) {
              voiceGain *= static_cast<float>(voice.samplesPlayed) / fadeSamples;
            }
            if (voice.samplesRemaining < fadeSamples) {
              voiceGain *= static_cast<float>(voice.samplesRemaining) / fadeSamples;
            }
            sample += voiceSample * voiceGain;

            voice.samplesRemaining--;
            voice.samplesPlayed++;
            if (voice.samplesRemaining == 0) {
              voice.active = false;
            }
          }
        }
        const float cloudNorm = stream.voiceCount > 1 ? (1.0f + 0.18f * static_cast<float>(stream.voiceCount - 1)) : 1.0f;
        logosDensityGainCur += (logosDensityGain() - logosDensityGainCur) * LOGOS_DENSITY_GAIN_EASE;
        const float logosOut = compressLogos(sample * (LOGOS_STREAM_GAIN * logosDensityGainCur / cloudNorm));
        followLogosEnvelope(logosOut);
        buffer[i] = floatToInt16(logosOut);

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
  if (logosDensitySwitchMs == 0 || static_cast<int32_t>(now - logosDensitySwitchMs) >= 0) {
    chooseLogosDensityMode();
    for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
      if (logosDensityMode == LOGOS_MODE_SWARM && i > 0) {
        logosStreams[i].activeMode = true;
      }
      scheduleLogosStream(logosStreams[i], i);
    }
  }

  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    LogosStream &stream = logosStreams[i];

    if (i > 0 && logosDensityMode != LOGOS_MODE_SWARM && static_cast<int32_t>(now - stream.stateSwitchMs) >= 0) {
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
  Serial.print("  dominantBand: ");
  Serial.println(dominantBandIndex(model.bands));
  Serial.print("  learnAmount: ");
  Serial.println(melosLearnAmount, 4);
  Serial.print("  logosEnvelope: ");
  Serial.println(melosLogosEnvelope, 4);
}

void printGrains() {
  grainCount = countGrains();
  Serial.print("GRAINS count: ");
  Serial.println(grainCount);
  Serial.print("LOGOS density: ");
  Serial.println(logosDensityModeName());
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
