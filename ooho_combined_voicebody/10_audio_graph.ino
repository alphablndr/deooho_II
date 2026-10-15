// ---------------- Mastering: per-channel EQ (biquads) + soft limiter ----------------
struct MasterBiquad {
  float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
  float z1 = 0.0f, z2 = 0.0f;  // transposed direct form II state
  inline float process(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
  void setHighpass(float fc, float q) {
    const float w0 = 2.0f * PI * fc / SAMPLE_RATE;
    const float c = cosf(w0), s = sinf(w0);
    const float alpha = s / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0 = (1.0f + c) * 0.5f / a0;
    b1 = -(1.0f + c) / a0;
    b2 = (1.0f + c) * 0.5f / a0;
    a1 = (-2.0f * c) / a0;
    a2 = (1.0f - alpha) / a0;
  }
  void setLowpass(float fc, float q) {
    const float w0 = 2.0f * PI * fc / SAMPLE_RATE;
    const float c = cosf(w0), s = sinf(w0);
    const float alpha = s / (2.0f * q);
    const float a0 = 1.0f + alpha;
    b0 = (1.0f - c) * 0.5f / a0;
    b1 = (1.0f - c) / a0;
    b2 = (1.0f - c) * 0.5f / a0;
    a1 = (-2.0f * c) / a0;
    a2 = (1.0f - alpha) / a0;
  }
  void setLowShelf(float fc, float dB) {  // RBJ cookbook, shelf slope 1
    const float A = powf(10.0f, dB / 40.0f);
    const float w0 = 2.0f * PI * fc / SAMPLE_RATE;
    const float c = cosf(w0), s = sinf(w0);
    const float alpha2 = 2.0f * sqrtf(A) * (s * 0.5f * sqrtf(2.0f));
    const float a0 = (A + 1.0f) + (A - 1.0f) * c + alpha2;
    b0 = A * ((A + 1.0f) - (A - 1.0f) * c + alpha2) / a0;
    b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * c) / a0;
    b2 = A * ((A + 1.0f) - (A - 1.0f) * c - alpha2) / a0;
    a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * c) / a0;
    a2 = ((A + 1.0f) + (A - 1.0f) * c - alpha2) / a0;
  }
  void setPeaking(float fc, float q, float dB) {
    const float A = powf(10.0f, dB / 40.0f);
    const float w0 = 2.0f * PI * fc / SAMPLE_RATE;
    const float c = cosf(w0), s = sinf(w0);
    const float alpha = s / (2.0f * q);
    const float a0 = 1.0f + alpha / A;
    b0 = (1.0f + alpha * A) / a0;
    b1 = (-2.0f * c) / a0;
    b2 = (1.0f - alpha * A) / a0;
    a1 = (-2.0f * c) / a0;
    a2 = (1.0f - alpha / A) / a0;
  }
};

class AudioMaster : public AudioStream {
public:
  AudioMaster() : AudioStream(1, inputQueueArray) {}
  MasterBiquad stage[4];
  uint8_t stages = 0;
  float ceiling = 0.9f;
  float inputGain = 1.0f;        // mastering: level into the chain
  // Gentle compressor between EQ and limiter (off unless configured):
  // peak follower 10 ms attack / 300 ms release, smooth gain.
  bool compOn = false;
  float compThreshold = 0.1f;    // linear
  float compRatio = 2.5f;
  float compMakeup = 1.0f;
  float compEnv = 0.0f;
  float compGain = 1.0f;
  float limEnv = 0.0f;
  float limGain = 1.0f;
  // Output fade, set from loop() via fadeTo(). Applied per sample at the very
  // end of the chain, so it acts at once (no play-queue latency) and without
  // clicks. Used to silence the cube while it records.
  volatile float levelTarget = 1.0f;
  volatile float levelStep = 1.0f;
  float level = 1.0f;
  void fadeTo(float target, float seconds) {
    levelStep = seconds > 0.0f ? 1.0f / (seconds * SAMPLE_RATE) : 1.0f;
    levelTarget = target;
  }
  virtual void update(void) {
    audio_block_t *block = receiveWritable(0);
    if (!block) {
      return;
    }
    const float target = levelTarget;
    const float step = levelStep;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
      float x = block->data[i] * (1.0f / 32768.0f) * inputGain;
      for (uint8_t s = 0; s < stages; ++s) {
        x = stage[s].process(x);
      }
      if (compOn) {
        const float a = fabsf(x);
        compEnv += (a - compEnv) * (a > compEnv ? 0.00227f : 0.0000756f);
        const float g = compEnv > compThreshold ? powf(compEnv / compThreshold, 1.0f / compRatio - 1.0f) : 1.0f;
        compGain += (g - compGain) * 0.01f;
        x *= compGain * compMakeup;
      }
      // Soft limiter: peak-follow gain reduction, fast attack / slow release.
      const float a = fabsf(x);
      limEnv += (a - limEnv) * (a > limEnv ? 0.30f : 0.0010f);
      const float g = limEnv > ceiling ? ceiling / limEnv : 1.0f;
      limGain += (g - limGain) * (g < limGain ? 0.5f : 0.004f);
      x *= limGain;
      if (level < target) {
        level = min(level + step, target);
      } else if (level > target) {
        level = max(level - step, target);
      }
      x *= level * level * (3.0f - 2.0f * level);  // S-curve: soft start and end both ways
      if (x > 1.0f) {
        x = 1.0f;
      } else if (x < -1.0f) {
        x = -1.0f;
      }
      block->data[i] = static_cast<int16_t>(x * 32767.0f);
    }
    transmit(block);
    release(block);
  }
private:
  audio_block_t *inputQueueArray[1];
};

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
AudioMaster masterL;   // LOGOS -> internal speaker
AudioMaster masterR;   // MELOS -> surface exciter
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
AudioConnection patchCord10(leftMixer, 0, masterL, 0);
AudioConnection patchCord11(rightMixer, 0, masterR, 0);
AudioConnection patchCord12(masterL, 0, i2sOut, 0);
AudioConnection patchCord13(masterR, 0, i2sOut, 1);
AudioControlSGTL5000 audioShield;


// ---------------- Global runtime state ----------------

DeviceState state = IDLE;
DebouncedButton recordButton{RECORD_BUTTON_PIN, "record"};
Model model;
uint16_t nextIndex = 1;
uint32_t grainCount = 0;
LogosPendingResponse logosResponse;
uint32_t recentGrainIndexes[LOGOS_RECENT_HISTORY];
uint8_t recentGrainWrite = 0;
uint8_t recentGrainUsed = 0;
uint8_t logosDensityMode = LOGOS_MODE_PRESENT;
uint32_t logosDensitySwitchMs = 0;
float logosActivity = LOGOS_SLEEP_LEVEL;  // how awake the crowd is (0..1)
uint32_t logosActivityUpdatedMs = 0;
uint32_t logosStormUntilMs = 0;  // short storm after a take runs until this time
float logosDensityGainCur = 1.0f;  // smoothed bus gain, eased toward the mode target

char currentFilename[13] = "W0000.WAV";
char tempFilename[13] = "T0000.WAV";
DMAMEM uint8_t internalRecordBuffer[INTERNAL_RECORD_BYTES];
EXTMEM uint8_t psramRecordBuffer[FIELD_RECORD_BYTES];
EXTMEM int16_t logosGrainBuffers[LOGOS_STREAM_COUNT][LOGOS_MAX_GRAIN_SAMPLES];
EXTMEM GrainEntry grainIndex[GRAIN_INDEX_CAPACITY];              // ~240 KB
EXTMEM RecordingEntry recordingIndex[RECORDING_INDEX_CAPACITY];  // ~80 KB
uint32_t grainIndexCount = 0;
uint32_t recordingIndexCount = 0;
uint8_t sdFailureStreak = 0;     // SD failures in a row (reset by a clean take)
uint32_t lastSdRetryMs = 0;      // no card at boot: last SD.begin() attempt
EXTMEM float humBlockEnergy[MAX_TAKE_BLOCKS];   // per-block loudness for hum removal
EXTMEM float humEnergySorted[MAX_TAKE_BLOCKS];  // scratch copy for the median
uint8_t *activeRecordBuffer = internalRecordBuffer;
size_t activeRecordCapacity = INTERNAL_RECORD_BYTES;
uint32_t activeRecordSeconds = INTERNAL_RECORD_SECONDS;
size_t recordBytesUsed = 0;
size_t recordSkipBytesLeft = 0;  // start-of-take bytes still to discard
bool synthPaused = false;        // synthesis paused for the rest of this take
size_t synthPauseByte = 0;       // take offset where synthesis paused (hum changes there)
uint32_t grainSelectLastUs = 0;  // time spent choosing the last LOGOS grain
uint32_t grainSelectMaxUs = 0;
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
float melosEnvPhase = 0.0f;
float melosF0 = DEFAULT_F0_HZ;
float melosVoicedRatio = DEFAULT_VOICED_RATIO;
float melosSmoothedEnv[ENV_BINS];
float melosVoicePhase[MELOS_VOICE_COUNT];
float melosLogosOpen = 0.0f;
float melosLogosConstrict = 0.0f;
float melosLogosStabilize = 0.0f;
float melosLogosDensity = 0.0f;
float melosLogosEnvelope = 0.0f;
float melosLearnAmount = 0.0f;
float melosLearnF0 = DEFAULT_F0_HZ;
uint32_t melosLearnStartMs = 0;
float melosLearnBreath = 0.0f;
uint8_t melosLearnBand = 2;
uint32_t melosSampleCounter = 0;
float melosSine[MELOS_SINE_SIZE];

// MELOS collective vocal body engine (source-formant-filter)
float melosVoiceF0[MELOS_VOICE_COUNT];
float melosArchivePitch[MELOS_PITCH_CAPACITY];      // real pitches of the archive's voiced takes
uint16_t melosArchivePitchCount = 0;
uint16_t melosArchivePitchWrite = 0;
float melosClusterLog[MELOS_VOICE_COUNT];           // current pitch per voice, log2 Hz
float melosClusterTarget[MELOS_VOICE_COUNT];        // next note (log2 Hz), taken while silent
bool melosClusterTargetEntry[MELOS_VOICE_COUNT];    // the next note is the new take's
uint8_t melosVoiceStage[MELOS_VOICE_COUNT];         // 0 sing, 1 fade out, 2 breath, 3 fade in, 4 wait to gather
float melosVoiceAmp[MELOS_VOICE_COUNT];             // 0..1, S-curved when applied
float melosVoiceFadeStep[MELOS_VOICE_COUNT];        // amp change per block
uint32_t melosVoiceStageMs[MELOS_VOICE_COUNT];
uint32_t melosClusterNextHopMs[MELOS_VOICE_COUNT];
bool melosClusterEntry[MELOS_VOICE_COUNT];          // holding the new take's pitch
float melosVoiceVowel[MELOS_VOICE_COUNT];           // own mouth: 0 oo .. 0.5 aa .. 1 ee
float melosVoiceVowelCenter[MELOS_VOICE_COUNT];
float melosVoiceVowelWander[MELOS_VOICE_COUNT];
uint32_t melosVoiceVowelHopMs[MELOS_VOICE_COUNT];
float melosVoiceBreath[MELOS_VOICE_COUNT];          // trace of air per voice
float melosVoiceSweepHz[MELOS_VOICE_COUNT];         // own vowel sweep rate
float melosVoiceSweepPhase[MELOS_VOICE_COUNT];
float melosVoiceFormantFreq[MELOS_VOICE_COUNT][3];
float melosVoiceFormantA1[MELOS_VOICE_COUNT][3];
float melosVoiceFormantA2[MELOS_VOICE_COUNT][3];
float melosVoiceFormantA3[MELOS_VOICE_COUNT][3];
float melosVoiceFormantS1[MELOS_VOICE_COUNT][3];
float melosVoiceFormantS2[MELOS_VOICE_COUNT][3];
float melosEntryVowel = 0.5f;                       // vowel of the newest take
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
float melosTractFreq[MELOS_TRACT_SECTIONS];        // eased moving tract resonators
float melosTractA1[MELOS_TRACT_SECTIONS];
float melosTractA2[MELOS_TRACT_SECTIONS];
float melosTractA3[MELOS_TRACT_SECTIONS];
float melosTractS1[MELOS_TRACT_SECTIONS];
float melosTractS2[MELOS_TRACT_SECTIONS];
float melosTractObstruction = 0.0f;                // smoothed constriction memory
float melosTractTransient = 0.0f;                  // release pop / click energy
float melosLfAlpha = 0.0f;                         // LF glottal waveform coefficients
float melosLfE0 = 0.0f;
float melosLfEpsilon = 1.0f;
float melosLfShift = 0.0f;
float melosLfDelta = 1.0f;
float melosLfTe = 0.6f;
float melosLfOmega = PI;
float melosTenseness = 0.6f;                      // glottal tone vs breath
float melosAspiration = 0.2f;                     // noise amount
float melosBreathArc = 0.4f;                      // breathing amplitude
float melosBreathArcTarget = 0.6f;
uint32_t melosBreathNextSample = 0;
float melosDcX1 = 0.0f;                            // MELOS DC blocker state
float melosDcY1 = 0.0f;
// Timbral movement state
float melosNasal = 0.2f;                           // 0 = open mouth, 1 = nasal hum
float melosNasalTarget = 0.2f;
float melosNasalA1 = 0.0f, melosNasalA2 = 0.0f, melosNasalA3 = 0.0f;
float melosNasalS1 = 0.0f, melosNasalS2 = 0.0f;
float melosConstrictAuto = 0.0f;                   // autonomous "almost-consonant" swells
float melosConstrictAutoTarget = 0.0f;
uint32_t melosConstrictNextSample = 0;
float melosVowelWander = 0.0f;                      // slow autonomous vowel drift
float melosVoiceBright[MELOS_VOICE_COUNT];          // sub-choir brightness bias per voice
// Pink-Trombone-inspired articulation state. These are not phonemes; they are
// slow physical biases that make the collective mouth respond to the archive.
float melosTonguePos = 0.45f;                       // 0=back/dark, 1=front/bright
float melosTongueTarget = 0.45f;
float melosTongueDiameter = 0.65f;                  // 1=open, 0=pinched
float melosTongueDiameterTarget = 0.65f;
float melosConstrBack = 0.0f;
float melosConstrMid = 0.0f;
float melosConstrFront = 0.0f;
float melosConstrBackTarget = 0.0f;
float melosConstrMidTarget = 0.0f;
float melosConstrFrontTarget = 0.0f;
float melosOralDamping = 0.0f;
float melosFricativeMix = 0.0f;
float melosSpeechGhost = 0.0f;                      // fast speech-motor memory, not speech
float melosSpeechGhostTarget = 0.0f;
float melosSpeechPhase = 0.0f;
float melosSpeechRateHz = 5.5f;
uint32_t melosSpeechReleaseSample = 0;
uint32_t melosNextMurmurSample = 0;
float melosSpeechTongue = 0.0f;
float melosSpeechDiameter = 0.0f;
float melosSpeechBack = 0.0f;
float melosSpeechMid = 0.0f;
float melosSpeechFront = 0.0f;
float melosSyllableAmount = 0.0f;
float melosSyllableTarget = 0.0f;
float melosSyllablePhase = 0.0f;
uint32_t melosSyllableStartSample = 0;
uint32_t melosSyllableDurationSamples = 0;
uint32_t melosNextSyllableSample = 0;
float melosSyllableTongue = 0.0f;
float melosSyllableDiameter = 0.0f;
float melosSyllableFront = 0.0f;
float melosSyllableMid = 0.0f;
float melosSyllableBack = 0.0f;

bool diagnosticToneOn = false;
bool melosSweep = false;
uint32_t melosSweepStartMs = 0;
uint32_t melosSweepLastPrintMs = 0;
uint32_t lastLedToggleMs = 0;
bool ledBlinkState = false;
uint32_t rngState = 0x12345678UL;
