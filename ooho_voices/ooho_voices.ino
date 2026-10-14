/*
  The Ooho II - voices build (EXPERIMENT, 2026-10-14)

  Fork of ooho_combined_voicebody. LOGOS is replaced by up to four
  'speakers' made of the archive's syllables: storm -> speech -> rare after
  every take (lab/cube_states.py). The exhibition build stays in
  ooho_combined_voicebody.

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
