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

uint8_t logosArchivePressure() {
  if (grainCount == 0) {
    return 0;
  }
  if (grainCount >= LOGOS_ARCHIVE_FULL_GRAINS) {
    return 100;
  }
  return static_cast<uint8_t>((grainCount * 100UL) / LOGOS_ARCHIVE_FULL_GRAINS);
}

uint32_t archiveScaledInterval(uint32_t minMs, uint32_t maxMs, uint8_t maxReductionPercent) {
  const uint32_t reduction = (static_cast<uint32_t>(logosArchivePressure()) * maxReductionPercent) / 100UL;
  const uint32_t scalePercent = 100UL - reduction;
  uint32_t scaledMin = (minMs * scalePercent) / 100UL;
  uint32_t scaledMax = (maxMs * scalePercent) / 100UL;
  if (scaledMin < 220UL) {
    scaledMin = 220UL;
  }
  if (scaledMax < scaledMin + 20UL) {
    scaledMax = scaledMin + 20UL;
  }
  return randomRangeMs(scaledMin, scaledMax);
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
  const uint8_t pressure = logosArchivePressure();
  const uint8_t sparseChance = 26 > (pressure * 18UL) / 100UL ? 26 - (pressure * 18UL) / 100UL : 8;
  const uint8_t swarmChance = 28 + (pressure * 34UL) / 100UL;
  const uint8_t presentEnd = 100 - swarmChance;
  const uint8_t r = nextRandomU32() % 100UL;
  if (r < sparseChance) {
    logosDensityMode = LOGOS_MODE_SPARSE;
  } else if (r < presentEnd) {
    logosDensityMode = LOGOS_MODE_PRESENT;
  } else {
    logosDensityMode = LOGOS_MODE_SWARM;
  }
  logosDensitySwitchMs = millis() + randomRangeMs(LOGOS_DENSITY_MIN_MS, LOGOS_DENSITY_MAX_MS);
  Serial.print("LOGOS density mode: ");
  Serial.print(logosDensityModeName());
  Serial.print(" archivePressure=");
  Serial.print(pressure);
  Serial.println("%");
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
  static const float wordRates[] = {0.6667f, 0.75f, 1.0f, 1.0f, 1.3333f, 1.50f};
  static const float bodyRates[] = {0.50f, 0.6667f, 0.75f, 1.0f, 1.3333f, 1.50f};
  static const float syllableRates[] = {0.50f, 0.6667f, 0.75f, 1.0f, 1.3333f, 1.50f, 2.0f};
  static const float sparkRates[] = {0.6667f, 0.75f, 1.0f, 1.3333f, 1.50f, 2.0f};
  const float *choices = wordRates;
  uint8_t count = sizeof(wordRates) / sizeof(wordRates[0]);
  if (index == 1) {
    choices = bodyRates;
    count = sizeof(bodyRates) / sizeof(bodyRates[0]);
  } else if (index == 2) {
    choices = syllableRates;
    count = sizeof(syllableRates) / sizeof(syllableRates[0]);
  } else if (index == 3) {
    choices = sparkRates;
    count = sizeof(sparkRates) / sizeof(sparkRates[0]);
  }
  return choices[nextRandomU32() % count];
}

float logosArpRate(uint8_t step) {
  static const float arpRates[] = {0.50f, 0.6667f, 0.75f, 1.0f, 1.3333f, 1.50f, 2.0f};
  return arpRates[step % (sizeof(arpRates) / sizeof(arpRates[0]))];
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
      stream.nextEventMs = millis() + archiveScaledInterval(520, 1900, LOGOS_ARCHIVE_SWARM_INTERVAL_REDUCTION);
    } else if (logosDensityMode == LOGOS_MODE_SPARSE) {
      stream.nextEventMs = millis() + archiveScaledInterval(4200, 11500, LOGOS_ARCHIVE_INTERVAL_REDUCTION / 2);
    } else {
      stream.nextEventMs = millis() + archiveScaledInterval(LOGOS_STREAM_1_MIN_INTERVAL_MS, LOGOS_STREAM_1_MAX_INTERVAL_MS,
                                                            LOGOS_ARCHIVE_INTERVAL_REDUCTION);
    }
  } else if (stream.activeMode || logosDensityMode == LOGOS_MODE_SWARM) {
    if (logosDensityMode == LOGOS_MODE_SWARM) {
      stream.nextEventMs = millis() + archiveScaledInterval(280, 1700, LOGOS_ARCHIVE_SWARM_INTERVAL_REDUCTION);
    } else if (logosDensityMode == LOGOS_MODE_SPARSE) {
      stream.nextEventMs = millis() + archiveScaledInterval(2600, 8500, LOGOS_ARCHIVE_INTERVAL_REDUCTION / 2);
    } else {
      const uint32_t interval = musicalLogosIntervalMs(index);
      stream.nextEventMs = millis() + archiveScaledInterval(interval, interval + LOGOS_PULSE_JITTER_MS,
                                                            LOGOS_ARCHIVE_INTERVAL_REDUCTION);
    }
  } else {
    const uint32_t density = grainCount < LOGOS_ARCHIVE_FULL_GRAINS ? grainCount : LOGOS_ARCHIVE_FULL_GRAINS;
    const uint32_t shrink = (density * LOGOS_ARCHIVE_DORMANT_REDUCTION * 80UL) / 100UL;
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
  logosResponse.pending = false;
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

// Timed wrapper: the CSV scan blocks loop() while play queues drain, so `d`
// reports how long a grain choice takes.
bool selectRandomGrain(GrainInfo &grain) {
  const uint32_t t0 = micros();
  const bool found = selectRandomGrainFromCsv(grain);
  grainSelectLastUs = micros() - t0;
  if (grainSelectLastUs > grainSelectMaxUs) {
    grainSelectMaxUs = grainSelectLastUs;
  }
  return found;
}

bool selectRandomGrainFromCsv(GrainInfo &grain) {
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
  const float event = constrain((0.42f + 0.12f * static_cast<float>(index)) * MELOS_LOGOS_COUPLING, 0.0f, 1.0f);
  if (reversed || granular) {
    melosLogosConstrict += (reversed ? 0.28f : 0.18f) * MELOS_LOGOS_COUPLING;
  }
  if (!granular && rate > 0.92f && rate < 1.08f) {
    melosLogosStabilize += 0.26f * MELOS_LOGOS_COUPLING;
    melosConstrMidTarget += 0.10f * event;
    melosTongueDiameterTarget += 0.18f * event;
  } else if (rate < 0.80f) {
    melosLogosStabilize += 0.12f * MELOS_LOGOS_COUPLING;
    melosLogosConstrict += 0.10f * MELOS_LOGOS_COUPLING;
    melosConstrBackTarget += 0.20f * event;
    melosTongueTarget -= 0.10f * event;
    melosTongueDiameterTarget -= 0.12f * event;
  }
  if (granular) {
    melosConstrFrontTarget += 0.32f * event;
    melosConstrMidTarget += 0.22f * event;
    melosTongueTarget += 0.13f * event;
    melosTongueDiameterTarget -= 0.20f * event;
  }
  if (reversed) {
    melosConstrBackTarget += 0.38f * event;
    melosNasalTarget += 0.18f * event;
    melosTongueTarget -= 0.18f * event;
    melosTongueDiameterTarget -= 0.10f * event;
  }
  if (rate > 1.18f) {
    melosConstrFrontTarget += 0.18f * event;
    melosTongueTarget += 0.08f * event;
  }
  uint8_t speechChance = granular ? 48 : 22;
  if (reversed) {
    speechChance += 14;
  }
  if (rate < 0.80f || rate > 1.18f) {
    speechChance += 12;
  }
  if (activeLogosStreamCount() >= 3) {
    speechChance += 16;
  }
  if ((nextRandomU32() % 100UL) < speechChance) {
    const float strength = constrain(0.32f + event * (granular ? 0.43f : 0.30f), 0.0f, 0.86f);
    triggerMelosSpeechGhost(strength, randomRangeMs(1400, granular ? 5200UL : 3600UL));
  }
  uint8_t syllableChance = granular ? 62 : 34;
  if (reversed) {
    syllableChance += 16;
  }
  if (activeLogosStreamCount() >= 3) {
    syllableChance += 18;
  }
  if ((nextRandomU32() % 100UL) < syllableChance) {
    triggerMelosSyllableGesture(constrain(0.30f + event * 0.46f, 0.0f, 0.92f),
                                randomRangeMs(MELOS_SYLLABLE_MIN_MS, MELOS_SYLLABLE_MAX_MS));
  }

  melosLogosOpen = constrain(melosLogosOpen, 0.0f, 1.0f);
  melosLogosConstrict = constrain(melosLogosConstrict, 0.0f, 1.0f);
  melosLogosStabilize = constrain(melosLogosStabilize, 0.0f, 1.0f);
  melosTongueTarget = constrain(melosTongueTarget, 0.0f, 1.0f);
  melosTongueDiameterTarget = constrain(melosTongueDiameterTarget, 0.16f, 0.98f);
  melosConstrBackTarget = constrain(melosConstrBackTarget, 0.0f, 1.0f);
  melosConstrMidTarget = constrain(melosConstrMidTarget, 0.0f, 1.0f);
  melosConstrFrontTarget = constrain(melosConstrFrontTarget, 0.0f, 1.0f);
  melosNasalTarget = constrain(melosNasalTarget, 0.0f, 0.95f);
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
  uint8_t minVoices = 1;
  uint8_t maxVoices = 2;
  if (logosDensityMode == LOGOS_MODE_SWARM) {
    if (index == 0) {
      minVoices = 3;
      maxVoices = 6;
    } else {
      minVoices = 6;
      maxVoices = LOGOS_MAX_CLOUD_VOICES;
    }
  } else if (logosDensityMode == LOGOS_MODE_SPARSE) {
    minVoices = index == 0 ? 1 : 2;
    maxVoices = index == 0 ? 2 : 4;
  } else if (index == 0) {
    minVoices = 2;
    maxVoices = 4;
  } else if (index == 1) {
    minVoices = 3;
    maxVoices = 6;
  } else if (index == 2) {
    minVoices = 4;
    maxVoices = 7;
  } else {
    minVoices = 5;
    maxVoices = LOGOS_MAX_CLOUD_VOICES;
  }

  const uint8_t pressureBoost = (logosArchivePressure() * LOGOS_ARCHIVE_VOICE_BOOST_MAX) / 100UL;
  if (index > 0 && pressureBoost > 0) {
    minVoices = min(static_cast<uint8_t>(minVoices + pressureBoost), LOGOS_MAX_CLOUD_VOICES);
    maxVoices = min(static_cast<uint8_t>(maxVoices + pressureBoost), LOGOS_MAX_CLOUD_VOICES);
    if (maxVoices < minVoices) {
      maxVoices = minVoices;
    }
  }
  return static_cast<uint8_t>(randomRangeMs(minVoices, maxVoices));
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

void scheduleLogosResponse(uint8_t sourceIndex, const GrainInfo &grain) {
  if (grain.durationMs < LOGOS_MIN_DURATION_MS || (nextRandomU32() % 100UL) >= LOGOS_RESPONSE_CHANCE_PERCENT) {
    return;
  }
  logosResponse.pending = true;
  logosResponse.sourceStream = sourceIndex;
  logosResponse.grain = grain;
  logosResponse.dueMs = millis() + randomRangeMs(LOGOS_RESPONSE_MIN_DELAY_MS, LOGOS_RESPONSE_MAX_DELAY_MS);
}

void serviceLogosResponses() {
  if (!logosResponse.pending || state != IDLE || !playbackServiceAllowed()) {
    return;
  }
  if (static_cast<int32_t>(millis() - logosResponse.dueMs) < 0) {
    return;
  }
  uint8_t startIndex = 1 + (nextRandomU32() % (LOGOS_STREAM_COUNT - 1));
  for (uint8_t tries = 0; tries < LOGOS_STREAM_COUNT; ++tries) {
    const uint8_t index = (startIndex + tries) % LOGOS_STREAM_COUNT;
    if (index == logosResponse.sourceStream || logosStreams[index].playing) {
      continue;
    }
    const uint32_t maxOffset = logosResponse.grain.durationMs > LOGOS_RESPONSE_MIN_MS ?
                               logosResponse.grain.durationMs - LOGOS_RESPONSE_MIN_MS : 0;
    const uint32_t offset = maxOffset > 0 ? randomRangeMs(0, maxOffset) : 0;
    if (startLogosGrain(index, logosResponse.grain, offset, false, LOGOS_LAUNCH_RESPONSE)) {
      logosResponse.pending = false;
      return;
    }
  }
  logosResponse.dueMs = millis() + 350;
}

bool startLogosGrain(uint8_t index, const GrainInfo &grain, uint32_t extraOffsetMs, bool repeatEligible, uint8_t launchMode) {
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
  bool forceResponse = launchMode == LOGOS_LAUNCH_RESPONSE;
  bool forceTail = launchMode == LOGOS_LAUNCH_REVERSE_TAIL;
  bool forcePhrase = launchMode == LOGOS_LAUNCH_PHRASE_GHOST;
  if (forceResponse && playbackDurationMs > LOGOS_RESPONSE_MIN_MS) {
    playbackDurationMs = randomRangeMs(LOGOS_RESPONSE_MIN_MS, min(LOGOS_RESPONSE_MAX_MS, playbackDurationMs));
  } else if (forceTail && playbackDurationMs > LOGOS_REVERSE_TAIL_MIN_MS) {
    playbackDurationMs = min(randomRangeMs(LOGOS_REVERSE_TAIL_MIN_MS, min(LOGOS_REVERSE_TAIL_MAX_MS, playbackDurationMs)), playbackDurationMs);
  } else if (forcePhrase) {
    const uint32_t maxPreRoll = grain.startMs < LOGOS_PHRASE_PREROLL_MAX_MS ? grain.startMs : LOGOS_PHRASE_PREROLL_MAX_MS;
    const uint32_t preRoll = maxPreRoll > 0 ? randomRangeMs(0, maxPreRoll) : 0;
    playbackStartMs = grain.startMs - preRoll;
    playbackDurationMs = randomRangeMs(LOGOS_PHRASE_MIN_MS, LOGOS_PHRASE_MAX_MS);
  }
  // Rare clear word. Otherwise every grain is deconstructed, so intelligible
  // whole words never slip through by accident.
  const bool clearWord = launchMode == LOGOS_LAUNCH_AUTO && (nextRandomU32() % 100UL) < LOGOS_WORD_PERCENT;
  const bool phraseGhost = !clearWord && !forcePhrase && launchMode == LOGOS_LAUNCH_AUTO && repeatEligible &&
                           index > 0 && grain.durationMs >= LOGOS_MIN_DURATION_MS &&
                           (nextRandomU32() % 100UL) < LOGOS_PHRASE_GHOST_PERCENT;
  if (phraseGhost) {
    const uint32_t maxPreRoll = grain.startMs < LOGOS_PHRASE_PREROLL_MAX_MS ? grain.startMs : LOGOS_PHRASE_PREROLL_MAX_MS;
    const uint32_t preRoll = maxPreRoll > 0 ? randomRangeMs(0, maxPreRoll) : 0;
    playbackStartMs = grain.startMs - preRoll;
    playbackDurationMs = randomRangeMs(LOGOS_PHRASE_MIN_MS, LOGOS_PHRASE_MAX_MS);
  }
  stream.cloud = forcePhrase || phraseGhost ||
                 (!clearWord && repeatEligible && availableDurationMs >= LOGOS_MIN_DURATION_MS &&
                  (nextRandomU32() % 100UL) < logosCloudChance(index));
  stream.granular = stream.cloud;
  if (forceResponse || forceTail) {
    stream.cloud = false;
    stream.granular = true;
  } else if (!clearWord && !stream.cloud && playbackDurationMs > LOGOS_MICRO_MIN_MS) {
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
  stream.clearWord = clearWord;
  stream.launchMode = launchMode;

  uint32_t outputTotal = 0;
  if (forceResponse || forceTail) {
    stream.reversed = true;
    stream.rate = forceResponse && (nextRandomU32() & 1UL) ? logosArpRate(nextRandomU32() % 6UL) : 1.0f;
    outputTotal = setupLogosVoice(stream, 0, 0, loaded, 0, stream.rate, true);
    if (outputTotal > 0) {
      stream.voiceCount = 1;
    }
  } else if (stream.cloud) {
    stream.cloudPattern = chooseLogosCloudPattern(index);
    if (forcePhrase || phraseGhost) {
      const uint8_t r = nextRandomU32() % 100UL;
      stream.cloudPattern = r < 42 ? 1 : (r < 74 ? 2 : (r < 92 ? 0 : 3));
    }
    WavesetIndex wavesets;
    const bool haveWavesets = stream.cloudPattern == 4 && buildWavesetIndex(stream, wavesets) >= 4;
    if (stream.cloudPattern == 4 && !haveWavesets) {
      stream.cloudPattern = 3;
    }
    uint8_t desiredVoices = logosCloudVoiceCount(index);
    if (forcePhrase || phraseGhost) {
      desiredVoices = desiredVoices + 2 > LOGOS_MAX_CLOUD_VOICES ? LOGOS_MAX_CLOUD_VOICES : desiredVoices + 2;
    }
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
  if (clearWord && state == IDLE) {
    scheduleLogosResponse(index, grain);
  }

  Serial.print("LOGOS ");
  Serial.print(index + 1);
  Serial.print(' ');
  Serial.print(grain.filename);
  Serial.print(forceResponse ? " response" : (forceTail ? " tail" : (stream.cloud ? " cloud" : (stream.granular ? " micro" : " word"))));
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
  const bool wasClearWord = stream.clearWord;
  const uint8_t launchMode = stream.launchMode;
  stream.playing = false;
  stream.queuedFreshAudio = false;
  setLogosStreamGain(index, 0.0f);

  // Only re-trigger from SD while idle, to avoid SD reads during processing.
  if (state == IDLE && wasClearWord && launchMode == LOGOS_LAUNCH_AUTO &&
      (nextRandomU32() % 100UL) < LOGOS_REVERSE_TAIL_CHANCE_PERCENT) {
    const uint32_t tailMs = randomRangeMs(LOGOS_REVERSE_TAIL_MIN_MS, min(LOGOS_REVERSE_TAIL_MAX_MS, stream.currentGrain.durationMs));
    if (stream.currentGrain.durationMs > tailMs + LOGOS_MIN_DURATION_MS) {
      const uint32_t offset = stream.currentGrain.durationMs - tailMs;
      startLogosGrain(index, stream.currentGrain, offset, false, LOGOS_LAUNCH_REVERSE_TAIL);
      return;
    }
  }
  if (state == IDLE && stream.repeatEligible && (nextRandomU32() % 100UL) < LOGOS_REPEAT_CHANCE_PERCENT) {
    const uint32_t offset = randomRangeMs(LOGOS_REPEAT_MIN_OFFSET_MS, LOGOS_REPEAT_MAX_OFFSET_MS);
    if (stream.currentGrain.durationMs > offset + LOGOS_MIN_DURATION_MS) {
      startLogosGrain(index, stream.currentGrain, offset, false, LOGOS_LAUNCH_AUTO);
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
        // DC blocker (recordings carry DC since the ADC high-pass is disabled).
        const float logosDc = logosOut - stream.dcX1 + DC_BLOCK_R * stream.dcY1;
        stream.dcX1 = logosOut;
        stream.dcY1 = logosDc;
        followLogosEnvelope(logosDc);
        buffer[i] = floatToInt16(logosDc);

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
