/*
  The Ooho Rev.1 bench firmware

  Hardware:
  - Teensy 4.1 + Teensy Audio Shield Rev D
  - MAX4466 mic preamp -> Audio Shield LINEIN_L
  - Audio Shield LINEOUT_L -> PAM8302 A+
  - Audio Shield GND -> PAM8302 A-
  - PAM8302 speaker output -> 4-8 ohm speaker or surface exciter
  - Momentary button between pin 32 and GND
  - Optional status LED on pin 31 through 220-1000 ohm resistor to GND

  Behavior:
  - Press button once to start recording W0001.WAV, W0002.WAV, ...
  - Press again to stop, save WAV, and return to idle
  - Serial commands: r = record/stop, p = play last, s = stop, t = diagnostic tone
*/

#include <Audio.h>
#include <SD.h>
#include <SerialFlash.h>
#include <SPI.h>
#include <Wire.h>

constexpr int BUTTON_PIN = 32;
constexpr int STATUS_LED_PIN = 31;
constexpr bool USE_EXTERNAL_STATUS_LED = true;
constexpr uint32_t DEBOUNCE_MS = 35;
constexpr uint32_t SAMPLE_RATE = 44100;
constexpr uint16_t AUDIO_MEMORY_BLOCKS = 200;
constexpr size_t SD_WRITE_CHUNK_BYTES = 4096;
constexpr float STARTUP_VOLUME = 0.15f;
constexpr uint8_t LINE_IN_LEVEL = 7;
constexpr bool USE_MAX4466_LINEIN = true;
constexpr uint32_t INTERNAL_RECORD_SECONDS = 3;
constexpr uint32_t PSRAM_RECORD_SECONDS = 60;
constexpr size_t INTERNAL_RECORD_BYTES = SAMPLE_RATE * 2UL * INTERNAL_RECORD_SECONDS;
constexpr size_t PSRAM_RECORD_BYTES = SAMPLE_RATE * 2UL * PSRAM_RECORD_SECONDS;

extern "C" uint8_t external_psram_size;

AudioInputI2S i2sIn;
AudioRecordQueue recordQueue;
AudioPlaySdWav playWav;
AudioSynthWaveform diagnosticTone;
AudioMixer4 outputMixer;
AudioOutputI2S i2sOut;
AudioConnection patchCord1(i2sIn, 0, recordQueue, 0);
AudioConnection patchCord2(playWav, 0, outputMixer, 0);
AudioConnection patchCord3(diagnosticTone, 0, outputMixer, 1);
AudioConnection patchCord4(outputMixer, 0, i2sOut, 0);
AudioConnection patchCord5(outputMixer, 0, i2sOut, 1);
AudioControlSGTL5000 audioShield;

enum DeviceState {
  IDLE,
  RECORDING,
  SAVING,
  PLAYING,
  ERROR_STATE
};

DeviceState state = IDLE;
FsFile recordingFile;
uint32_t audioBytesWritten = 0;
uint16_t nextIndex = 1;
char currentFilename[13] = "W0000.WAV";
char tempFilename[13] = "T0000.WAV";
char lastFilename[13] = "";
uint8_t sdWriteBuffer[SD_WRITE_CHUNK_BYTES];
size_t sdWriteBufferUsed = 0;
bool diagnosticToneOn = false;
bool autoPlaybackAfterRecord = false;
DMAMEM uint8_t internalRecordBuffer[INTERNAL_RECORD_BYTES];
EXTMEM uint8_t psramRecordBuffer[PSRAM_RECORD_BYTES];
uint8_t *activeRecordBuffer = internalRecordBuffer;
size_t activeRecordCapacity = INTERNAL_RECORD_BYTES;
uint32_t activeRecordSeconds = INTERNAL_RECORD_SECONDS;
size_t recordBytesUsed = 0;
bool recordOverflow = false;
bool usingPsramBuffer = false;

bool lastRawButton = HIGH;
bool stableButton = HIGH;
uint32_t lastButtonChangeMs = 0;
bool ledOutputState = false;
uint32_t lastLedChangeMs = 0;

void writeStatusLed(bool on) {
  ledOutputState = on;
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
  if (USE_EXTERNAL_STATUS_LED) {
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
  }
}

void updateStatusLed() {
  writeStatusLed(state == RECORDING);
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

void writeWavHeader(FsFile &file, uint32_t dataBytes) {
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

  file.write(header, sizeof(header));
}

void finalizeWavHeader(FsFile &file, uint32_t dataBytes) {
  const uint32_t finalSize = 44 + dataBytes;
  if (!file.truncate(finalSize)) {
    Serial.println("Warning: could not truncate preallocated WAV to final size.");
  }

  file.seekSet(0);
  writeWavHeader(file, dataBytes);
  file.sync();
}

void makeFilename(char *buffer, uint16_t index) {
  snprintf(buffer, 13, "W%04u.WAV", index);
}

void makeTempFilename(char *buffer, uint16_t index) {
  snprintf(buffer, 13, "T%04u.WAV", index);
}

uint16_t findNextIndex() {
  char name[13];
  for (uint16_t i = 1; i < 10000; ++i) {
    makeFilename(name, i);
    if (!SD.exists(name)) {
      return i;
    }
  }
  return 9999;
}

bool buttonPressedEdge() {
  const bool raw = digitalRead(BUTTON_PIN);
  const uint32_t now = millis();

  if (raw != lastRawButton) {
    lastRawButton = raw;
    lastButtonChangeMs = now;
  }

  if ((now - lastButtonChangeMs) > DEBOUNCE_MS && raw != stableButton) {
    stableButton = raw;
    return stableButton == LOW;
  }

  return false;
}

void drainRecordQueue(bool drainAll) {
  while (recordQueue.available() > 0) {
    const uint8_t *block = reinterpret_cast<const uint8_t *>(recordQueue.readBuffer());
    if (recordBytesUsed + 256 <= activeRecordCapacity) {
      memcpy(activeRecordBuffer + recordBytesUsed, block, 256);
      recordBytesUsed += 256;
    } else {
      recordOverflow = true;
    }
    recordQueue.freeBuffer();
  }
}

bool saveBufferRecordingToSd() {
  makeFilename(currentFilename, nextIndex);
  makeTempFilename(tempFilename, nextIndex);

  if (SD.exists(tempFilename)) {
    SD.remove(tempFilename);
  }
  if (SD.exists(currentFilename)) {
    SD.remove(currentFilename);
  }

  recordingFile = SD.sdfs.open(tempFilename, O_RDWR | O_CREAT | O_TRUNC);
  if (!recordingFile) {
    Serial.print("Could not open ");
    Serial.println(tempFilename);
    state = ERROR_STATE;
    return false;
  }

  const uint32_t wavBytes = 44 + recordBytesUsed;
  if (!recordingFile.preAllocate(wavBytes)) {
    Serial.println("Warning: preAllocate failed. Continuing, but SD latency risk is higher.");
  }

  recordingFile.seekSet(0);
  writeWavHeader(recordingFile, recordBytesUsed);

  size_t written = 0;
  while (written < recordBytesUsed) {
    const size_t chunk = min(SD_WRITE_CHUNK_BYTES, recordBytesUsed - written);
    recordingFile.write(activeRecordBuffer + written, chunk);
    written += chunk;
  }

  finalizeWavHeader(recordingFile, recordBytesUsed);
  recordingFile.close();

  if (!SD.rename(tempFilename, currentFilename)) {
    Serial.print("Could not rename ");
    Serial.print(tempFilename);
    Serial.print(" to ");
    Serial.println(currentFilename);
    state = ERROR_STATE;
    return false;
  }

  return true;
}

void startPlayback(const char *filename) {
  if (!filename[0]) {
    Serial.println("No recording to play yet.");
    state = IDLE;
    return;
  }

  if (playWav.isPlaying()) {
    playWav.stop();
    delay(5);
  }

  Serial.print("Playing ");
  Serial.println(filename);
  playWav.play(filename);
  delay(10);
  state = PLAYING;
}

void stopPlayback() {
  if (playWav.isPlaying()) {
    playWav.stop();
  }
  diagnosticTone.amplitude(0.0f);
  diagnosticToneOn = false;
  state = IDLE;
  Serial.println("Playback stopped.");
}

void toggleDiagnosticTone() {
  if (state == RECORDING) {
    Serial.println("Stop recording before tone test.");
    return;
  }

  if (playWav.isPlaying()) {
    playWav.stop();
    delay(5);
  }

  diagnosticToneOn = !diagnosticToneOn;
  if (diagnosticToneOn) {
    diagnosticTone.frequency(440.0f);
    diagnosticTone.amplitude(0.12f);
    state = PLAYING;
    Serial.println("Diagnostic tone ON. If this clicks, suspect amp/power/ground/output wiring.");
  } else {
    diagnosticTone.amplitude(0.0f);
    state = IDLE;
    Serial.println("Diagnostic tone OFF.");
  }
}

void startRecording() {
  if (playWav.isPlaying()) {
    playWav.stop();
    delay(5);
  }
  diagnosticTone.amplitude(0.0f);
  diagnosticToneOn = false;

  audioBytesWritten = 0;
  sdWriteBufferUsed = 0;
  recordBytesUsed = 0;
  recordOverflow = false;

  recordQueue.begin();
  state = RECORDING;

  Serial.print("Recording to ");
  Serial.print(usingPsramBuffer ? "PSRAM" : "internal RAM");
  Serial.print(" buffer, max ");
  Serial.print(activeRecordSeconds);
  Serial.println(" sec.");
}

void stopRecording() {
  recordQueue.end();
  drainRecordQueue(true);
  audioBytesWritten = recordBytesUsed;
  state = SAVING;
  updateStatusLed();

  if (!saveBufferRecordingToSd()) {
    return;
  }

  strncpy(lastFilename, currentFilename, sizeof(lastFilename));
  lastFilename[sizeof(lastFilename) - 1] = '\0';
  nextIndex++;

  Serial.print("Saved ");
  Serial.print(lastFilename);
  Serial.print(" (");
  Serial.print(audioBytesWritten);
  Serial.println(" bytes audio)");
  Serial.print("RAM-first mode: ");
  Serial.print(recordBytesUsed);
  Serial.print("/");
  Serial.print(activeRecordCapacity);
  Serial.print(usingPsramBuffer ? " bytes in PSRAM" : " bytes in internal RAM");
  if (recordOverflow) {
    Serial.println(", OVERFLOW clipped.");
  } else {
    Serial.println(".");
  }
  if (usingPsramBuffer) {
    Serial.print("PSRAM detected: ");
    Serial.print(external_psram_size);
    Serial.println(" MB.");
  }
  Serial.print("Audio CPU max: ");
  Serial.print(AudioProcessorUsageMax());
  Serial.print("%, memory max blocks: ");
  Serial.println(AudioMemoryUsageMax());

  state = IDLE;
  updateStatusLed();

  if (autoPlaybackAfterRecord) {
    startPlayback(lastFilename);
  }
}

void handleTrigger() {
  if (state == RECORDING) {
    stopRecording();
  } else {
    startRecording();
  }
}

void handleSerial() {
  while (Serial.available() > 0) {
    const char command = Serial.read();
    if (command == 'r' || command == 'R') {
      handleTrigger();
    } else if (command == 'p' || command == 'P') {
      startPlayback(lastFilename);
    } else if (command == 's' || command == 'S') {
      if (state == RECORDING) {
        stopRecording();
      } else {
        stopPlayback();
      }
    } else if (command == 't' || command == 'T') {
      toggleDiagnosticTone();
    }
  }
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);
  if (USE_EXTERNAL_STATUS_LED) {
    pinMode(STATUS_LED_PIN, OUTPUT);
  }
  writeStatusLed(false);

  Serial.begin(115200);
  delay(500);
  Serial.println("The Ooho Rev.1 bench firmware");

  AudioMemory(AUDIO_MEMORY_BLOCKS);
  audioShield.enable();
  audioShield.adcHighPassFilterDisable();
  audioShield.volume(STARTUP_VOLUME);
  outputMixer.gain(0, 0.8f);
  outputMixer.gain(1, 0.8f);
  diagnosticTone.begin(WAVEFORM_SINE);
  diagnosticTone.amplitude(0.0f);

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
    state = ERROR_STATE;
    return;
  }

  if (external_psram_size > 0) {
    activeRecordBuffer = psramRecordBuffer;
    activeRecordCapacity = min(static_cast<size_t>(external_psram_size) * 1024UL * 1024UL, PSRAM_RECORD_BYTES);
    activeRecordSeconds = activeRecordCapacity / (SAMPLE_RATE * 2UL);
    usingPsramBuffer = true;
  }

  Serial.print("Record buffer: ");
  Serial.print(activeRecordCapacity);
  Serial.print(" bytes, max ");
  Serial.print(activeRecordSeconds);
  Serial.println(usingPsramBuffer ? " sec in PSRAM." : " sec in internal RAM fallback.");

  nextIndex = findNextIndex();
  Serial.print("Ready. Next file: ");
  makeFilename(currentFilename, nextIndex);
  Serial.println(currentFilename);
  Serial.println("Button: start/stop recording. Serial: r=start/stop, p=play last, s=stop, t=tone test.");
}

void loop() {
  handleSerial();

  if (buttonPressedEdge()) {
    handleTrigger();
  }

  if (state == RECORDING) {
    drainRecordQueue(false);
  }

  if (state == PLAYING && !diagnosticToneOn && !playWav.isPlaying()) {
    state = IDLE;
    Serial.println("Playback finished.");
  }

  updateStatusLed();
}
