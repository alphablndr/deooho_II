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
  if (state == RECORDING && synthPaused) {
    return false;
  }
  return (state == IDLE || state == RECORDING || state == PROCESSING) && !diagnosticToneOn;
}

void workTick() {
  updateStatusLed();
  servicePlaybackDuringWork();
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
    if (recordSkipBytesLeft > 0) {
      // Start of the take: the cube is still fading out and the press click
      // may ring through the shell. Drop these blocks.
      recordSkipBytesLeft = recordSkipBytesLeft > 256 ? recordSkipBytesLeft - 256 : 0;
    } else if (recordBytesUsed + 256 <= activeRecordCapacity) {
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

void setOutputLevel(float target, float seconds) {
  masterL.fadeTo(target, seconds);
  masterR.fadeTo(target, seconds);
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

void startRecording() {
  if (state != IDLE) {
    return;
  }
  recordBytesUsed = 0;
  recordSkipBytesLeft = RECORD_TRIM_START_MS * (SAMPLE_RATE / 1000UL) * 2UL;
  synthPaused = false;
  synthPauseByte = 0;
  recordingStartedMs = millis();
  recordOverflow = false;
  AudioProcessorUsageMaxReset();
  AudioMemoryUsageMaxReset();
  setOutputLevel(0.0f, OUTPUT_DUCK_SECONDS);  // the cube falls silent to listen
  recordQueue.begin();
  enterState(RECORDING);
  Serial.print("Recording to ");
  Serial.print(usingPsramBuffer ? "PSRAM" : "internal RAM fallback");
  Serial.print(", max ");
  Serial.print(activeRecordSeconds);
  Serial.println(" sec.");
}

// The Teensy's per-audio-block CPU bursts leak into the analog input as a tone
// locked to the 128-sample block: 344.5 Hz plus a full harmonic series (SD
// card analysis 2026-10-13). It repeats exactly every block in the buffer, so
// averaging all blocks gives its waveform (speech averages out) and we subtract
// it. This also removes DC. Takes start on a block boundary, so sample i sits
// at block position i % AUDIO_BLOCK_SAMPLES.
// Only the quieter half of the blocks builds the template: in a short loud
// take the voice does not average out and would be subtracted back as a buzz
// (W0013, 2026-10-14: -39 dBFS left in the pauses; quiet half: -70 dBFS).
// firstSample must sit on a block boundary.
void removeHumInRange(uint32_t firstSample, uint32_t endSample) {
  int16_t *samples = reinterpret_cast<int16_t *>(activeRecordBuffer);
  uint32_t blocks = (endSample - firstSample) / AUDIO_BLOCK_SAMPLES;
  if (blocks > MAX_TAKE_BLOCKS) {
    blocks = MAX_TAKE_BLOCKS;
  }
  if (blocks < 8) {
    return;
  }

  for (uint32_t b = 0; b < blocks; ++b) {
    const int16_t *block = samples + firstSample + b * AUDIO_BLOCK_SAMPLES;
    float energy = 0.0f;
    for (uint16_t j = 0; j < AUDIO_BLOCK_SAMPLES; ++j) {
      energy += static_cast<float>(block[j]) * block[j];
    }
    humBlockEnergy[b] = energy;
    humEnergySorted[b] = energy;
    if ((b & 0xFFUL) == 0) {
      workTick();
    }
  }
  float *median = humEnergySorted + blocks / 2;
  std::nth_element(humEnergySorted, median, humEnergySorted + blocks);
  const float quietLimit = *median;

  int32_t sum[AUDIO_BLOCK_SAMPLES] = {0};  // 30 s max: 10336 blocks * 32767 fits int32
  int32_t used = 0;
  for (uint32_t b = 0; b < blocks; ++b) {
    if (humBlockEnergy[b] > quietLimit) {
      continue;
    }
    const int16_t *block = samples + firstSample + b * AUDIO_BLOCK_SAMPLES;
    for (uint16_t j = 0; j < AUDIO_BLOCK_SAMPLES; ++j) {
      sum[j] += block[j];
    }
    ++used;
    if ((b & 0xFFUL) == 0) {
      workTick();
    }
  }
  if (used == 0) {
    return;
  }

  int16_t hum[AUDIO_BLOCK_SAMPLES];
  double humEnergy = 0.0;
  for (uint16_t j = 0; j < AUDIO_BLOCK_SAMPLES; ++j) {
    hum[j] = static_cast<int16_t>(sum[j] / used);
    humEnergy += static_cast<double>(hum[j]) * hum[j];
  }

  for (uint32_t i = firstSample; i < endSample; ++i) {
    const int32_t v = static_cast<int32_t>(samples[i]) - hum[i % AUDIO_BLOCK_SAMPLES];
    samples[i] = static_cast<int16_t>(constrain(v, -32768L, 32767L));
    if ((i & 0x7FFFUL) == 0) {
      workTick();
    }
  }

  Serial.print("Block-rate hum removed: rms=");
  Serial.print(sqrtf(humEnergy / AUDIO_BLOCK_SAMPLES) / 32768.0f, 5);
  Serial.print(" over ");
  Serial.print((endSample - firstSample) * 1000UL / SAMPLE_RATE);
  Serial.println(" ms");
}

// The hum is stronger while synthesis still runs (during the duck fade), so the
// take is cleaned as two parts, each with its own hum waveform.
void removeBlockRateHum() {
  const uint32_t totalSamples = recordBytesUsed / 2;
  uint32_t split = static_cast<uint32_t>(synthPauseByte / 2);
  split -= split % AUDIO_BLOCK_SAMPLES;
  const uint32_t minPart = 8UL * AUDIO_BLOCK_SAMPLES;
  if (split < minPart || split + minPart > totalSamples) {
    removeHumInRange(0, totalSamples);
  } else {
    removeHumInRange(0, split);
    removeHumInRange(split, totalSamples);
  }
}

bool processRecording() {
  enterState(PROCESSING);

  if (recordBytesUsed == 0) {
    Serial.println("Empty recording ignored.");
    return true;
  }
  removeBlockRateHum();
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
  setOutputLevel(1.0f, OUTPUT_RETURN_SECONDS);

  // The release click is recorded before the debounce confirms the release.
  const size_t endTrimBytes = RECORD_TRIM_END_MS * (SAMPLE_RATE / 1000UL) * 2UL;
  recordBytesUsed = recordBytesUsed > endTrimBytes ? recordBytesUsed - endTrimBytes : 0;
  const uint32_t keptMs = static_cast<uint32_t>((recordBytesUsed / 2UL) * 1000UL / SAMPLE_RATE);
  Serial.print("Recording stopped: ");
  Serial.print(recordBytesUsed);
  Serial.print("/");
  Serial.print(activeRecordCapacity);
  Serial.print(recordOverflow ? " bytes, overflow clipped" : " bytes");
  Serial.print(" (");
  Serial.print(keptMs);
  Serial.println(" ms after trim).");

  if (keptMs < RECORD_MIN_SAVE_MS) {
    Serial.println("Take too short; not saved.");
    enterState(IDLE);
    return;
  }

  if (!processRecording()) {
    enterState(ERROR_STATE);
    return;
  }

  enterState(IDLE);
  grainCount = countGrains();
}
