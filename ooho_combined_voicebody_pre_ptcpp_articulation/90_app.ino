void servicePlaybackDuringWork() {
  if (!playbackServiceAllowed()) {
    return;
  }
  feedMelosQueue();
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    feedLogosQueue(i);
  }
}

void updateIdlePlayback() {
  if (!playbackServiceAllowed()) {
    return;
  }

  servicePlaybackDuringWork();
  serviceLogosResponses();

  const uint32_t now = millis();
  if (logosDensitySwitchMs == 0 || static_cast<int32_t>(now - logosDensitySwitchMs) >= 0) {
    chooseLogosDensityMode();
    for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
      if (logosDensityMode == LOGOS_MODE_SWARM && i > 0) {
        logosStreams[i].activeMode = true;
      }
      scheduleLogosStream(logosStreams[i], i);
    }
  }

  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    LogosStream &stream = logosStreams[i];

    if (i > 0 && logosDensityMode != LOGOS_MODE_SWARM && static_cast<int32_t>(now - stream.stateSwitchMs) >= 0) {
      stream.activeMode = !stream.activeMode;
      scheduleLogosStateSwitch(stream);
      scheduleLogosStream(stream, i);
    }

    // New grains only start while idle (a grain start does a bulk SD read).
    if (state == IDLE && grainCount > 0 && !stream.playing && static_cast<int32_t>(now - stream.nextEventMs) >= 0) {
      GrainInfo grain;
      if (selectRandomGrain(grain)) {
        startLogosGrain(i, grain, 0, true, LOGOS_LAUNCH_AUTO);
      } else {
        scheduleLogosStream(stream, i);
      }
    }
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
  Serial.print("  dominantBand: ");
  Serial.println(dominantBandIndex(model.bands));
  Serial.print("  learnAmount: ");
  Serial.println(melosLearnAmount, 4);
  Serial.print("  logosEnvelope: ");
  Serial.println(melosLogosEnvelope, 4);
  Serial.print("  tongue pos/diam: ");
  Serial.print(melosTonguePos, 3);
  Serial.print(" / ");
  Serial.println(melosTongueDiameter, 3);
  Serial.print("  constr back/mid/front: ");
  Serial.print(melosConstrBack, 3);
  Serial.print(" / ");
  Serial.print(melosConstrMid, 3);
  Serial.print(" / ");
  Serial.println(melosConstrFront, 3);
  Serial.print("  speechGhost/rate: ");
  Serial.print(melosSpeechGhost, 3);
  Serial.print(" / ");
  Serial.println(melosSpeechRateHz, 2);
  Serial.print("  tract Hz: ");
  Serial.print(melosTractFreq[0], 1);
  Serial.print(" / ");
  Serial.print(melosTractFreq[1], 1);
  Serial.print(" / ");
  Serial.println(melosTractFreq[2], 1);
}

void printGrains() {
  grainCount = countGrains();
  Serial.print("GRAINS count: ");
  Serial.println(grainCount);
  Serial.print("LOGOS density: ");
  Serial.println(logosDensityModeName());
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    const LogosStream &stream = logosStreams[i];
    const int32_t nextRemaining = static_cast<int32_t>(stream.nextEventMs - millis());
    Serial.print("LOGOS ");
    Serial.print(i + 1);
    Serial.print(stream.playing ? " playing, " : " idle, ");
    if (i == 0) {
      Serial.print("always, ");
    } else {
      Serial.print(stream.activeMode ? "active, " : "dormant, ");
    }
    Serial.print("next in ms: ");
    Serial.println(nextRemaining > 0 ? nextRemaining : 0);
  }
}

const char *stateName() {
  if (state == IDLE) {
    return "IDLE";
  }
  if (state == RECORDING) {
    return "RECORDING";
  }
  if (state == PROCESSING) {
    return "PROCESSING";
  }
  return "ERROR";
}

void printDiagnostics() {
  grainCount = countGrains();
  Serial.println("DIAGNOSTICS");
  Serial.print("  state: ");
  Serial.println(stateName());
  Serial.print("  audioCpuMax: ");
  Serial.print(AudioProcessorUsageMax());
  Serial.print("%, audioMemMax: ");
  Serial.println(AudioMemoryUsageMax());
  Serial.print("  recordBytes: ");
  Serial.print(recordBytesUsed);
  Serial.print("/");
  Serial.print(activeRecordCapacity);
  Serial.print(usingPsramBuffer ? " PSRAM" : " internal");
  Serial.println(recordOverflow ? " overflow" : "");
  Serial.print("  grains: ");
  Serial.print(grainCount);
  Serial.print(", archivePressure: ");
  Serial.print(logosArchivePressure());
  Serial.print("%");
  Serial.print(", nextWav: ");
  makeFilename(currentFilename, nextIndex);
  Serial.println(currentFilename);
  Serial.print("  logosDensity: ");
  Serial.print(logosDensityModeName());
  Serial.print(", activeStreams: ");
  Serial.println(activeLogosStreamCount());
  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    const LogosStream &stream = logosStreams[i];
    const int32_t nextRemaining = static_cast<int32_t>(stream.nextEventMs - millis());
    Serial.print("    L");
    Serial.print(i + 1);
    Serial.print(stream.playing ? " playing" : " idle");
    Serial.print(i == 0 ? " always" : (stream.activeMode ? " active" : " dormant"));
    Serial.print(", voices=");
    Serial.print(stream.voiceCount);
    Serial.print(", nextMs=");
    Serial.println(nextRemaining > 0 ? nextRemaining : 0);
  }
  Serial.print("  melos f0/voiced/rms: ");
  Serial.print(expf(model.meanLogF0), 2);
  Serial.print(" / ");
  Serial.print(model.voicedRatio, 3);
  Serial.print(" / ");
  Serial.println(model.meanRms, 5);
  Serial.print("  melos learn/logosEnv/density: ");
  Serial.print(melosLearnAmount, 3);
  Serial.print(" / ");
  Serial.print(melosLogosEnvelope, 3);
  Serial.print(" / ");
  Serial.println(melosLogosDensity, 3);
  Serial.print("  tongue pos/diam: ");
  Serial.print(melosTonguePos, 3);
  Serial.print(" / ");
  Serial.println(melosTongueDiameter, 3);
  Serial.print("  constr back/mid/front: ");
  Serial.print(melosConstrBack, 3);
  Serial.print(" / ");
  Serial.print(melosConstrMid, 3);
  Serial.print(" / ");
  Serial.println(melosConstrFront, 3);
  Serial.print("  speechGhost/rate: ");
  Serial.print(melosSpeechGhost, 3);
  Serial.print(" / ");
  Serial.println(melosSpeechRateHz, 2);
  Serial.print("  tract Hz: ");
  Serial.print(melosTractFreq[0], 1);
  Serial.print(" / ");
  Serial.print(melosTractFreq[1], 1);
  Serial.print(" / ");
  Serial.println(melosTractFreq[2], 1);
}

void playOneLogosNow() {
  if (state != IDLE || diagnosticToneOn || grainCount == 0 || logosStreams[0].playing) {
    Serial.println("LOGOS one-shot ignored.");
    return;
  }
  GrainInfo grain;
  if (selectRandomGrain(grain)) {
    startLogosGrain(0, grain, 0, true, LOGOS_LAUNCH_AUTO);
  } else {
    Serial.println("No valid LOGOS grain found.");
  }
}

void toggleDiagnosticTone() {
  if (state == RECORDING || state == PROCESSING) {
    Serial.println("Tone ignored while recording/processing.");
    return;
  }
  if (!diagnosticToneOn) {
    stopAllPlayback();
    diagnosticTone.frequency(440.0f);
    diagnosticTone.amplitude(0.12f);
    diagnosticToneOn = true;
    Serial.println("Diagnostic tone ON.");
  } else {
    diagnosticTone.amplitude(0.0f);
    diagnosticToneOn = false;
    initializeLogosSchedulers();
    melosQueuedFreshAudio = false;  // MELOS synth re-gates on next feed
    Serial.println("Diagnostic tone OFF.");
  }
}

void startMelosSweep() {
  if (state != IDLE) {
    Serial.println("Sweep ignored (busy).");
    return;
  }
  stopAllPlayback();
  diagnosticToneOn = true;        // pause LOGOS/MELOS during the sweep
  leftMixer.gain(3, 0.0f);        // tone only on the exciter (right) side
  rightMixer.gain(1, 0.7f);
  diagnosticTone.frequency(60.0f);
  diagnosticTone.amplitude(0.25f);
  melosSweep = true;
  melosSweepStartMs = millis();
  melosSweepLastPrintMs = 0;
  Serial.println("Exciter sweep 60 Hz -> 8 kHz over 30 s (right channel). Note ringing/booming Hz.");
}

void updateMelosSweep() {
  if (!melosSweep) {
    return;
  }
  const uint32_t now = millis();
  const float t = static_cast<float>(now - melosSweepStartMs) / 30000.0f;
  if (t >= 1.0f) {
    diagnosticTone.amplitude(0.0f);
    melosSweep = false;
    diagnosticToneOn = false;
    leftMixer.gain(3, 0.8f);      // restore default tone routing
    rightMixer.gain(1, 0.8f);
    melosQueuedFreshAudio = false;
    initializeLogosSchedulers();
    Serial.println("Sweep done.");
    return;
  }
  const float freq = 60.0f * powf(8000.0f / 60.0f, t);
  diagnosticTone.frequency(freq);
  if (now - melosSweepLastPrintMs >= 700) {
    melosSweepLastPrintMs = now;
    Serial.print("sweep Hz: ");
    Serial.println(freq, 0);
  }
}

void handleSerial() {
  while (Serial.available() > 0) {
    const char command = Serial.read();
    if (command == 'r' || command == 'R') {
      if (state == RECORDING) {
        stopRecording();
      } else if (state == IDLE) {
        startRecording();
      }
    } else if (command == 's' || command == 'S') {
      if (state == RECORDING) {
        stopRecording();
      } else {
        stopAllPlayback();
        if (state != ERROR_STATE) {
          enterState(IDLE);
        }
        Serial.println("Stopped playback.");
      }
    } else if (command == 't' || command == 'T') {
      toggleDiagnosticTone();
    } else if (command == 'm' || command == 'M') {
      printModel();
    } else if (command == 'g' || command == 'G') {
      printGrains();
    } else if (command == 'd' || command == 'D') {
      printDiagnostics();
    } else if (command == 'n' || command == 'N') {
      playOneLogosNow();
    } else if (command == 'e' || command == 'E') {
      Serial.println("MELOS is live-synthesized; nothing to render.");
    } else if (command == 'x' || command == 'X') {
      rebuildGrainsFromArchive();
    } else if (command == 'y' || command == 'Y') {
      startMelosSweep();
    }
  }
}

void handleRecordButton() {
  if (recordButton.pressedEdge && state == IDLE) {
    startRecording();
  }
  if (recordButton.releasedEdge && state == RECORDING) {
    stopRecording();
  }
}

void setup() {
  pinMode(RECORD_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  if (USE_EXTERNAL_STATUS_LED) {
    pinMode(STATUS_LED_PIN, OUTPUT);
  }
  setStatusLed(false);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho II - emergent build (Claude Code branch)");

  for (uint8_t i = 0; i < LOGOS_STREAM_COUNT; ++i) {
    logosStreams[i].buffer = logosGrainBuffers[i];
  }
  initSineTable();

  AudioMemory(AUDIO_MEMORY_BLOCKS);
  logosQueue1.setMaxBuffers(24);
  logosQueue2.setMaxBuffers(24);
  logosQueue3.setMaxBuffers(24);
  logosQueue4.setMaxBuffers(24);
  melosQueue.setMaxBuffers(32);
  // NON_STALLING is required: with ORIGINAL behaviour, available() reports pool
  // space (not queue space) and playBuffer() busy-waits when the queue ring is
  // full, so a "fill until available()" loop never returns. NON_STALLING makes
  // playBuffer() return 1 when the ring is full, bounding the feed loop.
  logosQueue1.setBehaviour(AudioPlayQueue::NON_STALLING);
  logosQueue2.setBehaviour(AudioPlayQueue::NON_STALLING);
  logosQueue3.setBehaviour(AudioPlayQueue::NON_STALLING);
  logosQueue4.setBehaviour(AudioPlayQueue::NON_STALLING);
  melosQueue.setBehaviour(AudioPlayQueue::NON_STALLING);
  audioShield.enable();
  audioShield.adcHighPassFilterDisable();
  audioShield.volume(STARTUP_VOLUME);
  logosMixer.gain(0, 0.0f);
  logosMixer.gain(1, 0.0f);
  logosMixer.gain(2, 0.0f);
  logosMixer.gain(3, 0.0f);
  leftMixer.gain(0, 1.0f);   // LOGOS crowd sum bus
  leftMixer.gain(1, 0.0f);
  leftMixer.gain(2, 0.0f);
  leftMixer.gain(3, 0.8f);   // diagnostic tone
  rightMixer.gain(0, 0.0f);  // MELOS synth (gated up on first fresh block)
  rightMixer.gain(1, 0.8f);  // diagnostic tone
  diagnosticTone.begin(WAVEFORM_SINE);
  diagnosticTone.amplitude(0.0f);

  // Mastering EQ per transducer + soft limiter.
  // LOGOS -> internal speaker: clear low mud, lift presence for word clarity.
  masterL.stage[0].setHighpass(90.0f, 0.707f);
  masterL.stage[1].setPeaking(3000.0f, 1.0f, 2.5f);
  masterL.stages = 2;
  masterL.ceiling = 0.92f;
  // MELOS -> 31 cm / 1.5 mm steel cube exciter. The metallic ring is WANTED
  // (the cube is the instrument), so we do NOT tame 2-6 kHz. Only a protective
  // high-pass below the panel fundamental (~130-180 Hz, where it can't radiate
  // anyway) + the limiter to keep the driver safe. Let the steel sing.
  masterR.stage[0].setHighpass(150.0f, 0.707f);
  masterR.stages = 1;
  masterR.ceiling = 0.90f;

  if (USE_MAX4466_LINEIN) {
    audioShield.inputSelect(AUDIO_INPUT_LINEIN);
    audioShield.lineInLevel(LINE_IN_LEVEL);
    Serial.println("Input: Audio Shield LINEIN_L for MAX4466 output.");
  } else {
    audioShield.inputSelect(AUDIO_INPUT_MIC);
    audioShield.micGain(20);
    Serial.println("Input: Audio Shield MIC for a raw electret capsule, not MAX4466.");
  }

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("SD init failed. Use the Teensy 4.1 built-in microSD slot.");
    enterState(ERROR_STATE);
    return;
  }

  if (external_psram_size > 0) {
    activeRecordBuffer = psramRecordBuffer;
    activeRecordCapacity = min(static_cast<size_t>(external_psram_size) * 1024UL * 1024UL, FIELD_RECORD_BYTES);
    activeRecordSeconds = activeRecordCapacity / (SAMPLE_RATE * 2UL);
    usingPsramBuffer = true;
  } else {
    Serial.println("Warning: PSRAM not detected; internal RAM fallback is for bench testing only.");
  }

  rngState ^= micros();
  rngState ^= static_cast<uint32_t>(analogRead(A0)) << 16;
  loadModel();
  initMelosSynth();
  nextIndex = loadCounter();
  grainCount = countGrains();

  Serial.print("Record buffer: ");
  Serial.print(activeRecordCapacity);
  Serial.print(" bytes, max ");
  Serial.print(activeRecordSeconds);
  Serial.println(usingPsramBuffer ? " sec in PSRAM." : " sec in internal RAM fallback.");
  Serial.print("Next recording: ");
  makeFilename(currentFilename, nextIndex);
  Serial.println(currentFilename);
  Serial.print("LOGOS grains: ");
  Serial.println(grainCount);
  Serial.println("Record button pin 32: hold to record, release to process.");
  Serial.println("Serial: r=record toggle, s=stop, t=tone, m=model, g=grains, d=diagnostics, n=LOGOS now, e=(no render), x=rebuild grains, y=exciter sweep.");
  Serial.println("MELOS is real-time synthesized; LOGOS is a 4-stream PSRAM crowd with reverse/rate.");

  enterState(IDLE);
  initializeLogosSchedulers();
}

void loop() {
  handleSerial();
  updateButton(recordButton);
  handleRecordButton();

  if (state == RECORDING) {
    drainRecordQueue();
    if (millis() - recordingStartedMs >= MAX_RECORD_MS) {
      Serial.println("Max recording time reached; auto-stopping recording.");
      stopRecording();
    } else if (recordBytesUsed >= activeRecordCapacity) {
      recordOverflow = true;
      stopRecording();
    }
  }

  updateMelosSweep();
  updateIdlePlayback();
  updateStatusLed();
}
