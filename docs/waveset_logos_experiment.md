# Ooho Waveset LOGOS Experiment

Forked from `ooho_combined_coupled_density_snapshot`.

## Sketch

- Active experiment: `ooho_combined_waveset/ooho_combined_waveset.ino`
- Fallback snapshot: `ooho_combined_coupled_density_snapshot/ooho_combined_coupled_density_snapshot.ino`

## CDP-Inspired Idea

This experiment borrows from the Composers Desktop Project / CDP waveset
approach: instead of only slicing voice by milliseconds, LOGOS can now slice
inside a loaded grain at zero-crossing boundaries.

The firmware does not port CDP. It uses a small real-time-friendly version:

- load one LOGOS grain into PSRAM;
- scan up to 96 zero-crossing boundaries;
- create short waveset groups from those boundaries;
- spawn cloud voices from these groups;
- sometimes repeat one group, sometimes jump across groups, sometimes reverse.

## New Pattern

`waveset` joins the existing LOGOS cloud patterns:

- `stutter`
- `arp`
- `scrub`
- `scatter`
- `waveset`

Expected sound: less like arbitrary chopped audio, more like voice-cycle
material being repeated, swapped, and chewed. It may be glitchier than the
density snapshot, which is why the previous build was saved separately.
