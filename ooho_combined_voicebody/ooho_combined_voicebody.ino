/*
  The Ooho II - voicebody field build

  Split into Arduino .ino tabs for readability.
  See docs/voicebody_snapshot.md for full lineage and behavior notes.
*/

#include <Audio.h>
#include <SD.h>
#include <SerialFlash.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>
#include <algorithm>
