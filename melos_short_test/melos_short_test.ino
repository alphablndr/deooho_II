/*
  The Ooho MELOS short render/playback test.

  Hardware:
  - Teensy 4.1 + Teensy Audio Shield Rev D
  - LINE OUT R -> amplifier -> surface exciter/cube
  - Optional external LED on pin 31 through resistor to GND
  - microSD card in built-in Teensy 4.1 slot

  Behavior:
  - Loads MODEL.BIN if present, otherwise uses a neutral default model
  - Renders MELOS.WAV as mono 44.1 kHz 16-bit, 30 seconds
  - Plays MELOS.WAV through AudioPlayQueue on right channel only
  - Serial: r = render MELOS.WAV, p = play/restart, s = stop, m = model summary
*/

#include <Audio.h>
#include <SD.h>
#include <SerialFlash.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>

constexpr int STATUS_LED_PIN = 31;
constexpr bool USE_EXTERNAL_STATUS_LED = true;
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr uint16_t AUDIO_MEMORY_BLOCKS = 80;
constexpr size_t SD_WRITE_CHUNK_BYTES = 4096;
constexpr float OUTPUT_VOLUME = 0.18f;
constexpr float MELOS_GAIN = 0.48f;
constexpr uint32_t MELOS_SECONDS = 30;
constexpr uint32_t MELOS_SAMPLES = SAMPLE_RATE * MELOS_SECONDS;
constexpr uint32_t MELOS_FADE_MS = 400;
constexpr uint32_t MODEL_MAGIC = 0x4F4F484FUL;  // "OOHO"
constexpr uint16_t MODEL_VERSION = 1;
constexpr uint8_t ENV_BINS = 64;
constexpr uint8_t BAND_COUNT = 8;
constexpr float DEFAULT_F0_HZ = 160.0f;
constexpr float DEFAULT_VOICED_RATIO = 0.5f;
constexpr float DEFAULT_RMS = 0.08f;
constexpr float F0_MIN_HZ = 70.0f;
constexpr float F0_MAX_HZ = 350.0f;

const char MODEL_FILE[] = "MODEL.BIN";
const char MELOS_FILE[] = "MELOS.WAV";
const char MELOS_TEMP_FILE[] = "MTEMP.WAV";

AudioPlayQueue melosQueue;
AudioOutputI2S i2sOut;
AudioConnection patchCordRight(melosQueue, 0, i2sOut, 1);
AudioControlSGTL5000 audioShield;

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

Model model;
FsFile melosFile;
bool playing = false;
bool queuedFreshAudio = false;
uint32_t rngState = 0xBEEFBADUL;

void setStatusLed(bool on) {
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
  if (USE_EXTERNAL_STATUS_LED) {
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
  }
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

int16_t floatToInt16(float value) {
  value = constrain(value, -1.0f, 1.0f);
  return static_cast<int16_t>(value * 32767.0f);
}

float softClip(float value) {
  return tanhf(value * 1.4f) / 1.4f;
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
  return candidate.voicedRatio >= 0.0f && candidate.voicedRatio <= 1.0f;
}

void loadModel() {
  FsFile file = SD.sdfs.open(MODEL_FILE, O_RDONLY);
  if (!file) {
    initDefaultModel();
    Serial.println("MODEL.BIN missing; using neutral default model.");
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
    Serial.println("MODEL.BIN corrupt; using neutral default model.");
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

void stopPlayback() {
  if (melosFile) {
    melosFile.close();
  }
  playing = false;
  queuedFreshAudio = false;
  setStatusLed(false);
  Serial.println("MELOS playback stopped.");
}

bool renderMelosWav() {
  stopPlayback();
  setStatusLed(true);

  if (SD.exists(MELOS_TEMP_FILE)) {
    SD.remove(MELOS_TEMP_FILE);
  }
  FsFile file = SD.sdfs.open(MELOS_TEMP_FILE, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    Serial.println("Could not open MTEMP.WAV.");
    setStatusLed(false);
    return false;
  }

  const uint32_t dataBytes = MELOS_SAMPLES * 2UL;
  if (!file.preAllocate(44 + dataBytes)) {
    Serial.println("Warning: MELOS preAllocate failed.");
  }
  if (!writeWavHeader(file, dataBytes)) {
    file.close();
    setStatusLed(false);
    return false;
  }

  float smoothedEnvelope[ENV_BINS];
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
    smoothedEnvelope[i] = sum / 10.0f;
    envelopeAverage += smoothedEnvelope[i];
  }
  envelopeAverage /= ENV_BINS;
  for (uint8_t i = 0; i < ENV_BINS; ++i) {
    smoothedEnvelope[i] = envelopeAverage * 0.82f + smoothedEnvelope[i] * 0.18f;
  }

  const float centers[BAND_COUNT] = {175.0f, 375.0f, 700.0f, 1200.0f, 2000.0f, 3250.0f, 5000.0f, 8500.0f};
  float phases[BAND_COUNT] = {0.0f};
  int16_t block[256];
  size_t blockUsed = 0;
  const float f0 = constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ);
  float mainPhase = 0.0f;
  float drift = 0.0f;
  float driftTarget = 0.0f;
  float breathFast = 0.0f;
  float breathSlow = 0.0f;
  const uint32_t fadeSamples = MELOS_FADE_MS * (SAMPLE_RATE / 1000UL);

  Serial.print("Rendering ");
  Serial.print(MELOS_FILE);
  Serial.print(" seconds=");
  Serial.println(MELOS_SECONDS);

  for (uint32_t i = 0; i < MELOS_SAMPLES; ++i) {
    const float normalized = static_cast<float>(i) / static_cast<float>(MELOS_SAMPLES - 1);
    uint32_t envBin = normalized * ENV_BINS;
    if (envBin >= ENV_BINS) {
      envBin = ENV_BINS - 1;
    }
    float amp = smoothedEnvelope[envBin] * 0.34f + 0.12f;
    if (i < fadeSamples) {
      amp *= static_cast<float>(i) / fadeSamples;
    } else if (MELOS_SAMPLES - i < fadeSamples) {
      amp *= static_cast<float>(MELOS_SAMPLES - i) / fadeSamples;
    }

    if ((i & 0x3FFUL) == 0) {
      driftTarget += nextRandomGaussian(0.0012f);
      driftTarget = constrain(driftTarget, -0.035f, 0.035f);
    }
    drift += (driftTarget - drift) * 0.0008f;
    mainPhase += f0 * (1.0f + drift) / SAMPLE_RATE;
    mainPhase -= floorf(mainPhase);

    const float phaseRadians = mainPhase * 2.0f * PI;
    const float voiced = sinf(phaseRadians) * 0.78f +
                         sinf(phaseRadians * 2.0f) * 0.15f +
                         sinf(phaseRadians * 3.0f) * 0.07f;

    float formantColor = 0.0f;
    for (uint8_t band = 0; band < BAND_COUNT; ++band) {
      phases[band] += 2.0f * PI * centers[band] / SAMPLE_RATE;
      if (phases[band] > 2.0f * PI) {
        phases[band] -= 2.0f * PI;
      }
      formantColor += sinf(phases[band]) * sqrtf(model.bands[band] * BAND_COUNT);
    }
    formantColor /= BAND_COUNT;

    const float white = nextRandomBipolar();
    breathFast = breathFast * 0.88f + white * 0.12f;
    breathSlow = breathSlow * 0.997f + white * 0.003f;
    const float breath = (breathFast - breathSlow) * 0.42f;
    const float body = voiced * (0.82f + formantColor * 0.18f);
    const float colored = body * model.voicedRatio + breath * (1.0f - model.voicedRatio);

    block[blockUsed++] = floatToInt16(softClip(colored * amp * MELOS_GAIN));
    if (blockUsed == 256) {
      if (!writeExact(file, block, sizeof(block))) {
        file.close();
        setStatusLed(false);
        return false;
      }
      blockUsed = 0;
    }
  }

  if (blockUsed > 0 && !writeExact(file, block, blockUsed * sizeof(int16_t))) {
    file.close();
    setStatusLed(false);
    return false;
  }

  const bool ok = file.truncate(44 + dataBytes) && file.sync();
  file.close();
  if (!ok) {
    Serial.println("MELOS.WAV finalize failed.");
    setStatusLed(false);
    return false;
  }
  if (SD.exists(MELOS_FILE)) {
    SD.remove(MELOS_FILE);
  }
  if (!SD.rename(MELOS_TEMP_FILE, MELOS_FILE)) {
    Serial.println("Could not rename MTEMP.WAV to MELOS.WAV.");
    setStatusLed(false);
    return false;
  }

  setStatusLed(false);
  Serial.println("Rendered MELOS.WAV.");
  return true;
}

void startPlayback() {
  stopPlayback();
  if (!SD.exists(MELOS_FILE)) {
    Serial.println("MELOS.WAV missing; render with r first.");
    return;
  }

  melosFile = SD.sdfs.open(MELOS_FILE, O_RDONLY);
  if (!melosFile) {
    Serial.println("Could not open MELOS.WAV.");
    return;
  }
  if (melosFile.size() <= 44 || !melosFile.seekSet(44)) {
    Serial.println("MELOS.WAV empty/corrupt.");
    stopPlayback();
    return;
  }

  playing = true;
  queuedFreshAudio = false;
  setStatusLed(true);
  Serial.println("MELOS playback started on right channel.");
}

void feedQueue() {
  if (!playing) {
    return;
  }

  while (melosQueue.available()) {
    int16_t *buffer = melosQueue.getBuffer();
    if (!buffer) {
      return;
    }

    bool wrapped = false;
    for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
      uint8_t bytes[2];
      if (melosFile.read(bytes, 2) != 2) {
        wrapped = true;
        if (!melosFile.seekSet(44) || melosFile.read(bytes, 2) != 2) {
          buffer[i] = 0;
          playing = false;
          continue;
        }
      }
      const int16_t sample = static_cast<int16_t>((bytes[1] << 8) | bytes[0]);
      buffer[i] = sample;
    }
    melosQueue.playBuffer();
    if (!queuedFreshAudio) {
      queuedFreshAudio = true;
    }
    if (wrapped) {
      Serial.println("MELOS loop wrapped to start.");
    }
  }
}

void handleSerial() {
  while (Serial.available() > 0) {
    const char command = Serial.read();
    if (command == 'r' || command == 'R') {
      renderMelosWav();
    } else if (command == 'p' || command == 'P') {
      startPlayback();
    } else if (command == 's' || command == 'S') {
      stopPlayback();
    } else if (command == 'm' || command == 'M') {
      printModel();
    }
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  if (USE_EXTERNAL_STATUS_LED) {
    pinMode(STATUS_LED_PIN, OUTPUT);
  }
  setStatusLed(false);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho MELOS short render/playback test");

  AudioMemory(AUDIO_MEMORY_BLOCKS);
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
  Serial.println("SD init PASS.");

  rngState ^= micros();
  rngState ^= static_cast<uint32_t>(analogRead(A0)) << 16;
  loadModel();
  printModel();

  if (!SD.exists(MELOS_FILE)) {
    Serial.println("MELOS.WAV missing; rendering default short test file.");
    renderMelosWav();
  }

  Serial.println("Serial: r=render, p=play/restart, s=stop, m=model.");
  startPlayback();
}

void loop() {
  handleSerial();
  feedQueue();
}
