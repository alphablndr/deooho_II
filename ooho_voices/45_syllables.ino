// ---------------- Syllables: cut a take into syllables + descriptors ----------------
// Port of lab/logos_lab.py (find_syllables, background, air_pads,
// pitch_and_voicing). Runs on the take in the record buffer while the cube is
// PROCESSING; results go to SYLLS.CSV and the PSRAM index.

// Fills syllBandEnv (speech band 250-3800 Hz, 30 ms windows, 3-point smoothed)
// and syllFullEnv (full band, 30 ms windows). Returns the window count.
uint32_t computeSyllableEnvelopes(const int16_t *x, uint32_t total) {
  MasterBiquad hp, lp;
  hp.setHighpass(250.0f, 0.707f);
  lp.setLowpass(3800.0f, 0.707f);
  uint32_t hops = total / SYLL_HOP;
  if (hops > SYLL_MAX_HOPS) {
    hops = SYLL_MAX_HOPS;
  }
  for (uint32_t h = 0; h < hops; ++h) {  // per-hop energies first
    float bandE = 0.0f;
    float fullE = 0.0f;
    const int16_t *p = x + h * SYLL_HOP;
    for (uint32_t j = 0; j < SYLL_HOP; ++j) {
      const float s = p[j] * (1.0f / 32768.0f);
      const float f = lp.process(hp.process(s));
      bandE += f * f;
      fullE += s * s;
    }
    syllBandEnv[h] = bandE;
    syllFullEnv[h] = fullE;
    if ((h & 0x3FUL) == 0) {
      workTick();
    }
  }
  if (hops < 3) {
    return 0;
  }
  const uint32_t n = hops - 2;  // a 30 ms window spans three hops
  const float norm = 1.0f / (3.0f * SYLL_HOP);
  for (uint32_t i = 0; i < n; ++i) {  // in place is safe: i+1, i+2 are still raw
    syllScratch[i] = sqrtf((syllBandEnv[i] + syllBandEnv[i + 1] + syllBandEnv[i + 2]) * norm);
    syllFullEnv[i] = sqrtf((syllFullEnv[i] + syllFullEnv[i + 1] + syllFullEnv[i + 2]) * norm);
  }
  for (uint32_t i = 0; i < n; ++i) {  // like numpy convolve(ones(3)/3, 'same')
    const float before = i > 0 ? syllScratch[i - 1] : 0.0f;
    const float after = i + 1 < n ? syllScratch[i + 1] : 0.0f;
    syllBandEnv[i] = (before + syllScratch[i] + after) / 3.0f;
  }
  return n;
}

// rankDivisor 10 -> 10th percentile. Uses syllScratch.
float percentileOf(const float *values, uint32_t n, uint32_t rankDivisor) {
  if (n == 0) {
    return 0.0f;
  }
  for (uint32_t i = 0; i < n; ++i) {
    syllScratch[i] = values[i];
  }
  std::nth_element(syllScratch, syllScratch + n / rankDivisor, syllScratch + n);
  return syllScratch[n / rankDivisor];
}

// Median F0 of voiced 40 ms frames (normalized autocorrelation at 11025 Hz)
// and the voiced share, as lab pitch_and_voicing().
void syllablePitch(const int16_t *x, uint32_t s, uint32_t e, float &f0, uint8_t &voicedPct) {
  static float dec[SYLL_MAX_SAMPLES / 4 + 8];
  static float frame[441];
  static float f0s[96];
  const uint32_t m = (e - s) / 4;
  for (uint32_t i = 0; i < m; ++i) {
    const int16_t *p = x + s + i * 4;
    dec[i] = (p[0] + p[1] + p[2] + p[3]) * (1.0f / 131072.0f);
  }
  const uint32_t frameLen = 441, hop = 110, lo = 27, hi = 157;  // 70-400 Hz at 11025 Hz
  uint32_t frames = 0, voiced = 0;
  for (uint32_t st = 0; st + frameLen < m && voiced < 96; st += hop) {
    ++frames;
    float ac0 = 0.0f;
    for (uint32_t j = 0; j < frameLen; ++j) {
      const float w = 0.5f - 0.5f * cosf(2.0f * PI * j / (frameLen - 1));
      frame[j] = dec[st + j] * w;
      ac0 += frame[j] * frame[j];
    }
    if (ac0 <= 0.0f) {
      continue;
    }
    float best = -1.0f;
    uint32_t bestLag = lo;
    for (uint32_t lag = lo; lag < hi; ++lag) {
      float acc = 0.0f;
      for (uint32_t j = 0; j + lag < frameLen; ++j) {
        acc += frame[j] * frame[j + lag];
      }
      if (acc > best) {
        best = acc;
        bestLag = lag;
      }
    }
    if (bestLag > lo && bestLag < hi - 1 && best / ac0 > 0.5f) {  // a peak at the edge is not a pitch
      f0s[voiced++] = 11025.0f / bestLag;
    }
  }
  voicedPct = frames > 0 ? static_cast<uint8_t>(100UL * voiced / frames) : 0;
  if (voiced >= 2) {
    std::nth_element(f0s, f0s + voiced / 2, f0s + voiced);
    f0 = f0s[voiced / 2];
  } else {
    f0 = 0.0f;
  }
}

bool appendSyllLine(FsFile &file, const SyllEntry &e) {
  char line[96];
  const int len = snprintf(line, sizeof(line), "%u,%u,%lu,%lu,%u,%u,%u,%u,%lu,%lu,%lu,%lu\n",
                           e.file, e.idx, static_cast<unsigned long>(e.start), static_cast<unsigned long>(e.length),
                           e.pre, e.post, e.gapMs, e.voiced,
                           static_cast<unsigned long>(e.rms * 1000000.0f), static_cast<unsigned long>(e.f0 * 10.0f),
                           static_cast<unsigned long>(e.bright), static_cast<unsigned long>(e.snr * 100.0f));
  return len > 0 && file.write(line, len) == len;
}

bool addSyllToIndex(const SyllEntry &e) {
  if (syllCount >= SYLL_INDEX_CAPACITY) {
    return false;
  }
  syllIndex[syllCount++] = e;
  return true;
}

void clearSyllIndex() {
  syllCount = 0;
}

// Cuts the take in the record buffer into syllables, appends them to
// SYLLS.CSV and the index. Returns how many were added.
uint32_t appendSyllables(uint16_t fileNumber) {
  const int16_t *x = reinterpret_cast<const int16_t *>(activeRecordBuffer);
  const uint32_t total = recordBytesUsed / 2;
  syllLastTakeFirst = syllCount;
  syllLastTakeCount = 0;
  const uint32_t n = computeSyllableEnvelopes(x, total);
  if (n < 5) {
    return 0;
  }
  const float bandFloor = percentileOf(syllBandEnv, n, 10) + 1e-9f;
  const float fullFloor = percentileOf(syllFullEnv, n, 10) + 1e-7f;

  static uint16_t peaks[SYLL_MAX_HOPS / 2];
  static uint16_t merged[SYLL_MAX_HOPS / 2];
  uint32_t np = 0, nm = 0;
  for (uint32_t i = 1; i + 1 < n; ++i) {  // nuclei: louder than both neighbours and the hiss
    if (syllBandEnv[i] >= syllBandEnv[i - 1] && syllBandEnv[i] > syllBandEnv[i + 1] &&
        syllBandEnv[i] > bandFloor * SYLL_PEAK_OVER_FLOOR) {
      if (np > 0 && i - peaks[np - 1] < SYLL_MIN_PEAK_GAP_HOPS) {
        if (syllBandEnv[i] > syllBandEnv[peaks[np - 1]]) {
          peaks[np - 1] = i;
        }
      } else {
        peaks[np++] = i;
      }
    }
  }
  for (uint32_t k = 0; k < np; ++k) {  // neighbours without a real dip are one syllable
    const uint32_t p = peaks[k];
    if (nm > 0) {
      const uint32_t q = merged[nm - 1];
      float dip = syllBandEnv[q];
      for (uint32_t i = q; i <= p; ++i) {
        dip = min(dip, syllBandEnv[i]);
      }
      if (dip > SYLL_DIP_RATIO * min(syllBandEnv[q], syllBandEnv[p])) {
        if (syllBandEnv[p] > syllBandEnv[q]) {
          merged[nm - 1] = p;
        }
        continue;
      }
    }
    merged[nm++] = p;
  }

  FsFile file = SD.sdfs.open(SYLLS_FILE, O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    Serial.println("Could not open SYLLS.CSV.");
    return 0;
  }
  const float edge = bandFloor * SYLL_EDGE_OVER_FLOOR;
  uint32_t prevEnd = 0;
  bool havePrev = false;
  uint32_t added = 0;
  for (uint32_t k = 0; k < nm && added < SYLL_PER_TAKE_MAX; ++k) {
    const uint32_t p = merged[k];
    const uint32_t leftLim = k > 0 ? merged[k - 1] : 0;
    const uint32_t rightLim = k + 1 < nm ? merged[k + 1] : n - 1;
    uint32_t a = p;
    if (k > 0) {
      for (uint32_t i = leftLim; i <= p; ++i) {
        if (syllBandEnv[i] < syllBandEnv[a]) {
          a = i;
        }
      }
    }
    if (k == 0 || syllBandEnv[a] < edge) {
      a = p;
      while (a > leftLim && syllBandEnv[a] > edge) {
        --a;
      }
    }
    uint32_t b = p;
    if (k + 1 < nm) {
      for (uint32_t i = p; i <= rightLim; ++i) {
        if (syllBandEnv[i] < syllBandEnv[b]) {
          b = i;
        }
      }
    }
    if (k + 1 == nm || syllBandEnv[b] < edge) {
      b = p;
      while (b < rightLim && syllBandEnv[b] > edge) {
        ++b;
      }
    }
    uint32_t s = a * SYLL_HOP;
    uint32_t e = min(b * SYLL_HOP + 3 * SYLL_HOP, total);
    if (e <= s || e - s < SYLL_MIN_SAMPLES) {
      continue;
    }
    if (e - s > SYLL_MAX_SAMPLES) {  // very long nucleus: keep a centred slice
      const uint32_t c = p * SYLL_HOP;
      s = c > SYLL_MAX_SAMPLES / 2 ? c - SYLL_MAX_SAMPLES / 2 : 0;
      e = min(c + SYLL_MAX_SAMPLES / 2, total);
    }

    SyllEntry entry;
    entry.file = fileNumber;
    entry.idx = static_cast<uint16_t>(added);
    entry.start = s;
    entry.length = e - s;
    double sumSq = 0.0, sumDiff = 0.0;
    for (uint32_t i = s; i < e; ++i) {
      const float v = x[i] * (1.0f / 32768.0f);
      sumSq += static_cast<double>(v) * v;
      if (i > s) {
        const float d = v - x[i - 1] * (1.0f / 32768.0f);
        sumDiff += static_cast<double>(d) * d;
      }
    }
    entry.rms = sqrtf(sumSq / (e - s));
    entry.bright = sumSq > 0.0 ? (SAMPLE_RATE / (2.0f * PI)) * sqrtf(sumDiff / sumSq) : 0.0f;
    entry.snr = entry.rms / fullFloor;
    syllablePitch(x, s, e, entry.f0, entry.voiced);
    // own air: quiet edge of this take, while it stays below 1.6 x its background
    uint32_t pre = 0, post = 0;
    const uint32_t k0 = s / SYLL_HOP, k1 = e / SYLL_HOP;
    while (pre < SYLL_AIR_MAX_HOPS && k0 >= pre + 1 && syllFullEnv[k0 - pre - 1] < fullFloor * SYLL_AIR_QUIET) {
      ++pre;
    }
    while (post < SYLL_AIR_MAX_HOPS && k1 + post + 1 < n && syllFullEnv[k1 + post + 1] < fullFloor * SYLL_AIR_QUIET) {
      ++post;
    }
    entry.pre = static_cast<uint16_t>(min(pre * SYLL_HOP, s));
    entry.post = static_cast<uint16_t>(min(post * SYLL_HOP, total - e));
    entry.gapMs = havePrev && s > prevEnd ? static_cast<uint16_t>(min((s - prevEnd) * 1000UL / SAMPLE_RATE, 60000UL)) : 0xFFFF;
    prevEnd = e;
    havePrev = true;
    if (!appendSyllLine(file, entry)) {
      break;
    }
    addSyllToIndex(entry);
    ++added;
    workTick();
  }
  file.sync();
  file.close();
  syllLastTakeCount = added;
  Serial.print("Syllables added: ");
  Serial.print(added);
  Serial.print(" (index ");
  Serial.print(syllCount);
  Serial.println(")");
  return added;
}

void loadSyllIndex() {
  clearSyllIndex();
  FsFile file = SD.sdfs.open(SYLLS_FILE, O_RDONLY);
  if (!file) {
    return;
  }
  char line[96];
  while (file.available()) {
    if (!readCsvLine(file, line, sizeof(line))) {
      continue;
    }
    unsigned long v[12];
    uint8_t got = 0;
    for (char *tok = strtok(line, ","); tok && got < 12; tok = strtok(NULL, ",")) {
      v[got++] = strtoul(tok, NULL, 10);
    }
    if (got != 12) {
      continue;
    }
    SyllEntry e;
    e.file = v[0];
    e.idx = v[1];
    e.start = v[2];
    e.length = v[3];
    e.pre = v[4];
    e.post = v[5];
    e.gapMs = v[6];
    e.voiced = v[7];
    e.rms = v[8] / 1000000.0f;
    e.f0 = v[9] / 10.0f;
    e.bright = v[10];
    e.snr = v[11] / 100.0f;
    if (!addSyllToIndex(e)) {
      break;
    }
    if ((syllCount & 0xFFUL) == 0) {
      kickWatchdog();
    }
  }
  file.close();
}
