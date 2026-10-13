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

