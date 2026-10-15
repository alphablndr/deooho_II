float logosWindowRms[LOGOS_MAX_WINDOWS];
float logosWindowScratch[LOGOS_MAX_WINDOWS];  // sorted copy for the hiss level

// Snap a boundary to the nearest zero crossing within a small radius.
uint32_t snapToZeroCrossing(const int16_t *samples, uint32_t total, uint32_t pos, uint32_t radius) {
  if (total == 0) {
    return 0;
  }
  if (pos >= total) {
    pos = total - 1;
  }
  const uint32_t lo = pos > radius ? pos - radius : 1;
  const uint32_t hi = pos + radius < total ? pos + radius : total - 1;
  uint32_t best = pos;
  uint32_t bestDist = radius + 1;
  for (uint32_t i = lo; i <= hi; ++i) {
    const bool crossing = (samples[i - 1] <= 0 && samples[i] > 0) ||
                          (samples[i - 1] >= 0 && samples[i] < 0);
    if (crossing) {
      const uint32_t d = i > pos ? i - pos : pos - i;
      if (d < bestDist) {
        bestDist = d;
        best = i;
      }
    }
  }
  return best;
}

// Emit grains for one voiced region. Long regions are split at the quietest
// window near the target length (a syllable/word seam), not on a fixed grid;
// every boundary is snapped to a zero crossing.
uint32_t emitRegionGrains(const int16_t *samples, uint32_t total, uint32_t winStart, uint32_t winEnd,
                          uint32_t s0, uint32_t s1) {
  if (s1 <= s0) {
    return 0;
  }
  s0 = snapToZeroCrossing(samples, total, s0, LOGOS_ZC_SEARCH_SAMPLES);
  s1 = snapToZeroCrossing(samples, total, s1, LOGOS_ZC_SEARCH_SAMPLES);
  if (s1 <= s0) {
    return 0;
  }

  const uint32_t samplesPerMs = SAMPLE_RATE / 1000UL;
  const uint32_t minSamples = LOGOS_MIN_DURATION_MS * samplesPerMs;
  const uint32_t maxSamples = LOGOS_MAX_DURATION_MS * samplesPerMs;
  const uint32_t splitSamples = LOGOS_SPLIT_DURATION_MS * samplesPerMs;
  const uint32_t searchWindows = (LOGOS_SPLIT_SEARCH_MS * samplesPerMs) / LOGOS_WINDOW_SAMPLES;

  if (s1 - s0 < minSamples) {
    return 0;
  }
  if (s1 - s0 <= maxSamples) {
    return appendGrain(currentFilename, s0 * 1000UL / SAMPLE_RATE, (s1 - s0) * 1000UL / SAMPLE_RATE) ? 1 : 0;
  }

  uint32_t added = 0;
  uint32_t cursor = s0;
  while (s1 - cursor > maxSamples) {
    const uint32_t targetSample = cursor + splitSamples;
    uint32_t targetWin = targetSample / LOGOS_WINDOW_SAMPLES;
    uint32_t lo = targetWin > searchWindows ? targetWin - searchWindows : winStart;
    if (lo < winStart + 1) {
      lo = winStart + 1;
    }
    uint32_t hi = targetWin + searchWindows < winEnd ? targetWin + searchWindows : winEnd - 1;
    uint32_t bestWin = targetWin;
    float bestRms = 1.0e9f;
    for (uint32_t ww = lo; ww <= hi; ++ww) {
      if (logosWindowRms[ww] < bestRms) {
        bestRms = logosWindowRms[ww];
        bestWin = ww;
      }
    }
    uint32_t cut = snapToZeroCrossing(samples, total, bestWin * LOGOS_WINDOW_SAMPLES, LOGOS_ZC_SEARCH_SAMPLES);
    if (cut < cursor + minSamples) {
      cut = cursor + splitSamples;  // safety: keep making progress
    }
    if (cut > s1) {
      cut = s1;
    }
    const uint32_t segMs = (cut - cursor) * 1000UL / SAMPLE_RATE;
    if (segMs >= LOGOS_MIN_DURATION_MS && appendGrain(currentFilename, cursor * 1000UL / SAMPLE_RATE, segMs)) {
      added++;
    }
    cursor = cut;
    workTick();
  }
  const uint32_t tailMs = (s1 - cursor) * 1000UL / SAMPLE_RATE;
  if (tailMs >= LOGOS_MIN_DURATION_MS && appendGrain(currentFilename, cursor * 1000UL / SAMPLE_RATE, tailMs)) {
    added++;
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

  // 1. Per-window RMS (stored so we can extend edges and find quiet split points).
  uint32_t windowCount = totalSamples / LOGOS_WINDOW_SAMPLES;
  if (windowCount > LOGOS_MAX_WINDOWS) {
    windowCount = LOGOS_MAX_WINDOWS;
  }
  float maxWindowRms = 0.0f;
  double totalEnergy = 0.0;
  for (uint32_t w = 0; w < windowCount; ++w) {
    const uint32_t base = w * LOGOS_WINDOW_SAMPLES;
    double sumSquares = 0.0;
    for (uint32_t i = 0; i < LOGOS_WINDOW_SAMPLES; ++i) {
      const float s = samples[base + i] / 32768.0f;
      sumSquares += static_cast<double>(s) * s;
    }
    const float rms = sqrtf(sumSquares / LOGOS_WINDOW_SAMPLES);
    logosWindowRms[w] = rms;
    if (rms > maxWindowRms) {
      maxWindowRms = rms;
    }
    totalEnergy += sumSquares;
    if ((w & 0x3FUL) == 0) {
      workTick();
    }
  }

  float startThreshold = constrain(maxWindowRms * LOGOS_WINDOW_PEAK_FACTOR, LOGOS_MIN_RMS_THRESHOLD, LOGOS_MAX_RMS_THRESHOLD);
  float continueThreshold = startThreshold * LOGOS_HYST_FACTOR;
  // The continue threshold must stay above the take's own hiss, or a run never
  // ends in a pause and a quiet whisper becomes one grain that is mostly hiss
  // (2026-10-14: 64-85 % hiss in whisper grains; Anton preferred the fix by
  // ear). The 10th-percentile window is this take's hiss level.
  for (uint32_t w = 0; w < windowCount; ++w) {
    logosWindowScratch[w] = logosWindowRms[w];
  }
  std::nth_element(logosWindowScratch, logosWindowScratch + windowCount / 10, logosWindowScratch + windowCount);
  const float hissLevel = logosWindowScratch[windowCount / 10];
  if (continueThreshold < hissLevel * LOGOS_HISS_CONTINUE_FACTOR) {
    continueThreshold = hissLevel * LOGOS_HISS_CONTINUE_FACTOR;
  }
  if (startThreshold < continueThreshold * 1.05f) {
    startThreshold = continueThreshold * 1.05f;
  }

  const float overallRms = sqrtf(totalEnergy / (static_cast<double>(windowCount) * LOGOS_WINDOW_SAMPLES));
  Serial.print("LOGOS analysis: duration_ms=");
  Serial.print(totalSamples * 1000UL / SAMPLE_RATE);
  Serial.print(", rms=");
  Serial.print(overallRms, 5);
  Serial.print(", max_window_rms=");
  Serial.print(maxWindowRms, 5);
  Serial.print(", startThr=");
  Serial.print(startThreshold, 5);
  Serial.print(", contThr=");
  Serial.print(continueThreshold, 5);
  Serial.print(", hiss=");
  Serial.print(hissLevel, 5);
  Serial.print(", windows=");
  Serial.println(windowCount);

  // 2. Hysteresis regions: a run stays alive above continueThreshold (so soft
  // consonants/onsets/tails are kept), but is only emitted if it contains a
  // voiced core above startThreshold.
  uint32_t gapWindows = (LOGOS_GAP_TOLERANCE_MS * (SAMPLE_RATE / 1000UL)) / LOGOS_WINDOW_SAMPLES;
  if (gapWindows < 1) {
    gapWindows = 1;
  }
  uint32_t added = 0;
  uint32_t w = 0;
  while (w < windowCount) {
    if (logosWindowRms[w] <= continueThreshold) {
      ++w;
      continue;
    }
    const uint32_t runStart = w;
    uint32_t lastAbove = w;
    bool hasCore = false;
    uint32_t gap = 0;
    while (w < windowCount) {
      if (logosWindowRms[w] > continueThreshold) {
        if (logosWindowRms[w] > startThreshold) {
          hasCore = true;
        }
        lastAbove = w;
        gap = 0;
      } else {
        ++gap;
        if (gap > gapWindows) {
          break;
        }
      }
      ++w;
    }
    if (hasCore) {
      const uint32_t runEnd = lastAbove + 1;  // exclusive window
      uint32_t s0 = runStart * LOGOS_WINDOW_SAMPLES;
      uint32_t s1 = runEnd * LOGOS_WINDOW_SAMPLES;
      if (s1 > totalSamples) {
        s1 = totalSamples;
      }
      added += emitRegionGrains(samples, totalSamples, runStart, runEnd, s0, s1);
    }
    workTick();
  }

  grainCount = grainIndexCount;
  Serial.print("LOGOS grains added: ");
  Serial.println(added);
}

int compareFloats(const void *a, const void *b) {
  const float fa = *static_cast<const float *>(a);
  const float fb = *static_cast<const float *>(b);
  return (fa > fb) - (fa < fb);
}

// Median pitch of a take, or 0 when it has none (whisper, silence, noise).
// Checked against a YIN reference on 14 bench takes (2026-10-15): the old
// version gave notes to whispers, silence and a covered mic (mostly 355.6 Hz,
// the edge of the lag range). Now: decimation averages 4 samples (no hiss
// aliasing), a frame must be 3x louder than the take's own background, the
// match must be confident (0.5) and not at the range edge, and a take needs
// at least 5 such frames. voicedRatio keeps its old meaning (loud frames).
float estimateMedianF0(float &voicedRatio) {
  static float frame[F0_FRAME_SAMPLES];
  static float frameRms[MAX_F0_FRAMES];
  static float rmsSorted[MAX_F0_FRAMES];
  const int16_t *source = reinterpret_cast<const int16_t *>(activeRecordBuffer);
  const uint32_t decimatedSamples = (recordBytesUsed / 2) / 4;
  voicedRatio = 0.0f;
  if (decimatedSamples < F0_FRAME_SAMPLES) {
    return 0.0f;
  }

  auto loadFrame = [&](uint32_t frameStart) {
    for (uint16_t i = 0; i < F0_FRAME_SAMPLES; ++i) {
      const int16_t *p = source + (frameStart + i) * 4;
      frame[i] = (p[0] + p[1] + p[2] + p[3]) * (1.0f / 131072.0f);
    }
  };

  // Pass 1: loudness of every frame -> this take's background.
  uint32_t frames = 0, loudFrames = 0;
  for (uint32_t st = 0; st + F0_FRAME_SAMPLES <= decimatedSamples && frames < MAX_F0_FRAMES; st += F0_HOP_SAMPLES) {
    loadFrame(st);
    float energy = 0.0f;
    for (uint16_t i = 0; i < F0_FRAME_SAMPLES; ++i) {
      energy += frame[i] * frame[i];
    }
    frameRms[frames] = sqrtf(energy / F0_FRAME_SAMPLES);
    rmsSorted[frames] = frameRms[frames];
    loudFrames += frameRms[frames] >= F0_RMS_THRESHOLD ? 1 : 0;
    ++frames;
    if ((frames & 0x1FUL) == 0) {
      workTick();
    }
  }
  voicedRatio = frames > 0 ? static_cast<float>(loudFrames) / frames : 0.0f;
  std::nth_element(rmsSorted, rmsSorted + frames / 10, rmsSorted + frames);
  const float gate = max(rmsSorted[frames / 10] * F0_GATE_OVER_BACKGROUND, F0_GATE_MIN);

  // Pass 2: pitch of the frames clearly above the background.
  float acceptedF0[MAX_F0_VALUES];
  uint16_t acceptedCount = 0;
  for (uint32_t f = 0; f < frames && acceptedCount < MAX_F0_VALUES; ++f) {
    if (frameRms[f] < gate) {
      continue;
    }
    loadFrame(f * F0_HOP_SAMPLES);
    float bestCorr = 0.0f;
    uint16_t bestLag = F0_MIN_LAG;
    for (uint16_t lag = F0_MIN_LAG; lag <= F0_MAX_LAG; ++lag) {
      float cross = 0.0f, e1 = 0.0f, e2 = 0.0f;
      const uint16_t count = F0_FRAME_SAMPLES - lag;
      for (uint16_t i = 0; i < count; ++i) {
        const float a = frame[i];
        const float b = frame[i + lag];
        cross += a * b;
        e1 += a * a;
        e2 += b * b;
      }
      const float corr = cross / (sqrtf(e1 * e2) + 1.0e-9f);
      if (corr > bestCorr) {
        bestCorr = corr;
        bestLag = lag;
      }
    }
    if (bestCorr > F0_CONFIDENCE_THRESHOLD && bestLag > F0_MIN_LAG && bestLag < F0_MAX_LAG) {
      acceptedF0[acceptedCount++] = static_cast<float>(ANALYSIS_RATE) / bestLag;
    }
    workTick();
  }
  if (acceptedCount < F0_MIN_VOICED_FRAMES) {
    return 0.0f;
  }
  qsort(acceptedF0, acceptedCount, sizeof(float), compareFloats);
  return acceptedF0[acceptedCount / 2];
}

void analyzeEnvelopeAndRms(float envelopeOut[], float &rmsOut) {
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

void analyzeBands(float bandsOut[]) {
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
  appendMelosPitch(medianF0);   // this person's note joins the MELOS choir
  melosClusterEnter(medianF0, bandsVowelPosition(incomingBands));
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

bool loadWavIntoRecordBuffer(const char *filename) {
  FsFile file = SD.sdfs.open(filename, O_RDONLY);
  if (!file) {
    return false;
  }
  const uint32_t fileBytes = file.size();
  if (fileBytes <= 44) {
    file.close();
    return false;
  }
  uint32_t dataBytes = fileBytes - 44;
  if (dataBytes > activeRecordCapacity) {
    dataBytes = activeRecordCapacity;
  }
  if (!file.seekSet(44)) {
    file.close();
    return false;
  }
  uint32_t readTotal = 0;
  while (readTotal < dataBytes) {
    const uint32_t chunk = min(static_cast<uint32_t>(SD_WRITE_CHUNK_BYTES), dataBytes - readTotal);
    const int got = file.read(activeRecordBuffer + readTotal, chunk);
    if (got <= 0) {
      break;
    }
    readTotal += static_cast<uint32_t>(got);
    workTick();
  }
  file.close();
  recordBytesUsed = readTotal;
  return readTotal > 0;
}

// One-off: re-slice every archived Wxxxx.WAV with the current grain detector and
// rewrite GRAINS.CSV from scratch. Triggered by serial 'x'.
void rebuildGrainsFromArchive() {
  if (state != IDLE) {
    Serial.println("Rebuild ignored (busy).");
    return;
  }
  Serial.println("Rebuilding GRAINS.CSV from archive...");
  stopAllPlayback();
  enterState(PROCESSING);
  if (SD.exists(GRAINS_FILE)) {
    SD.remove(GRAINS_FILE);
  }
  clearGrainIndex();  // appendGrain() refills it file by file
  if (SD.exists(PITCHES_FILE)) {
    SD.remove(PITCHES_FILE);
  }
  clearMelosPitches();
  grainCount = 0;

  uint32_t filesDone = 0;
  uint32_t misses = 0;
  char name[13];
  for (uint16_t i = 1; i < 10000 && misses < 60; ++i) {
    makeFilename(name, i);
    if (!SD.exists(name)) {
      misses++;
      continue;
    }
    misses = 0;
    if (!loadWavIntoRecordBuffer(name)) {
      Serial.print("Skip ");
      Serial.println(name);
      continue;
    }
    strncpy(currentFilename, name, sizeof(currentFilename));
    currentFilename[sizeof(currentFilename) - 1] = '\0';
    appendLogosGrains();  // appends grains for this file to GRAINS.CSV
    float voicedRatio = 0.0f;
    const float pitch = estimateMedianF0(voicedRatio);
    appendMelosPitch(pitch);  // and its pitch to the MELOS choir
    Serial.print(name);
    Serial.print(" pitch ");
    Serial.println(pitch, 1);
    filesDone++;
    workTick();
  }

  grainCount = countGrains();
  Serial.print("Rebuild done: files=");
  Serial.print(filesDone);
  Serial.print(", grains=");
  Serial.println(grainCount);
  initializeLogosSchedulers();
  enterState(IDLE);
}

// Log-sweep tone (60 Hz -> 8 kHz, 30 s) on the right channel only, so you can
