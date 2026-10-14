// ---------------- Voices: LOGOS as speakers ----------------
// Port of lab/cube_states.py. After every take: STORM (15-30 s: fragments of
// syllables, fast pulse, sometimes a shared grid) -> SPEECH (1-2 min: words
// and short phrases of real speech) -> RARE (an episode every 10-20 s).
// Voices by archive size: <10 takes 1, <30 2, <80 3, else 4. Each voice runs
// on one LOGOS stream; a unit is read from SD into the stream's buffer.

float voicesLevel = 0.05f;        // median syllable rms: the level units are matched to
float voicesSnrMedian = 10.0f;
float voicesVoicedShare = 0.7f;
static const uint8_t VOICE_REGISTER_PCT[LOGOS_STREAM_COUNT] = {30, 70, 45, 60};
static const float VOICE_GAIN[LOGOS_STREAM_COUNT] = {1.0f, 0.85f, 0.7f, 0.6f};

uint8_t voicesForArchive() {
  const uint32_t n = archiveRecordings();
  return n < 10 ? 1 : n < 30 ? 2 : n < 80 ? 3 : 4;
}

VoicePhase voicesPhase(float &progress) {
  progress = 0.0f;
  if (voicesTakeMs == 0) {
    return PHASE_RARE;
  }
  const uint32_t since = millis() - voicesTakeMs;
  if (since < voicesStormMs) {
    progress = static_cast<float>(since) / voicesStormMs;
    return PHASE_STORM;
  }
  if (since < voicesStormMs + voicesSpeechMs) {
    progress = static_cast<float>(since - voicesStormMs) / voicesSpeechMs;
    return PHASE_SPEECH;
  }
  return PHASE_RARE;
}

static float uniformF(float lo, float hi) {
  return lo + (hi - lo) * static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
}

static float logNormal(float median, float sigma) {
  return median * expf(nextRandomGaussian(sigma * 1.41f));  // sum-of-6 gaussian has sd 0.71*sigma
}

static uint8_t poissonSmall(float lambda) {
  const float limit = expf(-lambda);
  float p = 1.0f;
  uint8_t k = 0;
  do {
    ++k;
    p *= uniformF(0.0f, 1.0f);
  } while (p > limit && k < 20);
  return k - 1;
}

// Slow random walk around a mean (Ornstein-Uhlenbeck), as lab Drift.
static void driftStep(float &x, float mean, float sd, float tauS, float dtS) {
  const float a = min(1.0f, dtS / tauS);
  x += (mean - x) * a + nextRandomGaussian(sd * sqrtf(2.0f * a) * 1.41f);
}

static bool isRecentSyll(uint32_t i) {
  for (uint8_t k = 0; k < VOICES_RECENT; ++k) {
    if (voicesRecent[k] == i + 1) {
      return true;
    }
  }
  return false;
}

static void rememberSyll(uint32_t i) {
  voicesRecent[voicesRecentWrite] = i + 1;
  voicesRecentWrite = (voicesRecentWrite + 1) % VOICES_RECENT;
}

// Archive statistics the voices aim at (a sample of up to 3000 syllables).
void initVoices() {
  const uint32_t n = min(syllCount, static_cast<uint32_t>(SYLL_MAX_HOPS));
  if (n == 0) {
    return;
  }
  const uint32_t step = max(1UL, static_cast<unsigned long>(syllCount / n));
  uint32_t m = 0, voiced = 0;
  for (uint32_t i = 0; i < syllCount && m < n; i += step) {
    syllFullEnv[m++] = syllIndex[i].rms;
  }
  voicesLevel = percentileOf(syllFullEnv, m, 2);
  m = 0;
  for (uint32_t i = 0; i < syllCount && m < n; i += step) {
    syllFullEnv[m++] = syllIndex[i].snr;
  }
  voicesSnrMedian = percentileOf(syllFullEnv, m, 2);
  m = 0;
  for (uint32_t i = 0; i < syllCount && m < n; i += step) {
    if (syllIndex[i].f0 > 0.0f) {
      syllFullEnv[m++] = syllIndex[i].f0;
    }
  }
  for (uint32_t i = 0; i < syllCount; i += step) {
    voiced += syllIndex[i].f0 > 0.0f ? 1 : 0;
  }
  voicesVoicedShare = static_cast<float>(voiced) / max(1UL, static_cast<unsigned long>((syllCount + step - 1) / step));
  for (uint8_t k = 0; k < LOGOS_STREAM_COUNT; ++k) {
    float base = 150.0f;
    if (m > 0) {
      for (uint32_t i = 0; i < m; ++i) {
        syllScratch[i] = syllFullEnv[i];
      }
      const uint32_t rank = (m * VOICE_REGISTER_PCT[k]) / 100;
      std::nth_element(syllScratch, syllScratch + rank, syllScratch + m);
      base = syllScratch[rank];
    }
    voiceAgents[k].regMean = log2f(base);
    if (voiceAgents[k].prev < 0) {
      voiceAgents[k].reg = voiceAgents[k].regMean;
    }
  }
}

// A take was saved: storm now, with its own lengths; its syllables are "new".
void voicesOnTake(uint16_t fileNumber, uint32_t firstSyll, uint32_t count) {
  (void)fileNumber;
  const uint32_t now = millis();
  voicesTakeMs = now;
  voicesStormMs = randomRangeMs(VOICES_STORM_MIN_MS, VOICES_STORM_MAX_MS);
  voicesSpeechMs = randomRangeMs(VOICES_SPEECH_MIN_MS, VOICES_SPEECH_MAX_MS);
  voicesNewFirst = firstSyll;
  voicesNewCount = count;
  voicesGridMs = randomRangeMs(120, 200);
  for (uint8_t k = 0; k < LOGOS_STREAM_COUNT; ++k) {
    VoiceAgent &ag = voiceAgents[k];
    ag.mode = VMODE_IDLE;
    ag.unitsLeft = 0;
    ag.burstEndMs = 0;
    ag.nextMs = now + 1200 + randomRangeMs(0, 800) * k;  // while the cube fades back in
  }
  initVoices();
  Serial.print("Voices: storm ");
  Serial.print(voicesStormMs / 1000);
  Serial.print(" s, speech ");
  Serial.print(voicesSpeechMs / 1000);
  Serial.print(" s, new take syllables ");
  Serial.println(count);
}

// Best of 8 by sound (pitch, brightness, length, voicing) and background
// match with the previous syllable; never the same take twice in a row.
int32_t pickSyll(VoiceAgent &agent, float f0Target, float brightTarget, float lengthTarget, float voicedTarget) {
  if (syllCount == 0) {
    return -1;
  }
  const uint16_t prevFile = agent.prev >= 0 ? syllIndex[agent.prev].file : 0;
  const float snrTarget = agent.prev >= 0 ? syllIndex[agent.prev].snr : 0.0f;
  int32_t best[8];
  float bestD[8];
  uint8_t filled = 0;
  for (uint32_t i = 0; i < syllCount; ++i) {
    const SyllEntry &c = syllIndex[i];
    if (c.file == prevFile && syllCount > 1) {
      continue;
    }
    float d = 0.0f;
    if (f0Target > 0.0f && c.f0 > 0.0f) {
      d += 2.0f * fabsf(log2f(c.f0 / f0Target));
    } else if ((f0Target > 0.0f) != (c.f0 > 0.0f)) {
      d += 0.8f;
    }
    d += 0.8f * fabsf(log2f(max(c.bright, 50.0f) / max(brightTarget, 50.0f)));
    d += 0.6f * fabsf(log2f((static_cast<float>(c.length) / SAMPLE_RATE) / max(lengthTarget, 0.03f)));
    d += 0.3f * fabsf(c.voiced / 100.0f - voicedTarget);
    if (snrTarget > 0.0f) {
      d += 0.7f * fabsf(log2f(max(c.snr, 0.01f) / snrTarget)) + 0.5f * max(0.0f, log2f(voicesSnrMedian / max(c.snr, 0.01f)));
    }
    if (filled == 8 && d >= bestD[7]) {
      continue;
    }
    if (isRecentSyll(i)) {
      continue;
    }
    uint8_t pos = filled < 8 ? filled++ : 7;
    while (pos > 0 && bestD[pos - 1] > d) {
      bestD[pos] = bestD[pos - 1];
      best[pos] = best[pos - 1];
      --pos;
    }
    bestD[pos] = d;
    best[pos] = static_cast<int32_t>(i);
  }
  if (filled == 0) {
    return static_cast<int32_t>(nextRandomU32() % syllCount);
  }
  float total = 0.0f;  // weights 1.0 .. 0.3 like the lab
  for (uint8_t k = 0; k < filled; ++k) {
    total += 1.0f - 0.7f * k / max(1, filled - 1);
  }
  float r = uniformF(0.0f, total);
  for (uint8_t k = 0; k < filled; ++k) {
    r -= 1.0f - 0.7f * k / max(1, filled - 1);
    if (r <= 0.0f) {
      return best[k];
    }
  }
  return best[filled - 1];
}

// Neighbouring syllables of the same take while they stay close (<= 350 ms
// apart) and the stretch is shorter than targetSeconds.
uint32_t extendUnit(uint32_t first, float targetSeconds) {
  uint32_t last = first;
  const uint32_t limit = static_cast<uint32_t>(targetSeconds * SAMPLE_RATE);
  while (last + 1 < syllCount) {
    const SyllEntry &cur = syllIndex[last];
    const SyllEntry &nxt = syllIndex[last + 1];
    if (nxt.file != cur.file || nxt.idx != cur.idx + 1) {
      break;
    }
    if (nxt.start - (cur.start + cur.length) > SAMPLE_RATE * 35UL / 100UL ||
        nxt.start + nxt.length - syllIndex[first].start > limit) {
      break;
    }
    ++last;
  }
  return last;
}

// Reads one unit into the stream and starts it. cutLength > 0 plays a cut
// [cutStart, cutStart + cutLength) of the take instead (storm fragments).
bool startVoiceUnit(uint8_t index, uint32_t first, uint32_t last, bool air, bool reverse, float gainScale,
                    uint32_t cutStart, uint32_t cutLength) {
  LogosStream &stream = logosStreams[index];
  const SyllEntry &a = syllIndex[first];
  const SyllEntry &b = syllIndex[last];
  uint32_t s = cutLength > 0 ? cutStart : a.start;
  uint32_t e = cutLength > 0 ? cutStart + cutLength : b.start + b.length;
  uint32_t pre = (air && cutLength == 0) ? a.pre : 0;
  uint32_t post = (air && cutLength == 0) ? b.post : 0;
  uint32_t from = s - pre;
  uint32_t total = e + post - from;
  if (total > LOGOS_MAX_GRAIN_SAMPLES) {
    total = LOGOS_MAX_GRAIN_SAMPLES;
    post = 0;
  }

  char name[13];
  makeFilename(name, a.file);
  FsFile file = SD.sdfs.open(name, O_RDONLY);
  if (!file) {
    Serial.print("Could not open voice source ");
    Serial.println(name);
    kickWatchdog();
    if (!SD.exists(SYLLS_FILE)) {  // the card is gone: reboot and wait for it
      rebootNow("SD card unreadable");
    }
    return false;
  }
  if (!file.seekSet(44 + from * 2UL)) {
    file.close();
    return false;
  }
  const int got = file.read(reinterpret_cast<uint8_t *>(stream.buffer), total * 2UL);
  file.close();
  if (got < 128) {
    return false;
  }
  const uint32_t loaded = static_cast<uint32_t>(got) / 2UL;
  if (pre + post >= loaded) {
    pre = post = 0;
  }

  double sumSq = 0.0;  // level match on the core, without the air
  for (uint32_t i = pre; i < loaded - post; ++i) {
    const float v = stream.buffer[i] * (1.0f / 32768.0f);
    sumSq += static_cast<double>(v) * v;
  }
  const float coreRms = sqrtf(sumSq / max(1UL, static_cast<unsigned long>(loaded - pre - post)));
  stream.unitGain = min(voicesLevel * VOICE_GAIN[index] * gainScale / max(coreRms, 1e-5f), VOICES_UNIT_GAIN_MAX);

  clearLogosVoices(stream);
  LogosVoice &v = stream.voices[0];
  const uint32_t minFade = SAMPLE_RATE * 8UL / 1000UL;
  v.active = true;
  v.reversed = reverse;
  v.delaySamples = 0;
  v.sourceStart = 0;
  v.sourceEnd = loaded;
  v.samplesRemaining = loaded;
  v.samplesTotal = loaded;
  v.samplesPlayed = 0;
  v.rate = reverse ? -1.0f : 1.0f;
  v.readPos = reverse ? static_cast<float>(loaded - 1) : 0.0f;
  v.fadeIn = min(max(reverse ? post : pre, minFade), loaded / 2);
  v.fadeOut = min(max(reverse ? pre : post, minFade), loaded / 2);
  stream.voiceCount = 1;
  stream.bufferSamples = loaded;
  stream.reversed = reverse;
  stream.rate = 1.0f;
  stream.granular = cutLength > 0;
  stream.cloud = false;
  stream.clearWord = false;
  stream.samplesTotal = loaded;
  stream.samplesRemaining = loaded;
  stream.samplesPlayed = 0;
  stream.queuedFreshAudio = false;
  stream.blockReady = false;
  stream.playing = true;
  makeFilename(stream.currentGrain.filename, a.file);
  stream.currentGrain.startMs = from * 1000UL / SAMPLE_RATE;
  stream.currentGrain.durationMs = loaded * 1000UL / SAMPLE_RATE;
  rememberSyll(first);
  return true;
}

void finishVoiceUnit(uint8_t index) {
  LogosStream &stream = logosStreams[index];
  stream.playing = false;
  stream.queuedFreshAudio = false;
  VoiceAgent &ag = voiceAgents[index];
  ag.nextMs = millis() + static_cast<uint32_t>(max(0L, static_cast<long>(ag.gapAfterMs)));
}

static uint32_t randomGapMs() {  // a real pause between syllables of the archive
  for (uint8_t t = 0; t < 8; ++t) {
    const SyllEntry &c = syllIndex[nextRandomU32() % syllCount];
    if (c.gapMs >= 30 && c.gapMs <= 1200) {
      return c.gapMs;
    }
  }
  return 150;
}

static void logUnit(uint8_t index, const char *what, uint32_t first, uint32_t last, bool reverse) {
  Serial.print("V");
  Serial.print(index + 1);
  Serial.print(" ");
  Serial.print(what);
  Serial.print(" W");
  Serial.print(syllIndex[first].file);
  Serial.print("#");
  Serial.print(syllIndex[first].idx);
  if (last != first) {
    Serial.print("-");
    Serial.print(syllIndex[last].idx);
  }
  Serial.println(reverse ? " REV" : "");
}

// One word-level unit of real speech (speech, phrases and episodes).
static bool playWord(uint8_t index, float f0Target, float targetS, bool reverse, const char *what) {
  VoiceAgent &ag = voiceAgents[index];
  const float bright = ag.prev >= 0 ? syllIndex[ag.prev].bright : 1500.0f;
  const int32_t first = pickSyll(ag, f0Target, bright, min(targetS, 0.5f), f0Target > 0.0f ? 0.9f : 0.3f);
  if (first < 0) {
    return false;
  }
  const uint32_t last = extendUnit(first, targetS);
  if (!startVoiceUnit(index, first, last, true, reverse, 1.0f, 0, 0)) {
    return false;
  }
  ag.prev = static_cast<int32_t>(last);
  ag.repeatFirst = first;
  ag.repeatLast = static_cast<int32_t>(last);
  if (static_cast<uint8_t>(nextRandomU32() % 100UL) < 50) {
    exciteMelosFromLogos(index, false, reverse, 1.0f);
  }
  logUnit(index, what, first, last, reverse);
  return true;
}

// STORM / burst: a cut of one syllable, 60-250 ms; 40 % from the new take.
static bool playFragment(uint8_t index, bool fromNewTake) {
  uint32_t i;
  if (fromNewTake && voicesNewCount > 0 && static_cast<uint8_t>(nextRandomU32() % 100UL) < VOICES_NEW_TAKE_PERCENT) {
    i = voicesNewFirst + nextRandomU32() % voicesNewCount;
  } else {
    i = nextRandomU32() % syllCount;
  }
  const SyllEntry &c = syllIndex[i];
  uint32_t length = randomRangeMs(60, 250) * (SAMPLE_RATE / 1000UL);
  uint32_t start = c.start;
  if (c.length > length) {
    start += nextRandomU32() % (c.length - length);
  } else {
    length = c.length;
  }
  const bool reverse = static_cast<uint8_t>(nextRandomU32() % 100UL) < VOICES_STORM_REVERSE_PERCENT;
  if (!startVoiceUnit(index, i, i, false, reverse, 0.8f, start, length)) {
    return false;
  }
  if ((nextRandomU32() & 3UL) == 0) {
    exciteMelosFromLogos(index, true, reverse, 1.0f);
  }
  return true;
}

// A burst of fragments, sometimes locked to the shared grid. Returns false
// when the burst is over (caller decides the pause).
static bool stormStep(uint8_t index, float progress, bool fromNewTake) {
  VoiceAgent &ag = voiceAgents[index];
  const uint32_t now = millis();
  if (ag.burstEndMs == 0) {
    ag.burstEndMs = now + randomRangeMs(1000, 3000);
    ag.gridMs = (nextRandomU32() % 100UL) < 45 ? voicesGridMs : 0;
    ag.burstRate = uniformF(5.0f, 8.0f) * (1.0f - 0.3f * progress);
  } else if (static_cast<int32_t>(now - ag.burstEndMs) >= 0) {
    ag.burstEndMs = 0;
    return false;
  }
  const uint32_t fragmentMs = 150;  // typical; the stream ends it when it is done
  if (!playFragment(index, fromNewTake)) {
    ag.nextMs = now + 200;
    return true;
  }
  uint32_t targetMs;
  if (ag.gridMs > 0) {  // next tick of the common pulse, counted from the take
    const uint32_t since = now + fragmentMs / 2 - voicesTakeMs;
    targetMs = voicesTakeMs + ((since / ag.gridMs) + 1) * ag.gridMs;
  } else {
    targetMs = now + static_cast<uint32_t>(1000.0f / ag.burstRate * uniformF(0.85f, 1.25f));
  }
  const uint32_t unitMs = logosStreams[index].samplesTotal * 1000UL / SAMPLE_RATE;
  ag.gapAfterMs = static_cast<int32_t>(targetMs - now) - static_cast<int32_t>(unitMs);
  return true;
}

static float contourOffset(uint8_t contour, uint8_t u, uint8_t n) {
  const float frac = n > 1 ? static_cast<float>(u) / (n - 1) : 0.0f;
  switch (contour) {
    case 0: return 0.2f - 0.45f * frac;            // fall
    case 1: return -0.15f + 0.4f * frac;           // rise
    case 2: return 0.25f * sinf(PI * frac);        // arch
    default: return 0.0f;                          // flat
  }
}

// SPEECH: phrases of 1-4 word-level units; pauses grow toward the end.
static void speechStep(uint8_t index, float progress) {
  VoiceAgent &ag = voiceAgents[index];
  const uint32_t now = millis();
  if (ag.mode != VMODE_PHRASE || ag.unitsLeft == 0) {
    if (ag.mode == VMODE_PHRASE) {  // phrase done: breathe
      ag.mode = VMODE_IDLE;
      ag.nextMs = now + static_cast<uint32_t>(1000.0f * logNormal(0.8f + 2.5f * progress, 0.5f));
      return;
    }
    ag.mode = VMODE_PHRASE;
    ag.unitsTotal = constrain(poissonSmall(2.2f), 1, 4);
    ag.unitsLeft = ag.unitsTotal;
    ag.contour = nextRandomU32() % 4UL;
    ag.voicedPhrase = uniformF(0.0f, 1.0f) < voicesVoicedShare;
    driftStep(ag.rubato, 0.0f, 0.35f, 12.0f, 3.0f);
    driftStep(ag.reg, ag.regMean, 0.2f, 30.0f, 3.0f);
  }
  const uint8_t u = ag.unitsTotal - ag.unitsLeft;
  const float f0 = ag.voicedPhrase ? exp2f(ag.reg + contourOffset(ag.contour, u, ag.unitsTotal)) : 0.0f;
  const float pace = exp2f(ag.rubato);
  const float target = constrain(logNormal(0.9f / pace, 0.4f), 0.4f, 1.5f);
  const bool reverse = static_cast<uint8_t>(nextRandomU32() % 100UL) < VOICES_SPEECH_REVERSE_PERCENT;
  ag.unitsLeft--;
  if (!playWord(index, f0, target, reverse, "speech")) {
    ag.nextMs = now + 300;
    return;
  }
  ag.gapAfterMs = static_cast<int32_t>(randomGapMs() / pace * uniformF(0.7f, 1.3f));
}

// RARE: voice 1 starts an episode every 10-20 s; the others sometimes join.
static void beginEpisode(uint8_t index) {
  VoiceAgent &ag = voiceAgents[index];
  const uint32_t r = nextRandomU32() % 100UL;
  ag.episode = r < 30 ? EP_PHRASE : r < 55 ? EP_WORD : r < 70 ? EP_REVERSED : r < 85 ? EP_BURST : EP_REPEAT;
  if (index > 0 && (ag.episode == EP_BURST || ag.episode == EP_REPEAT)) {
    ag.episode = EP_WORD;  // someone joining just says something
  }
  ag.mode = VMODE_EPISODE;
  ag.episodeEndMs = millis() + VOICES_EPISODE_MAX_MS;
  ag.unitsTotal = ag.episode == EP_PHRASE ? constrain(poissonSmall(1.8f), 1, 3)
                  : ag.episode == EP_REPEAT ? static_cast<uint8_t>(2 + nextRandomU32() % 2UL)
                                            : 1;
  ag.unitsLeft = ag.unitsTotal;
  ag.contour = nextRandomU32() % 4UL;
  ag.voicedPhrase = uniformF(0.0f, 1.0f) < voicesVoicedShare;
  ag.burstEndMs = 0;
  ag.repeatFirst = -1;
  if (index == 0) {
    for (uint8_t k = 1; k < voicesForArchive(); ++k) {
      if ((nextRandomU32() % 100UL) < 30) {
        voiceAgents[k].mode = VMODE_EPISODE;
        voiceAgents[k].episode = EP_WORD;
        voiceAgents[k].unitsTotal = voiceAgents[k].unitsLeft = 1;
        voiceAgents[k].episodeEndMs = millis() + VOICES_EPISODE_MAX_MS;
        voiceAgents[k].nextMs = millis() + randomRangeMs(200, 3000);
      }
    }
  }
}

static void endEpisode(uint8_t index) {
  VoiceAgent &ag = voiceAgents[index];
  ag.mode = VMODE_IDLE;
  if (index == 0) {
    voicesNextEpisodeMs = millis() + randomRangeMs(VOICES_RARE_MIN_MS, VOICES_RARE_MAX_MS);
    ag.nextMs = voicesNextEpisodeMs;
  } else {
    ag.nextMs = millis() + 1000;
  }
}

static void rareStep(uint8_t index) {
  VoiceAgent &ag = voiceAgents[index];
  const uint32_t now = millis();
  if (ag.mode == VMODE_PHRASE && ag.unitsLeft > 0) {  // finish a phrase begun in speech
    speechStep(index, 1.0f);
    return;
  }
  if (ag.mode != VMODE_EPISODE) {
    if (index == 0 && static_cast<int32_t>(now - voicesNextEpisodeMs) >= 0) {
      beginEpisode(index);
    } else {
      ag.mode = VMODE_IDLE;
      ag.nextMs = now + 500;
      return;
    }
  }
  if (static_cast<int32_t>(now - ag.episodeEndMs) >= 0) {
    endEpisode(index);
    return;
  }
  switch (ag.episode) {
    case EP_BURST:
      if (!stormStep(index, 0.5f, false)) {
        endEpisode(index);
      }
      return;
    case EP_REPEAT:
      if (ag.unitsLeft == 0) {
        endEpisode(index);
        return;
      }
      if (ag.repeatFirst < 0) {
        if (!playWord(index, 0.0f, uniformF(0.3f, 0.8f), false, "repeat")) {
          endEpisode(index);
          return;
        }
      } else if (!startVoiceUnit(index, ag.repeatFirst, ag.repeatLast, true, false, 1.0f, 0, 0)) {
        endEpisode(index);
        return;
      } else {
        logUnit(index, "repeat", ag.repeatFirst, ag.repeatLast, false);
      }
      ag.unitsLeft--;
      ag.gapAfterMs = static_cast<int32_t>(randomRangeMs(150, 600));
      return;
    default: {
      if (ag.unitsLeft == 0) {
        endEpisode(index);
        return;
      }
      const uint8_t u = ag.unitsTotal - ag.unitsLeft;
      const float f0 = ag.voicedPhrase ? exp2f(ag.reg + contourOffset(ag.contour, u, ag.unitsTotal)) : 0.0f;
      const bool reverse = ag.episode == EP_REVERSED;
      const float target = ag.episode == EP_PHRASE ? constrain(logNormal(0.9f, 0.4f), 0.4f, 1.5f) : uniformF(0.4f, 1.2f);
      ag.unitsLeft--;
      if (!playWord(index, f0, target, reverse, ag.episode == EP_PHRASE ? "rare phrase" : reverse ? "rare reversed" : "rare word")) {
        endEpisode(index);
        return;
      }
      ag.gapAfterMs = static_cast<int32_t>(randomGapMs() * uniformF(0.7f, 1.3f));
      return;
    }
  }
}

// Called from updateIdlePlayback() while IDLE: start the next unit of every
// voice that is free.
void updateVoices() {
  if (syllCount == 0) {
    return;
  }
  float progress = 0.0f;
  const VoicePhase phase = voicesPhase(progress);
  const uint8_t active = voicesForArchive();
  const uint32_t now = millis();
  for (uint8_t i = 0; i < active; ++i) {
    VoiceAgent &ag = voiceAgents[i];
    if (logosStreams[i].playing || static_cast<int32_t>(now - ag.nextMs) < 0) {
      continue;
    }
    if (phase == PHASE_STORM) {
      if (ag.mode != VMODE_STORM) {
        ag.mode = VMODE_STORM;
        ag.burstEndMs = 0;
      }
      if (!stormStep(i, progress, true)) {
        ag.nextMs = now + randomRangeMs(100, 500);  // breath between bursts
      }
    } else if (phase == PHASE_SPEECH) {
      speechStep(i, progress);
    } else {
      rareStep(i);
    }
  }
}

void printVoices() {
  float progress = 0.0f;
  const VoicePhase phase = voicesPhase(progress);
  Serial.print("  voices: phase ");
  Serial.print(phase == PHASE_STORM ? "STORM" : phase == PHASE_SPEECH ? "SPEECH" : "RARE");
  Serial.print(" ");
  Serial.print(static_cast<int>(progress * 100));
  Serial.print("%, active ");
  Serial.print(voicesForArchive());
  Serial.print(", syllables ");
  Serial.print(syllCount);
  Serial.print(", level ");
  Serial.print(voicesLevel, 4);
  Serial.print(", snr median ");
  Serial.println(voicesSnrMedian, 1);
  const uint32_t now = millis();
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    const VoiceAgent &ag = voiceAgents[i];
    Serial.print("    V");
    Serial.print(i + 1);
    Serial.print(logosStreams[i].playing ? " playing" : " idle");
    Serial.print(", mode ");
    Serial.print(ag.mode);
    Serial.print(", register ");
    Serial.print(exp2f(ag.reg), 0);
    Serial.print(" Hz, next in ");
    Serial.print(static_cast<int32_t>(ag.nextMs - now) > 0 ? ag.nextMs - now : 0);
    Serial.println(" ms");
  }
}
