# Probes — what gets played, and why

A probe is the MIDI both sides render. It decides which measurements are available at all: a pattern that holds velocity fixed cannot score a dynamics term, and a pattern whose notes sit outside an instrument's compass measures a transposition rather than a voice.

## Patterns

`patterns.PATTERN_BUILDERS`, selected with `--pattern`:

| pattern | what it probes | analyzed per-note |
|---|---|---|
| `sustain` | steady-state timbre at low/mid/high register (per-program ranges) | yes |
| `velocity` | dynamics curve at one pitch (vel 40/70/100/127) | yes |
| `staccato` | attack transients and release | no (too short for spectra) |
| `drum` | one percussion instrument struck at vel 64/100/127, on the drum channel | yes (percussion metrics) |
| `drum-holdout` | the same at vel 48/88/112 — the generalisation check for a drum fit | yes (percussion metrics) |
| `drum-sequence` | the mute group, a sixteenth pattern, a flam and a roll — see [below](#drum-sequence-is-what-fires-the-mute-group) | no (whole-timeline only) |
| `room-probe` | short notes, 4 s gaps — measures a reference's reverberation | no |
| `scale` | legato musicality, listening check | no |

`--notes` and `--velocities` override a pattern's own axis. A pattern that takes a single value rather than a list accepts the plural flag anyway: `--notes 62` on `velocity` names the pitch it sweeps velocity at, and `--velocities 90` on `sustain` names the one it holds. A pattern with neither axis refuses the flag rather than ignoring it.

## Which notes a program is probed at

Not a fixed table. `toneclass.py` decides it from what kind of sound the GM program makes, and the same classification decides two other things that used to be decided separately — which metric set scores the program, and which loss terms carry weight. Before that, a register table covered three families and everything else fell through to C3/C4/C5 whatever the instrument's compass was, which is how program 9 came to be probed at C3/C4/C5, scored with a harmonic ladder, under weights tuned for a violin.

## Drums

`--drum-note N` switches the probe and the metric set at once, because a drum note has no fundamental and every metric anchored on one would measure a frequency the sound does not contain. MIDI note 38 is the acoustic snare; reading its "expected f0" as 87 Hz would build a harmonic ladder out of a noise burst.

**The probe moves to MIDI channel 10.** That is what makes a note number select an instrument rather than a pitch; `--programs` / `--program` then selects the *kit* (0 is the standard kit), not a melodic instrument. libsonare routes the channel through `drum_note_table()` and a GM reference synth through its drum bank, so both sides play the same instrument from the same file.

**Velocity is the axis, because a drum note has no register.** The probe strikes one instrument at three velocities and the held-out set is three more (`--validate-velocities`) — the drum equivalent of fitting a violin on three pitches and checking it on three others. `--validate-notes` does nothing for a drum fit; a different drum note is voiced by a different patch, whose knobs this fit never touched.

**The hits are 50 ms long, and for a few notes that is the measurement rather than the instrument.** A drum is a one-shot, so the note-off carries nothing and the gate can be as short as the score allows — true of 58 of the 61 notes on a sampled GM kit, which render bit for bit identically at every gate from 50 ms to 1600 ms. The exceptions are the record scratch and the two whistles, which that kit voices as held instruments whose length *is* the gate; libsonare voices all three as fixed one-shots, so their envelopes are being compared against a number the probe chose. `--drum-gate-ms` moves it. Nothing else in the kit hears the difference.

### The cymbals get a longer probe and a longer capture than the rest of the kit

Every cymbal's measured `decay_ms` — the time to fall 20 dB — sat between 1170 and 1760 ms against an analysis ceiling of 1800, which means those numbers were the window and not the instrument, and the four to ten seconds of wash after it was never recorded on either side. The wash is most of what makes a cymbal a cymbal.

`patterns.LONG_DECAY_DRUM_NOTES` (defined in `metrics.py` and imported by `patterns.py`) gives those notes an eight-second gap in the probe, and `tail_by_note` in the capture definition records them for eight seconds instead of two; a flat eight-second tail would triple a 282-note grid's render time for the sake of ten notes, and naming them costs about six minutes. The third side of the same decision is `HIT_LONG_MAX_SEC`, the analysis ceiling those notes are measured to — without it the longer probe and the longer capture both arrive at a window that still stops at 1.8 s, which is a mechanism that renders nothing.

The set and the capture's table are not the same list, and should not be: `LONG_DECAY_DRUM_NOTES` names the belltree at 84 because that is a fact about the instrument, while the drum capture's grid is the GM standard kit and stops at 81. A `tail_by_note` entry for a note outside the grid reads as a decision and records nothing, so a test refuses one.

### `drum-sequence` is what fires the mute group

42, 44 and 46 share a GM exclusive class, so striking one chokes the others — that is the pedal, it is most of what a hi-hat part sounds like, and a probe that strikes one note every two seconds has never fired it once. The same gap covers a sixteenth pattern landing on its own ring, a flam, and a roll.

Nothing in the sequence is an analysis note: every per-hit measurement assumes an isolated strike, and none of it survives hits a tenth of a second apart. What the probe is for is the whole-timeline comparison, which needs no such assumption and is the one measure that can see a choke that did not happen.

### A sampled kit need not lay its instruments out the way GM does

`note_map` in a capture definition is where a capture says so. The kit measured for `reference/drums.json` does not: its six toms ascend 45, 47, 48, 50, 41, 43, with the two largest on the keys where GM puts the two smallest. Without a map, `profile.py compare` scores the low floor tom against the high one and reports a tuning error that is really a mapping — the reference rows are correct measurements that could not be used, for want of a correspondence.

The map is applied to the **oracle side only**: libsonare ships GM's layout because that is what a MIDI file is written against, and correcting the model would calibrate this reference's idiosyncrasy into the product. A mapped row prints as `41>48`.

**The kit-relation term needs none of this**, and that is a design decision rather than a coincidence. It compares each family's sorted contrasts, so a family is a set and has no order to disagree about; the six toms group correctly whichever key holds which drum. What that buys is a measurement of the series that stands whether or not a map was ever written, and what it costs is that the term cannot see a **permuted** family — placement stays the per-note terms' job. See the [`kit` term](loss.md#percussion-terms).

What is **still** not covered: the kit assignment itself, and anything that needs two members struck together.
