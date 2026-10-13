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

uint8_t dominantBandIndex(const float bands[]) {
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
void triggerMelosLearnedGesture(float incomingF0, float incomingVoicedRatio, const float incomingBands[]) {
  melosLearnAmount = 1.0f;
  melosLearnF0 = incomingF0 > 0.0f && isfinite(incomingF0) ? constrain(incomingF0, F0_MIN_HZ, F0_MAX_HZ) : constrain(expf(model.meanLogF0), F0_MIN_HZ, F0_MAX_HZ);
  melosLearnBreath = constrain(1.0f - incomingVoicedRatio, 0.0f, 1.0f);
  melosLearnBand = dominantBandIndex(incomingBands);
  melosLogosOpen = constrain(melosLogosOpen + 0.45f, 0.0f, 1.0f);
  melosLogosStabilize = constrain(melosLogosStabilize + 0.35f, 0.0f, 1.0f);
  triggerMelosSpeechGhost(0.78f, randomRangeMs(2600, 6200));
  Serial.print("MELOS learned gesture: f0=");
  Serial.print(melosLearnF0, 2);
  Serial.print(" Hz, breath=");
  Serial.print(melosLearnBreath, 3);
  Serial.print(", dominantBand=");
  Serial.println(melosLearnBand);
}

void triggerMelosSpeechGhost(float strength, uint32_t durationMs) {
  strength = constrain(strength, 0.0f, 1.0f);
  if (strength <= 0.0f) {
    return;
  }
  durationMs = constrain(durationMs, MELOS_SPEECH_GHOST_MIN_MS, MELOS_SPEECH_GHOST_MAX_MS);
  melosSpeechGhostTarget = constrain(melosSpeechGhostTarget + strength, 0.0f, 1.0f);
  melosSpeechReleaseSample = melosSampleCounter + durationMs * (SAMPLE_RATE / 1000UL);
  const float r = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
  melosSpeechRateHz = MELOS_SPEECH_GHOST_MIN_RATE_HZ +
                      (MELOS_SPEECH_GHOST_MAX_RATE_HZ - MELOS_SPEECH_GHOST_MIN_RATE_HZ) * r;
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

float melosSpectralCentroid() {
  return melosVowelPosition();
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

void melosApplyArticulation(float &f1, float &f2, float &f3, float &oralDamping, float &fricativeMix) {
  const float tongue = constrain(melosTonguePos + melosSpeechTongue, 0.0f, 1.0f);
  const float diameter = constrain(melosTongueDiameter + melosSpeechDiameter, 0.12f, 1.0f);
  const float closed = constrain(1.0f - diameter, 0.0f, 1.0f);
  const float back = constrain(melosConstrBack + melosSpeechBack + closed * (1.0f - tongue) * 0.45f, 0.0f, 1.0f);
  const float mid = constrain(melosConstrMid + melosSpeechMid + closed * (1.0f - fabsf(tongue - 0.5f) * 2.0f) * 0.38f, 0.0f, 1.0f);
  const float front = constrain(melosConstrFront + melosSpeechFront + closed * tongue * 0.55f, 0.0f, 1.0f);

  // Back constriction: darker swallowed vowels. Front constriction: brighter,
  // noisier almost-sibilants. Mid constriction pinches the mouth without making
  // literal consonants.
  f1 *= 1.0f + back * 0.28f - front * 0.18f;
  f2 *= 1.0f - back * 0.34f + front * 0.42f - mid * 0.12f;
  f3 *= 1.0f + front * 0.24f + mid * 0.16f;
  oralDamping = constrain(back * 0.42f + mid * 0.30f + front * 0.22f, 0.0f, 0.82f);
  const float thinness = constrain(8.0f * (0.70f - diameter), 0.0f, 1.0f);
  const float openness = constrain(30.0f * (diameter - 0.30f), 0.0f, 1.0f);
  const float turbulenceWindow = thinness * openness;
  fricativeMix = constrain((front * 0.92f + mid * 0.34f) * (0.18f + turbulenceWindow * 0.96f), 0.0f, 1.0f);

  f1 = constrain(f1, 180.0f, 1100.0f);
  f2 = constrain(f2, 500.0f, 3400.0f);
  f3 = constrain(f3, 1600.0f, 4300.0f);
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
  static const float formantQ[3] = {7.2f, 9.5f, 10.8f};
  for (uint8_t k = 0; k < 3; ++k) {
    const float g = tanf(PI * melosFormantFreq[k] / SAMPLE_RATE);
    const float kq = 1.0f / formantQ[k];
    const float a1 = 1.0f / (1.0f + g * (g + kq));
    melosFormantA1[k] = a1;
    melosFormantA2[k] = g * a1;
    melosFormantA3[k] = g * melosFormantA2[k];
  }
}

void melosComputeTractCoeffs() {
  for (uint8_t k = 0; k < MELOS_TRACT_SECTIONS; ++k) {
    const float g = tanf(PI * melosTractFreq[k] / SAMPLE_RATE);
    const float kq = 1.0f / MELOS_TRACT_Q;
    const float a1 = 1.0f / (1.0f + g * (g + kq));
    melosTractA1[k] = a1;
    melosTractA2[k] = g * a1;
    melosTractA3[k] = g * melosTractA2[k];
  }
}

void melosUpdateMovingTract(float constr) {
  const float blk = static_cast<float>(AUDIO_BLOCK_SAMPLES);
  const float tongue = constrain(melosTonguePos + melosSpeechTongue, 0.0f, 1.0f);
  const float diameter = constrain(melosTongueDiameter + melosSpeechDiameter, 0.12f, 1.0f);
  const float closed = constrain(1.0f - diameter, 0.0f, 1.0f);
  const float back = constrain(melosConstrBack + melosSpeechBack + closed * (1.0f - tongue) * 0.45f, 0.0f, 1.0f);
  const float mid = constrain(melosConstrMid + melosSpeechMid + closed * (1.0f - fabsf(tongue - 0.5f) * 2.0f) * 0.38f, 0.0f, 1.0f);
  const float front = constrain(melosConstrFront + melosSpeechFront + closed * tongue * 0.55f, 0.0f, 1.0f);

  // Three broad oral-tube sections. They intentionally follow the same physical
  // controls as the formants, but move more slowly and in series, so MELOS gets
  // cavity memory rather than only parallel vowel peaks.
  float target[MELOS_TRACT_SECTIONS];
  target[0] = 420.0f + closed * 360.0f + back * 280.0f - front * 80.0f;
  target[1] = 880.0f + tongue * 520.0f + mid * 540.0f - back * 160.0f;
  target[2] = 1700.0f + tongue * 1050.0f + front * 860.0f + constr * 420.0f;
  target[0] = constrain(target[0], 240.0f, 1050.0f);
  target[1] = constrain(target[1], 620.0f, 2300.0f);
  target[2] = constrain(target[2], 1300.0f, 4200.0f);

  for (uint8_t k = 0; k < MELOS_TRACT_SECTIONS; ++k) {
    melosTractFreq[k] += (target[k] - melosTractFreq[k]) * (MELOS_TRACT_EASE * blk);
  }
  melosComputeTractCoeffs();
}

float melosApplyMovingTract(float mouthOut) {
  static const float sectionGain[MELOS_TRACT_SECTIONS] = {1.0f, 0.78f, 0.54f};
  float serial = mouthOut;
  for (uint8_t k = 0; k < MELOS_TRACT_SECTIONS; ++k) {
    const float v3 = serial - melosTractS2[k];
    const float v1 = melosTractA1[k] * melosTractS1[k] + melosTractA2[k] * v3;
    const float v2 = melosTractS2[k] + melosTractA2[k] * melosTractS1[k] + melosTractA3[k] * v3;
    melosTractS1[k] = 2.0f * v1 - melosTractS1[k];
    melosTractS2[k] = 2.0f * v2 - melosTractS2[k];
    serial = serial * (1.0f - MELOS_TRACT_DRIVE) + v1 * (MELOS_TRACT_DRIVE * sectionGain[k]);
  }
  const float blend = constrain(MELOS_TRACT_BLEND * (0.64f + melosTenseness * 0.28f + melosSpeechGhost * 0.22f) *
                                (1.0f - melosOralDamping * 0.30f),
                                0.0f, 0.58f);
  return mouthOut * (1.0f - blend) + serial * blend;
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
    const float vibPhase = (static_cast<float>(melosSampleCounter) / SAMPLE_RATE) * MELOS_VIBRATO_RATE_HZ +
                           0.17f * static_cast<float>(v);
    const float vibDepth = MELOS_VIBRATO_DEPTH * (0.55f + melosSpeechGhost * 0.85f + melosLearnAmount * 0.35f);
    melosVoiceF0[v] = melosF0 * expf(semis * (0.6931472f / 12.0f)) *
                      (1.0f + sineLookup(vibPhase) * vibDepth);
  }

  // Autonomous timbral movement (bounded, around archive-set centers).
  melosVowelWander += nextRandomGaussian(0.0026f);
  melosVowelWander = constrain(melosVowelWander, -MELOS_VOWEL_WANDER, MELOS_VOWEL_WANDER);

  // Archive mouth shape: spectral centroid becomes tongue position; voiced,
  // stable archives open the mouth, while breathy/dense archives narrow it.
  const float archiveTongue = melosSpectralCentroid();
  const float densityClose = constrain(melosLogosDensity * 0.26f + melosLogosConstrict * 0.34f, 0.0f, 0.60f);
  melosTongueTarget += (archiveTongue - melosTongueTarget) * (MELOS_TONGUE_EASE * blk);
  melosTongueTarget += melosLogosOpen * (0.5f - melosTongueTarget) * 0.22f;
  melosTongueTarget += melosLogosConstrict * (melosConstrFront >= melosConstrBack ? 0.10f : -0.10f);
  melosTongueTarget = constrain(melosTongueTarget + nextRandomGaussian(0.0014f), 0.0f, 1.0f);
  melosTonguePos += (melosTongueTarget - melosTonguePos) * (MELOS_TONGUE_EASE * blk);

  const float openBias = constrain(melosLogosOpen * 0.22f + melosLogosStabilize * 0.16f, 0.0f, 0.38f);
  const float diameterCenter = constrain(0.48f + melosVoicedRatio * 0.30f + openBias - densityClose, 0.18f, 0.94f);
  melosTongueDiameterTarget += (diameterCenter - melosTongueDiameterTarget) * (MELOS_TONGUE_DIAMETER_EASE * blk);
  melosTongueDiameter += (melosTongueDiameterTarget - melosTongueDiameter) * (MELOS_TONGUE_DIAMETER_EASE * blk);

  if (melosSampleCounter >= melosNextMurmurSample && melosSpeechGhost < 0.08f) {
    triggerMelosSpeechGhost(0.20f + 0.15f * (static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f),
                            randomRangeMs(2200, 5200));
    melosNextMurmurSample = melosSampleCounter + melosRandomSamples(MELOS_MURMUR_MIN_MS, MELOS_MURMUR_MAX_MS);
  }

  // Speech ghost: occasional fast human-motor articulation rhythm. It does not
  // sequence phonemes; it briefly makes the slow collective mouth remember the
  // jaw/tongue tempo of speech, then dissolves back into ambient drift.
  if (melosSampleCounter < melosSpeechReleaseSample) {
    melosSpeechGhostTarget = constrain(melosSpeechGhostTarget, 0.0f, 1.0f);
  } else {
    melosSpeechGhostTarget = 0.0f;
  }
  melosSpeechGhost += (melosSpeechGhostTarget - melosSpeechGhost) * (MELOS_SPEECH_GHOST_EASE * blk);
  melosSpeechPhase += melosSpeechRateHz * (blk / static_cast<float>(SAMPLE_RATE));
  if (melosSpeechPhase >= 1.0f) {
    melosSpeechPhase -= floorf(melosSpeechPhase);
    melosSpeechRateHz += nextRandomGaussian(0.42f);
    melosSpeechRateHz = constrain(melosSpeechRateHz, MELOS_SPEECH_GHOST_MIN_RATE_HZ, MELOS_SPEECH_GHOST_MAX_RATE_HZ);
  }
  const float jaw = 0.5f + 0.5f * sineLookup(melosSpeechPhase);
  const float jawSharp = jaw * jaw;
  const float tongueFlutter = sineLookup(melosSpeechPhase * 2.0f + 0.19f) * 0.70f +
                              sineLookup(melosSpeechPhase * 3.0f + 0.41f) * 0.30f;
  melosSpeechTongue = melosSpeechGhost * tongueFlutter * 0.17f;
  melosSpeechDiameter = melosSpeechGhost * (jawSharp * 0.23f - (1.0f - jawSharp) * 0.14f);
  melosSpeechBack = melosSpeechGhost * constrain(-tongueFlutter, 0.0f, 1.0f) * 0.13f;
  melosSpeechMid = melosSpeechGhost * (0.09f + (1.0f - jawSharp) * 0.23f);
  melosSpeechFront = melosSpeechGhost * constrain(tongueFlutter, 0.0f, 1.0f) * 0.24f;

  // Nasality drifts toward an archive-set center (darker/hummed archive = more nasal).
  const float nasalCenter = constrain(0.16f + (1.0f - melosVowelPos) * 0.34f + melosVoicedRatio * 0.16f, 0.0f, 0.72f);
  melosNasalTarget += (nasalCenter - melosNasalTarget) * (MELOS_NASAL_EASE * blk);
  melosNasalTarget += nextRandomGaussian(0.0030f);
  melosNasalTarget = constrain(melosNasalTarget, 0.0f, 0.95f);
  melosNasal += (melosNasalTarget - melosNasal) * (MELOS_NASAL_EASE * blk);

  // Wandering constriction: rare gentle swells -> "almost-consonants".
  if (melosSampleCounter >= melosConstrictNextSample) {
    const float rr = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
    const float rr2 = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
    melosConstrictAutoTarget = rr < 0.40f ? 0.0f : (0.22f + 0.55f * rr2);
    melosConstrictNextSample = melosSampleCounter + melosRandomSamples(1500, 6000);
  }
  melosConstrictAuto += (melosConstrictAutoTarget - melosConstrictAuto) * (MELOS_CONSTRICT_EASE * blk);
  melosConstrBackTarget += (0.0f - melosConstrBackTarget) * (MELOS_ARTICULATION_DECAY * blk);
  melosConstrMidTarget += (0.0f - melosConstrMidTarget) * (MELOS_ARTICULATION_DECAY * blk);
  melosConstrFrontTarget += (0.0f - melosConstrFrontTarget) * (MELOS_ARTICULATION_DECAY * blk);
  melosConstrBack += (melosConstrBackTarget - melosConstrBack) * (MELOS_ARTICULATION_EASE * blk);
  melosConstrMid += (melosConstrMidTarget - melosConstrMid) * (MELOS_ARTICULATION_EASE * blk);
  melosConstrFront += (melosConstrFrontTarget - melosConstrFront) * (MELOS_ARTICULATION_EASE * blk);

  // Vowel from the archive (+ autonomous wander); LOGOS events open it toward aa.
  float vowelTarget = constrain(melosVowelPosition() + melosVowelWander, 0.0f, 1.0f);
  const float openAmt = constrain(melosLogosOpen + melosLogosStabilize * 0.5f + melosLogosEnvelope * 0.4f, 0.0f, 1.0f);
  vowelTarget = vowelTarget * (1.0f - openAmt * 0.5f) + 0.5f * (openAmt * 0.5f);
  vowelTarget = vowelTarget * 0.70f + constrain(melosTonguePos + melosSpeechTongue, 0.0f, 1.0f) * 0.30f;
  melosVowelPos += (vowelTarget - melosVowelPos) * (MELOS_VOWEL_EASE * blk);

  float f1, f2, f3;
  melosFormantTargets(melosVowelPos, f1, f2, f3);
  float oralDamping = 0.0f;
  float fricativeMix = 0.0f;
  melosApplyArticulation(f1, f2, f3, oralDamping, fricativeMix);
  const float constr = constrain(melosLogosConstrict + melosConstrictAuto +
                                melosConstrBack * 0.28f + melosConstrMid * 0.36f + melosConstrFront * 0.44f, 0.0f, 1.0f);
  f3 += constr * 750.0f;
  melosFormantFreq[0] += (f1 - melosFormantFreq[0]) * (MELOS_VOWEL_EASE * blk);
  melosFormantFreq[1] += (f2 - melosFormantFreq[1]) * (MELOS_VOWEL_EASE * blk);
  melosFormantFreq[2] += (f3 - melosFormantFreq[2]) * (MELOS_VOWEL_EASE * blk);
  melosOralDamping += (oralDamping - melosOralDamping) * (MELOS_ARTICULATION_EASE * blk);
  melosFricativeMix += (fricativeMix - melosFricativeMix) * (MELOS_ARTICULATION_EASE * blk);
  melosComputeFormantCoeffs();
  const float previousObstruction = melosTractObstruction;
  melosTractObstruction += (constr - melosTractObstruction) * (MELOS_ARTICULATION_EASE * blk);
  if (previousObstruction > 0.58f && constr < 0.34f && melosNasal < 0.72f) {
    melosTractTransient = constrain(melosTractTransient + (previousObstruction - constr) * MELOS_TRACT_TRANSIENT_GAIN,
                                    0.0f, 0.26f);
  }
  melosUpdateMovingTract(constr);

  // Tone vs breath.
  const float tenseTarget = constrain(melosVoicedRatio * (1.0f - melosLogosDensity * 0.18f) +
                                      melosLogosStabilize * 0.26f + melosSpeechGhost * 0.12f,
                                      0.18f, 0.99f);
  melosTenseness += (tenseTarget - melosTenseness) * (MELOS_PARAM_EASE * blk);
  const float aspTarget = constrain((1.0f - melosTenseness) * 0.45f + melosLogosDensity * 0.32f +
                                    constr * 0.34f + melosLearnBreath * melosLearnAmount * 0.42f,
                                    0.0f, 1.05f);
  melosAspiration += (aspTarget - melosAspiration) * (MELOS_PARAM_EASE * blk);

  // Breathing arc: gentle, narrow range, no near-silence dips.
  if (melosSampleCounter >= melosBreathNextSample) {
    const float r2 = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
    melosBreathArcTarget = 0.66f + 0.2f * r2;  // ~0.66..0.86
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
  melosTractFreq[0] = 520.0f;
  melosTractFreq[1] = 1180.0f;
  melosTractFreq[2] = 2400.0f;
  melosComputeTractCoeffs();
  for (uint8_t k = 0; k < MELOS_TRACT_SECTIONS; ++k) {
    melosTractS1[k] = 0.0f;
    melosTractS2[k] = 0.0f;
  }

  for (uint8_t v = 0; v < MELOS_VOICE_COUNT; ++v) {
    melosVoicePhase[v] = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
    melosVoiceJitter[v] = (static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f * 2.0f - 1.0f) * MELOS_DETUNE_CENTS;
    melosVoiceOffset[v] = 0.0f;
    melosVoiceOffsetTarget[v] = 0.0f;
    melosVoiceDrift[v] = 0.0f;
    melosVoiceF0[v] = melosF0;
    // Sub-choirs: half the voices darker, half brighter (a crowd of different mouths).
    melosVoiceBright[v] = (v < MELOS_VOICE_COUNT / 2) ? -0.22f : 0.22f;
  }

  // Nasal resonator (fixed pole, fixed Q).
  {
    const float g = tanf(PI * MELOS_NASAL_FREQ / SAMPLE_RATE);
    const float kq = 1.0f / MELOS_NASAL_Q;
    const float a1 = 1.0f / (1.0f + g * (g + kq));
    melosNasalA1 = a1;
    melosNasalA2 = g * a1;
    melosNasalA3 = g * melosNasalA2;
  }
  melosNasalS1 = 0.0f;
  melosNasalS2 = 0.0f;
  melosNasal = 0.2f;
  melosNasalTarget = 0.2f;
  melosConstrictAuto = 0.0f;
  melosConstrictAutoTarget = 0.0f;
  melosConstrictNextSample = melosRandomSamples(2500, 11000);
  melosVowelWander = 0.0f;
  melosTonguePos = melosVowelPosition();
  melosTongueTarget = melosTonguePos;
  melosTongueDiameter = 0.65f;
  melosTongueDiameterTarget = 0.65f;
  melosConstrBack = 0.0f;
  melosConstrMid = 0.0f;
  melosConstrFront = 0.0f;
  melosConstrBackTarget = 0.0f;
  melosConstrMidTarget = 0.0f;
  melosConstrFrontTarget = 0.0f;
  melosOralDamping = 0.0f;
  melosFricativeMix = 0.0f;
  melosTractObstruction = 0.0f;
  melosTractTransient = 0.0f;
  melosSpeechGhost = 0.0f;
  melosSpeechGhostTarget = 0.0f;
  melosSpeechPhase = static_cast<float>(nextRandomU32() & 0xFFFFUL) / 65535.0f;
  melosSpeechRateHz = 5.5f;
  melosSpeechReleaseSample = 0;
  melosNextMurmurSample = melosRandomSamples(MELOS_MURMUR_MIN_MS, MELOS_MURMUR_MAX_MS);
  melosSpeechTongue = 0.0f;
  melosSpeechDiameter = 0.0f;
  melosSpeechBack = 0.0f;
  melosSpeechMid = 0.0f;
  melosSpeechFront = 0.0f;

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

    // Sub-choir: each voice gets a brightness bias, so the crowd has varied mouths.
    const float tenseV = constrain(melosTenseness + melosVoiceBright[v], 0.05f, 0.98f);
    float buzz = 0.0f;
    for (uint16_t n = 1; n <= MELOS_SOURCE_HARMONICS; ++n) {
      const float f = static_cast<float>(n) * vf0;
      if (f >= MELOS_PARTIAL_MAX_HZ) {
        break;
      }
      // Tense -> ~1/n (bright sawtooth glottis); lax -> ~1/n^2 (dark, breathy).
      const float harmonic = static_cast<float>(n);
      const float amp = (0.16f + tenseV * 0.84f + (1.0f - tenseV) / harmonic) / harmonic;
      buzz += sineLookup(melosVoicePhase[v] * static_cast<float>(n)) * amp;
    }
    source += buzz;
  }
  source /= static_cast<float>(MELOS_VOICE_COUNT);

  // Aspiration noise shares the vocal tract -> whispered vowels when breathy.
  const float noise = nextRandomBipolar();
  const float fricativeNoise = nextRandomBipolar() * melosFricativeMix * MELOS_FRONT_FRICATIVE_GAIN;
  const float excitation = source * melosTenseness + noise * melosAspiration * 0.36f + fricativeNoise;

  // Three parallel formant resonators (TPT state-variable bandpass) = the mouth.
  static const float formantGain[3] = {1.12f, 0.82f, 0.52f};
  float formantOut = 0.0f;
  for (uint8_t k = 0; k < 3; ++k) {
    const float v3 = excitation - melosFormantS2[k];
    const float v1 = melosFormantA1[k] * melosFormantS1[k] + melosFormantA2[k] * v3;
    const float v2 = melosFormantS2[k] + melosFormantA2[k] * melosFormantS1[k] + melosFormantA3[k] * v3;
    melosFormantS1[k] = 2.0f * v1 - melosFormantS1[k];
    melosFormantS2[k] = 2.0f * v2 - melosFormantS2[k];
    formantOut += v1 * formantGain[k];  // bandpass output
  }

  // Nasal branch: a low murmur pole; nasality damps the oral mouth -> "mmm/nnng".
  const float nv3 = excitation - melosNasalS2;
  const float nv1 = melosNasalA1 * melosNasalS1 + melosNasalA2 * nv3;
  const float nv2 = melosNasalS2 + melosNasalA2 * melosNasalS1 + melosNasalA3 * nv3;
  melosNasalS1 = 2.0f * nv1 - melosNasalS1;
  melosNasalS2 = 2.0f * nv2 - melosNasalS2;
  const float mouthOut = formantOut * (1.0f - 0.6f * melosNasal) * (1.0f - melosOralDamping) +
                         nv1 * 2.0f * melosNasal;
  const float releaseTransient = melosTractTransient *
                                 (0.58f * nextRandomBipolar() + 0.42f * (1.0f - melosOralDamping));
  melosTractTransient *= MELOS_TRACT_TRANSIENT_DECAY;
  const float tractOut = melosApplyMovingTract(mouthOut + releaseTransient);

  // Slow archive envelope shaping + breathing arc.
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
  const float envShape = 0.85f + 0.15f * envVal;  // less amplitude movement
  // Sidechain ducking removed: MELOS amplitude no longer pumps with LOGOS.
  const float amp = melosBreathArc * envShape;

  melosSampleCounter++;
  const float melosOut = softClip(tractOut * amp * MELOS_GAIN * MELOS_MAKEUP);
  // DC blocker (belt-and-suspenders; the formant bandpass already removes DC).
  const float melosDc = melosOut - melosDcX1 + DC_BLOCK_R * melosDcY1;
  melosDcX1 = melosOut;
  melosDcY1 = melosDc;
  return melosDc;
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
