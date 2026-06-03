# Ooho Coupled MELOS/LOGOS Experiment

Created on 2026-06-03 as a fork from the saved `ooho_combined_emergent_v3`
snapshot.

## Sketch

- Active experiment: `ooho_combined_coupled/ooho_combined_coupled.ino`
- Previous uncoupled snapshot: `ooho_combined_emergent_v3/ooho_combined_emergent_v3.ino`

## Concept

In v3, LOGOS and MELOS were independent layers:

- LOGOS: real recorded word fragments on the left channel.
- MELOS: live synthesized collective voice on the right channel.

This fork makes them listen to each other. Every LOGOS grain sends a small
excitation into MELOS:

- normal recognizable words can briefly stabilize MELOS pitch;
- micro-grains open the vocal tract and make MELOS more vowel-like;
- reversed grains and dense grain texture add constriction and breath;
- overlapping LOGOS streams increase MELOS air/noise and high-band tension.

The goal is not an obvious effect. The goal is a coupled object-body:
when a word appears in the archive, the collective voice subtly changes shape.

## Implementation Notes

The coupling is done without SD reads or writes in the audio callback:

- LOGOS grains are already loaded into PSRAM before playback.
- `startLogosGrain()` calls `exciteMelosFromLogos()`.
- MELOS stores four decaying excitation values:
  `melosLogosOpen`, `melosLogosConstrict`, `melosLogosStabilize`,
  `melosLogosDensity`.
- `melosNextSample()` uses those values to alter drift, band weights, breath,
  voiced/noise balance, and amplitude.

The saved v3 snapshot remains the fallback if the coupling feels too reactive
or too unstable in the installation.

## Listening Notes

Expected behavior:

- right channel MELOS should subtly react when left-channel grains appear;
- dense LOGOS passages should make MELOS airier and more tense;
- clear forward words should make MELOS briefly less wandering;
- micro/reversed grains should create a slight throat/narrowing response;
- there should be no crunch, rhythmic modulation, or playback dropout.

## LOGOS Cloud Engine Update

The coupled fork now extends LOGOS from four one-shot grain players into four
role-based granular streams.

Each stream still loads one selected word/grain from SD into PSRAM, but it can
now spawn a small cloud of internal grain voices from that same buffer:

- no extra SD seeks are needed for the internal cloud voices;
- every cloud voice has its own offset, duration, delay, rate, direction, and
  fade;
- up to 8 cloud voices can be mixed inside one LOGOS stream;
- recent grain indexes are remembered so the shuffle avoids immediate repeats.

Stream roles:

- stream 1: mostly recognizable words, rare small clouds;
- stream 2: lower/body fragments, slower ratios and more reverse;
- stream 3: syllable rhythm, medium micro-clouds;
- stream 4: short bright sparks and denser burst clouds.

Musical rate ratios are role-based rather than purely random:

- slow/body: `0.5x`, `0.666x`, `0.75x`;
- speech center: `1.0x`;
- bright/upper: `1.333x`, `1.5x`, `2.0x`.

This keeps LOGOS made from real archive recordings, but lets words decompose
into rhythm, harmony, and texture without adding reverb or external effects.

## Anti-Language Musical Patterns

The cloud engine now keeps occasional recognizable words, including longer
chunks up to about 1.8 seconds, but most dense behavior avoids linguistic
meaning by using pattern-based deconstruction:

- `stutter`: repeats the same articulatory point with small jitter;
- `arp`: turns one voice fragment into a pitched/rate arpeggio;
- `scrub`: scans forward or backward through a word in short slices.
- `scatter`: throws short fragments across the word with irregular delays,
  rates, and a higher reverse chance.

Full words remain important as ghosts of language, but the dominant texture is
phoneme dust, syllable rhythm, and musical rate relationships.

## More Audible MELOS Archive Control

MELOS now reacts more clearly to the voice archive:

- `model.meanLogF0` controls the pitch center more directly;
- `model.bands` have stronger influence over the live vocal tract color;
- `model.voicedRatio` affects the tonal/breath balance more audibly;
- after each recording, MELOS performs a temporary learned-voice gesture using
  the new recording's detected F0, breathiness, and dominant spectral band.

This should make the right channel feel less like an unrelated mysterious
wave, and more like the cube digesting the archive in real time.

## Close LOGOS Balance

After listening, LOGOS was too distant relative to MELOS. The coupled fork now
uses a closer left-channel balance:

- LOGOS stream gain is raised;
- MELOS gain is slightly reduced;
- stream 1 plays more often and remains mostly full-word;
- dormant/active schedules are shorter, so grains appear more frequently;
- cloud attenuation is lighter, so stutter/arp/scrub events feel nearer;
- fades are shorter, keeping articulation present without hard clicks.

The LOGOS-to-MELOS coupling has also been raised by about 25%, so LOGOS events
should more clearly open, constrict, stabilize, or aerate the right-channel
MELOS body.

## Voice-Forward Compression

The current coupled build makes the recorded voice the main character:

- LOGOS and MELOS output gains are both raised by about 25%;
- LOGOS has a simple static compressor/soft limiter after cloud mixing;
- the compressor lifts voice fragments forward while keeping dense clouds from
  clipping too harshly;
- MELOS is louder than before but should remain the supporting body behind the
  left-channel archive voice.

## MELOS Sidechain And Forms

MELOS now listens to the actual LOGOS audio level, not only grain start events:

- the compressed LOGOS output feeds a small envelope follower;
- the follower ducks MELOS slightly so recorded voice comes forward;
- the same envelope opens/brightens MELOS and adds breath during LOGOS events;
- clear forward words push MELOS toward an open vocal/chord form;
- reversed grains push MELOS toward a narrow/nasal form;
- micro-grains push MELOS toward breath/air forms.

This should make LOGOS influence more legible: when the archive voice appears,
the right-channel body yields and changes shape instead of remaining one static
cluster.

## LOGOS Crowd Density Modes

LOGOS now has macro-density states instead of a flat random scheduler:

- `sparse`: fewer voices, lower gain, more air;
- `present`: the normal close archive voice;
- `swarm`: all streams wake more often, clouds use more internal voices, and
  LOGOS gain rises.

The mode changes every 18-56 seconds. In swarm mode, streams 2-4 are forced
active so the listener can hear multiple layers at once instead of only
occasional isolated grains.

Grain selection also now biases toward the most recent recordings:

- in present mode, recent recordings are preferred often;
- in swarm mode, recent recordings are preferred even more strongly;
- this makes newly recorded voices enter LOGOS audibly soon after capture.
