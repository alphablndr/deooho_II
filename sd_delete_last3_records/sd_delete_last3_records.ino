#include <Arduino.h>
#include <SD.h>

constexpr uint8_t DELETE_COUNT = 3;
constexpr uint32_t BAUD = 115200;

struct Recording {
  char name[13];
  uint16_t index;
};

Recording targets[DELETE_COUNT];
uint8_t targetCount = 0;

bool parseRecordingName(const char *name, uint16_t &index) {
  if (strlen(name) != 9) {
    return false;
  }
  if (name[0] != 'W' || name[5] != '.' ||
      toupper(name[6]) != 'W' || toupper(name[7]) != 'A' || toupper(name[8]) != 'V') {
    return false;
  }
  uint16_t value = 0;
  for (uint8_t i = 1; i < 5; ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return false;
    }
    value = value * 10 + (name[i] - '0');
  }
  index = value;
  return value > 0;
}

bool isTargetName(const char *name) {
  for (uint8_t i = 0; i < targetCount; ++i) {
    if (strcasecmp(name, targets[i].name) == 0) {
      return true;
    }
  }
  return false;
}

void considerTarget(const char *name, uint16_t index) {
  if (targetCount < DELETE_COUNT) {
    strncpy(targets[targetCount].name, name, sizeof(targets[targetCount].name));
    targets[targetCount].name[sizeof(targets[targetCount].name) - 1] = '\0';
    targets[targetCount].index = index;
    targetCount++;
    return;
  }

  uint8_t lowest = 0;
  for (uint8_t i = 1; i < targetCount; ++i) {
    if (targets[i].index < targets[lowest].index) {
      lowest = i;
    }
  }
  if (index > targets[lowest].index) {
    strncpy(targets[lowest].name, name, sizeof(targets[lowest].name));
    targets[lowest].name[sizeof(targets[lowest].name) - 1] = '\0';
    targets[lowest].index = index;
  }
}

void sortTargets() {
  for (uint8_t i = 0; i < targetCount; ++i) {
    for (uint8_t j = i + 1; j < targetCount; ++j) {
      if (targets[j].index < targets[i].index) {
        Recording tmp = targets[i];
        targets[i] = targets[j];
        targets[j] = tmp;
      }
    }
  }
}

bool lineStartsWithTarget(const char *line) {
  char filename[13] = {0};
  uint8_t i = 0;
  while (line[i] != '\0' && line[i] != ',' && i < sizeof(filename) - 1) {
    filename[i] = line[i];
    i++;
  }
  filename[i] = '\0';
  return isTargetName(filename);
}

void rewriteGrains() {
  if (!SD.exists("GRAINS.CSV")) {
    Serial.println("GRAINS.CSV missing, nothing to rewrite.");
    return;
  }
  SD.remove("GRAINS.TMP");

  File in = SD.open("GRAINS.CSV", FILE_READ);
  File out = SD.open("GRAINS.TMP", FILE_WRITE);
  if (!in || !out) {
    Serial.println("Could not open GRAINS.CSV/GRAINS.TMP.");
    if (in) in.close();
    if (out) out.close();
    return;
  }

  char line[96];
  uint8_t pos = 0;
  uint32_t kept = 0;
  uint32_t removed = 0;
  while (in.available()) {
    const char c = in.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n' || pos >= sizeof(line) - 1) {
      line[pos] = '\0';
      if (pos > 0) {
        if (lineStartsWithTarget(line)) {
          removed++;
        } else {
          out.println(line);
          kept++;
        }
      }
      pos = 0;
    } else {
      line[pos++] = c;
    }
  }
  if (pos > 0) {
    line[pos] = '\0';
    if (lineStartsWithTarget(line)) {
      removed++;
    } else {
      out.println(line);
      kept++;
    }
  }
  in.close();
  out.close();

  SD.remove("GRAINS.BAK");
  SD.rename("GRAINS.CSV", "GRAINS.BAK");
  SD.rename("GRAINS.TMP", "GRAINS.CSV");

  Serial.print("GRAINS kept=");
  Serial.print(kept);
  Serial.print(" removed=");
  Serial.println(removed);
}

void updateCounter() {
  if (targetCount == 0) {
    return;
  }
  const uint16_t nextIndex = targets[0].index;
  SD.remove("COUNTER.TXT");
  File counter = SD.open("COUNTER.TXT", FILE_WRITE);
  if (!counter) {
    Serial.println("Could not write COUNTER.TXT.");
    return;
  }
  counter.println(nextIndex);
  counter.close();
  Serial.print("COUNTER.TXT set to ");
  Serial.println(nextIndex);
}

void setup() {
  Serial.begin(BAUD);
  delay(1500);
  Serial.println("Ooho maintenance: delete last 3 recordings");

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial.println("SD.begin failed.");
    return;
  }

  File root = SD.open("/");
  if (!root) {
    Serial.println("Could not open SD root.");
    return;
  }

  while (true) {
    File entry = root.openNextFile();
    if (!entry) {
      break;
    }
    if (!entry.isDirectory()) {
      uint16_t index = 0;
      if (parseRecordingName(entry.name(), index)) {
        considerTarget(entry.name(), index);
      }
    }
    entry.close();
  }
  root.close();
  sortTargets();

  if (targetCount == 0) {
    Serial.println("No Wxxxx.WAV recordings found.");
    return;
  }

  Serial.println("Deleting:");
  for (uint8_t i = 0; i < targetCount; ++i) {
    Serial.print("  ");
    Serial.print(targets[i].name);
    Serial.print(" index=");
    Serial.println(targets[i].index);
  }

  for (uint8_t i = 0; i < targetCount; ++i) {
    if (SD.remove(targets[i].name)) {
      Serial.print("Removed ");
      Serial.println(targets[i].name);
    } else {
      Serial.print("Remove failed ");
      Serial.println(targets[i].name);
    }
  }

  rewriteGrains();
  updateCounter();
  Serial.println("Done. Restore field firmware now.");
}

void loop() {
}
