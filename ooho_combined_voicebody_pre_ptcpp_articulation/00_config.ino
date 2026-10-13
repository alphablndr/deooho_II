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

constexpr float MELOS_GAIN = 0.13f;           // quiet supporting body (halved again)
constexpr float LOGOS_STREAM_GAIN = 0.60f;    // +25%, then compressed below
constexpr float LOGOS_COMP_THRESHOLD = 0.18f;
constexpr float LOGOS_COMP_RATIO = 3.0f;
constexpr float LOGOS_COMP_MAKEUP = 1.30f;
constexpr uint32_t LOGOS_FADE_MS = 24;
constexpr uint32_t AUDIO_FEED_MIN_US = 2300;
constexpr float DC_BLOCK_R = 0.9995f;  // one-pole DC blocker, ~3.5 Hz cutoff
// Ease the density-mode bus gain so sparse/present/swarm changes don't step
// the LOGOS level (that step was an audible click every 18-56 s).
constexpr float LOGOS_DENSITY_GAIN_EASE = 1.0f / (0.18f * SAMPLE_RATE);  // ~180 ms

// MELOS live synthesis
constexpr float MELOS_PARAM_EASE = 1.0f / (4.0f * SAMPLE_RATE);   // ~4 s ease toward model
constexpr float MELOS_ENV_INC = 1.0f / (28.0f * SAMPLE_RATE);     // ~28 s envelope cycle
constexpr float MELOS_PARTIAL_MAX_HZ = 8000.0f;
constexpr uint16_t MELOS_SINE_SIZE = 2048;
constexpr uint8_t MELOS_VOICE_COUNT = 6;
constexpr uint8_t MELOS_TRACT_SECTIONS = 3;

// MELOS collective vocal body (variant A): source-formant-filter engine.
constexpr uint16_t MELOS_SOURCE_HARMONICS = 16;   // glottal buzz harmonics per voice
constexpr float MELOS_SPREAD_MAX_SEMITONES = 5.2f; // widest bloom at max archive spread
constexpr float MELOS_DETUNE_CENTS = 11.0f;        // per-voice humanizing detune
constexpr float MELOS_CONSONANCE_PULL = 0.30f;     // pull toward unison/octave/fifth
constexpr float MELOS_VOWEL_EASE = 1.0f / (3.0f * SAMPLE_RATE);   // ~3 s vowel/detune morph
constexpr float MELOS_F0_EASE = 1.0f / (4.0f * SAMPLE_RATE);      // ~4 s pitch center ease
constexpr uint32_t MELOS_BREATH_MIN_MS = 4000;
constexpr uint32_t MELOS_BREATH_MAX_MS = 8000;
constexpr float MELOS_BREATH_EASE = 1.0f / (5.0f * SAMPLE_RATE);  // ~5 s glide, very smooth
constexpr float MELOS_MAKEUP = 2.45f;              // post-formant makeup before soft clip
constexpr float MELOS_VIBRATO_RATE_HZ = 5.15f;     // subtle synthetic voice instability
constexpr float MELOS_VIBRATO_DEPTH = 0.0045f;     // ~0.45%, stronger during speech-ghost
// Timbral movement (Pink-Trombone-inspired): nasality, wandering constriction,
// vowel micro-wander. All drift autonomously but around archive-set centers.
constexpr float MELOS_NASAL_EASE = 1.0f / (3.5f * SAMPLE_RATE);
constexpr float MELOS_NASAL_FREQ = 270.0f;         // nasal murmur pole
constexpr float MELOS_NASAL_Q = 7.0f;
constexpr float MELOS_CONSTRICT_EASE = 1.0f / (3.0f * SAMPLE_RATE);
constexpr float MELOS_VOWEL_WANDER = 0.40f;        // max autonomous vowel drift (audible)
constexpr float MELOS_LOGOS_OPEN_DECAY = 1.0f / (2.4f * SAMPLE_RATE);
constexpr float MELOS_LOGOS_CONSTRICT_DECAY = 1.0f / (1.1f * SAMPLE_RATE);
constexpr float MELOS_LOGOS_STABILIZE_DECAY = 1.0f / (3.2f * SAMPLE_RATE);
constexpr float MELOS_LOGOS_DENSITY_EASE = 1.0f / (1.5f * SAMPLE_RATE);
constexpr float MELOS_LOGOS_COUPLING = 1.25f;
constexpr float MELOS_LEARN_DECAY = 1.0f / (18.0f * SAMPLE_RATE);
constexpr float MELOS_TONGUE_EASE = 1.0f / (2.6f * SAMPLE_RATE);
constexpr float MELOS_TONGUE_DIAMETER_EASE = 1.0f / (1.8f * SAMPLE_RATE);
constexpr float MELOS_ARTICULATION_DECAY = 1.0f / (1.7f * SAMPLE_RATE);
constexpr float MELOS_ARTICULATION_EASE = 1.0f / (0.42f * SAMPLE_RATE);
constexpr float MELOS_FRONT_FRICATIVE_GAIN = 0.22f;
constexpr float MELOS_TRACT_EASE = 1.0f / (2.2f * SAMPLE_RATE);
constexpr float MELOS_TRACT_Q = 3.4f;
constexpr float MELOS_TRACT_BLEND = 0.34f;
constexpr float MELOS_TRACT_DRIVE = 0.58f;
constexpr float MELOS_SPEECH_GHOST_EASE = 1.0f / (0.18f * SAMPLE_RATE);
constexpr uint32_t MELOS_SPEECH_GHOST_MIN_MS = 900;
constexpr uint32_t MELOS_SPEECH_GHOST_MAX_MS = 6200;
constexpr float MELOS_SPEECH_GHOST_MIN_RATE_HZ = 4.0f;
constexpr float MELOS_SPEECH_GHOST_MAX_RATE_HZ = 8.8f;
constexpr uint32_t MELOS_MURMUR_MIN_MS = 18000;
constexpr uint32_t MELOS_MURMUR_MAX_MS = 45000;

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
constexpr uint32_t LOGOS_PHRASE_MIN_MS = 1500;
constexpr uint32_t LOGOS_PHRASE_MAX_MS = 4000;
constexpr uint32_t LOGOS_PHRASE_PREROLL_MAX_MS = 900;
constexpr uint32_t LOGOS_SPLIT_DURATION_MS = 1200;
constexpr uint32_t LOGOS_SPLIT_GAP_MS = 160;
constexpr float LOGOS_MIN_RMS_THRESHOLD = 0.008f;
constexpr float LOGOS_MAX_RMS_THRESHOLD = 0.090f;
constexpr float LOGOS_WINDOW_PEAK_FACTOR = 0.18f;
// Smarter grain slicing: hysteresis (capture consonants/soft edges), split long
// regions at the quietest internal point, snap grain edges to zero crossings.
constexpr float LOGOS_HYST_FACTOR = 0.45f;          // continue threshold = start * this
constexpr uint32_t LOGOS_SPLIT_SEARCH_MS = 260;     // search radius for a quiet cut
constexpr uint32_t LOGOS_ZC_SEARCH_SAMPLES = 256;   // ~6 ms zero-crossing snap radius
constexpr uint32_t LOGOS_MAX_WINDOWS = ((FIELD_RECORD_BYTES / 2) / LOGOS_WINDOW_SAMPLES) + 2;

// LOGOS reverse / rate variation
constexpr uint8_t LOGOS_REVERSE_PERCENT = 30;
constexpr uint8_t LOGOS_RATE_PERCENT = 20;
constexpr float LOGOS_RATE_SLOW = 0.70f;
constexpr float LOGOS_RATE_FAST = 1.40f;
constexpr uint32_t LOGOS_MAX_GRAIN_SAMPLES = ((LOGOS_PHRASE_MAX_MS + 50) * SAMPLE_RATE) / 1000UL;
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
constexpr uint8_t LOGOS_PHRASE_GHOST_PERCENT = 18;
constexpr uint8_t LOGOS_RESPONSE_CHANCE_PERCENT = 42;
constexpr uint8_t LOGOS_REVERSE_TAIL_CHANCE_PERCENT = 30;
constexpr uint32_t LOGOS_RESPONSE_MIN_DELAY_MS = 180;
constexpr uint32_t LOGOS_RESPONSE_MAX_DELAY_MS = 820;
constexpr uint32_t LOGOS_RESPONSE_MIN_MS = 90;
constexpr uint32_t LOGOS_RESPONSE_MAX_MS = 360;
constexpr uint32_t LOGOS_REVERSE_TAIL_MIN_MS = 180;
constexpr uint32_t LOGOS_REVERSE_TAIL_MAX_MS = 520;
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
constexpr uint32_t LOGOS_ARCHIVE_FULL_GRAINS = 240;
constexpr uint8_t LOGOS_ARCHIVE_INTERVAL_REDUCTION = 38;
constexpr uint8_t LOGOS_ARCHIVE_SWARM_INTERVAL_REDUCTION = 52;
constexpr uint8_t LOGOS_ARCHIVE_DORMANT_REDUCTION = 62;
constexpr uint8_t LOGOS_ARCHIVE_VOICE_BOOST_MAX = 3;
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

constexpr uint8_t LOGOS_LAUNCH_AUTO = 0;
constexpr uint8_t LOGOS_LAUNCH_RESPONSE = 1;
constexpr uint8_t LOGOS_LAUNCH_REVERSE_TAIL = 2;
constexpr uint8_t LOGOS_LAUNCH_PHRASE_GHOST = 3;

const char COUNTER_FILE[] = "COUNTER.TXT";
const char GRAINS_FILE[] = "GRAINS.CSV";
const char MODEL_FILE[] = "MODEL.BIN";
const char MODEL_TEMP_FILE[] = "MODEL.TMP";

extern "C" uint8_t external_psram_size;



// ---------------- Core data types ----------------

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

struct LogosPendingResponse {
  bool pending = false;
  uint8_t sourceStream = 0;
  uint32_t dueMs = 0;
  GrainInfo grain;
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
  float dcX1 = 0.0f;             // per-stream DC blocker state
  float dcY1 = 0.0f;
  bool granular = false;
  bool cloud = false;
  bool clearWord = false;
  uint8_t cloudPattern = 0;
  uint8_t launchMode = LOGOS_LAUNCH_AUTO;
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



// ---------------- Cross-tab declarations ----------------
void setStatusLed(bool on);
void updateStatusLed();
void enterState(DeviceState nextState);
bool playbackServiceAllowed();
void workTick();
void updateButton(DebouncedButton &button);
void drainRecordQueue();
void stopLogosPlayback();
void stopAllPlayback();
bool saveRawRecording();
void startRecording();
bool processRecording();
void stopRecording();

uint32_t nextRandomU32();
float nextRandomBipolar();
float nextRandomGaussian(float sigma);
uint32_t randomRangeMs(uint32_t minMs, uint32_t maxMs);
float softClip(float x);
float compressLogos(float x);
int16_t floatToInt16(float x);
void initSineTable();
float sineLookup(float phase);
uint8_t bandForFreq(float f);

void writeLe16(uint8_t *buffer, uint16_t value);
void writeLe32(uint8_t *buffer, uint32_t value);
bool writeExact(FsFile &file, const void *data, size_t bytes);
bool writeWavHeader(FsFile &file, uint32_t dataBytes);
void makeFilename(char *buffer, uint16_t index);
void makeTempFilename(char *buffer, uint16_t index);
uint16_t scanNextIndex();
uint16_t loadCounter();
bool saveCounter(uint16_t value);
void initDefaultModel();
bool modelLooksValid(const Model &candidate);
void loadModel();
bool saveModel();

uint32_t musicalLogosIntervalMs(uint8_t index);
uint8_t logosArchivePressure();
const char *logosDensityModeName();
void chooseLogosDensityMode();
float musicalLogosRate(uint8_t index);
uint8_t logosMicroChance(uint8_t index);
uint8_t logosCloudChance(uint8_t index);
float logosRoleRate(uint8_t index);
float logosArpRate(uint8_t step);
uint8_t chooseLogosCloudPattern(uint8_t index);
const char *logosCloudPatternName(uint8_t pattern);
uint32_t logosRoleDurationMs(uint8_t index, uint32_t availableMs);
void scheduleLogosStream(LogosStream &stream, uint8_t index);
void scheduleLogosStateSwitch(LogosStream &stream);
void initializeLogosSchedulers();
void scheduleLogosResponse(uint8_t sourceIndex, const GrainInfo &grain);
void serviceLogosResponses();
uint32_t countGrains();
bool readCsvLine(FsFile &file, char *line, size_t maxLen);
bool parseGrainLine(const char *line, GrainInfo &grain);
bool isRecentGrainIndex(uint32_t index);
void rememberGrainIndex(uint32_t index);
bool isRecentRecordingFilename(const char *filename);
bool selectRecentRecordingGrain(GrainInfo &grain);
bool selectRandomGrain(GrainInfo &grain);
bool appendGrain(const char *filename, uint32_t startMs, uint32_t durationMs);

uint32_t snapToZeroCrossing(const int16_t *samples, uint32_t total, uint32_t pos, uint32_t radius);
uint32_t emitRegionGrains(const int16_t *samples, uint32_t total, uint32_t winStart, uint32_t winEnd, uint32_t s0, uint32_t s1);
void appendLogosGrains();
int compareFloats(const void *a, const void *b);
float estimateMedianF0(float &voicedRatio);
void analyzeEnvelopeAndRms(float envelopeOut[], float &rmsOut);
void analyzeBands(float bandsOut[]);
void updateCollectiveModel();
bool loadWavIntoRecordBuffer(const char *filename);
void rebuildGrainsFromArchive();

void computeMelosEnvelope();
uint32_t melosRandomSamples(uint32_t minMs, uint32_t maxMs);
uint8_t dominantBandIndex(const float bands[]);
void triggerMelosLearnedGesture(float incomingF0, float incomingVoicedRatio, const float incomingBands[]);
void triggerMelosSpeechGhost(float strength, uint32_t durationMs);
float melosVowelPosition();
float melosSpectralCentroid();
void melosFormantTargets(float pos, float &f1, float &f2, float &f3);
void melosApplyArticulation(float &f1, float &f2, float &f3, float &oralDamping, float &fricativeMix);
float melosNearestConsonant(float semis);
void melosComputeFormantCoeffs();
void melosComputeTractCoeffs();
void melosUpdateMovingTract(float constr);
float melosApplyMovingTract(float mouthOut);
void melosBlockUpdate();
void initMelosSynth();
float melosNextSample();
void feedMelosQueue();

void setLogosStreamGain(uint8_t index, float gain);
uint8_t activeLogosStreamCount();
void exciteMelosFromLogos(uint8_t index, bool granular, bool reversed, float rate);
void followLogosEnvelope(float sample);
void clearLogosVoices(LogosStream &stream);
uint32_t setupLogosVoice(LogosStream &stream, uint8_t voiceIndex, uint32_t sourceStart, uint32_t sourceSamples, uint32_t delaySamples, float rate, bool reversed);
uint8_t buildWavesetIndex(const LogosStream &stream, WavesetIndex &wavesets);
bool chooseWavesetGroup(const WavesetIndex &wavesets, uint32_t &sourceStart, uint32_t &sourceSamples);
uint8_t logosCloudVoiceCount(uint8_t index);
float logosDensityGain();
bool startLogosGrain(uint8_t index, const GrainInfo &grain, uint32_t extraOffsetMs, bool repeatEligible, uint8_t launchMode);
void finishLogosGrain(uint8_t index);
void feedLogosQueue(uint8_t index);
void servicePlaybackDuringWork();
void updateIdlePlayback();

void printModel();
void printGrains();
void printDiagnostics();
void playOneLogosNow();
void toggleDiagnosticTone();
void startMelosSweep();
void updateMelosSweep();
void handleSerial();
void handleRecordButton();
