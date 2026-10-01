# GS classic realization

The classic realization is the opt-in alternative to the default (modern) way the GS insertion effect is voiced. Where modern maps each printed parameter onto a chain of the library's own inserts, classic runs the soundings archive's whole-type model of the effect — the archive's leading candidate (`p0`) for each of the 64 types, a node graph drawn at 32 kHz — in a double-precision graph engine (`src/midi/synth/gs_classic/`). Both realizations share the protocol layer: the address map, parameter storage, which bytes a slot accepts, unit assignment and power-on values. Only the layer that makes sound differs.

Every candidate is still stopped short of the archive's fit gates, so "classic" means "the C++ engine draws what the archive's Python renderer draws", not "what the hardware does". How far each candidate sits from the unit is recorded as its stage-8 gross residual in the table below.

## Generated data

`tools/gs/classic_models.py` reads the archive and writes four files; none is edited by hand.

| file | what it holds | ships |
|---|---|---|
| `src/midi/synth/gs_classic_models.inc` | the default configuration: `p0` models with `tools/gs/classic-overlays/*.json` applied | yes |
| `tests/midi/gs_classic_models_raw.inc` | the raw configuration: `p0` models without overlays | no |
| `tests/midi/gs_classic_reference.tsv` | band digests the archive's renderer draws for the raw configuration | no |
| `tests/midi/gs_classic_map_expansion.inc` | every map spec of both configurations expanded over bytes 0–127 | no |

The generator starts under any python and re-executes itself under `$GS_EFX_ARCHIVE/.venv/bin/python`, since it imports `soundings.reproduce` and `soundings.render.graph`; it stops if that environment is absent. Byte maps and control curves are evaluated by the archive's own code (`_from_map` and the renderer's value reader), so the meaning of a map has one definition. Byte maps ship as specs and the registry expands each into a 128-entry LUT once, at construction; the expansion file pins the C++ expander to `_from_map` on every spec and byte.

Modes:

- default: every check, and both configurations, the reference and the expansion are written. It stops while any printed slot is read by no node.
- `--raw`: the raw configuration and the reference only (with the expansion and this page); every check but the unbound-slot one.
- `--scope MSB[:LSB-LSB],…`: the default configuration; the unbound-slot check covers the scope and reports the rest per MSB. An empty scope checks no type.
- `--check`: regenerates what the chosen mode writes into a scratch directory and diffs it against the committed files, reporting a different archive revision on its own line first.

The generator stops on a `source: document` value that is none of the four document forms (at most three points, at most five states, a semitone-ratio, cent-ratio or linear-step formula, a constant column), on a node kind or reference type the engine does not draw (the renderer's `lti` and `response` kinds among them), on an `x-` kind written by the archive, on a shaper whose `oversample` is anything but the integer 1 (the only factor the engine draws), on a section whose `gain_db` or `sections` is driven by a control (the renderer refuses both, since they decide which sections the stage is built from), and on an overlay entry that rewrites an existing node without `replaces`. A formula or constant column is re-derived from the formula rather than copied.

`load_graph()` validates each model with `_check_rows`, which reads soundings' own `documents/<doc>/effect-list.json` to confirm the model accounts for every printed row; no value from that file enters any generated output.

## Settings the renderer cannot draw

Two kinds of setting are refused by the archive's renderer and drawn by the engine anyway: a byte a `states` map names no state for, which the engine gives the nearest named state's value when the protocol accepts the byte (the lower on a tie) and the power-on byte's value when it does not; and a loop whose delay falls under one sample, which the engine draws at one sample. Both are the engine's own reading, and the reference records such states as `unrenderable` rather than comparing them.

## Invented behaviour that changes the power-on sound

An overlay that replaces a constant `p0` fitted is anchored so the power-on byte still reads that constant. An overlay that adds structure `p0` has no path for is not, and it sounds at power-on whenever the power-on bytes turn it on. Every such addition is invented or carried from a modern insert, never measured on the unit, so for the types below the default configuration's power-on sound is not the archive candidate's.

The last column is the largest third-octave band difference between the default and the raw configuration at power-on (bands at −60 dBFS or above, the reference stimulus and digest), measured at archive revision `fcef26d`. Every other type draws its power-on state as `p0` does.

| type | what sounds at power-on that `p0` does not draw | dB |
|---|---|---|
| `01 02` | the enhancer path (band-pass, drive, tanh, added to the dry signal) | 3.5 |
| `01 10`, `01 11` | Amp Sw powers up on: the amp type's cabinet after the curve | 60.9, 54.1 |
| `01 70` | the azimuth's timbre, the binaural ring's sections on the candidate's head | 6.7 |
| `01 73` | Lo-Fi Type powers up at state 1, the first Lo-Fi type's hold of three samples (10.7 kHz); the noise generators stay silent, since R.Detune, W/P Level, Disc Nz Lev and Hum Level all power up at 0 | 14.2 |
| `02 06`, `02 07`, `02 08` | the enhancer path | 3.5 |
| `02 0C` | the rotary's acceleration as the `01 22` candidate's rate gap, so the high rotor runs at a different rate, and Separate as a stereo spread | 2.8 |
| `04 00`, `04 01`, `04 02` | Amp Sw on: the cabinet after the drive; `04 00` and `04 02` also the delay's feedback loop, `04 00` and `04 01` the compressor's designed detector times | 56.8, 49.3, 39.8 |
| `04 03` | the compressor's designed detector times and the delay's feedback loop | 0.8 |
| `04 04` | the delay's feedback loop | 1.1 |
| `04 06` | the enhancer path | 1.9 |
| `05 00` | the ring modulator (the input times a sine carrier, balanced against the dry signal) and the delay's feedback loop | 15.2 |
| `11 03`–`11 06` | the drive half's Amp Sw on: its cabinet; in `11 04` also the rotary half's acceleration and Separate, in `11 06` the auto-wah half's Sens | 60.9 |
| `11 07` | the rotary half's acceleration and Separate, so the high rotor's rate differs | 4.1 |

## Reference digests

The stimulus, digest and state list are stated in the header lines of `tests/midi/gs_classic_reference.tsv`, which is the definition the conformance test follows. The states are the power-on state and each printed slot at its lowest and highest accepted byte; a slot no raw-model node reads is recorded as `unbound`, and a byte equal to the power-on byte as `same-as-power-on`, instead of being drawn again.

## Host-rate agreement

`tests/midi/gs_classic_conformance_test.cpp` holds the classic unit to drawing the same at 44.1 and 48 kHz: within 0.5 dB in every third-octave band below 12.5 kHz that either rate puts at −60 dBFS or above. The reference stimulus, repeated, is resampled up from 32 kHz, drawn through a `GsClassicUnit` at power-on, and advanced by the unit's round trip. A digest of one drawing is the wrong statistic for this, because the graph's output from a clipping, stepped or lfo-swept path depends on where the input falls against the 32 kHz grid and the lfo's start, which moves with the resamplers' latency; a single host sample of input delay at one rate moves such a type by as much as 11 dB. The statistic is therefore made alignment-invariant: each rate draws the type with its input delayed by K offsets (K = 8) spread evenly over 0.5 ms, or over the longest power-on pitch window where that is longer, since a pitch node's output cycles with its window. Each drawing is Welch-averaged over half-overlapping 0.2 s Hann windows, and the band power is averaged across the K offsets before it becomes decibels. A type is drawn for three periods of its slowest power-on lfo, between 2 and 6 s. Each rate is drawn over two interleaved sets of K offsets, and each set must meet the 0.5 dB bound on its own. In bands both sets put at −60 dBFS or above, the two sets' 44.1−48 kHz differences must agree within 0.25 dB; where they do not, K doubles (up to 64) rather than the tolerance widening. Phase variation common to both rates cancels in that paired difference and is not read as a rate error.

## CPU cost

One classic unit at a 48 kHz host rate, 128-sample stereo blocks, power-on bytes, a Release build on an Apple M5 Max, as a percentage of real time (the best of three 5 s runs, two sessions a few minutes apart on a loaded machine):

| types | % of real time |
|---|---|
| `01 00`–`01 31` | 0.26–1.13 |
| `01 40`–`01 73` | 0.29–1.67 (`01 70` 1.67–1.84, `01 71` 1.46–1.60) |
| `02 00`–`02 0C` | 0.38–0.74 |
| `04 00`–`04 06` | 0.71–1.65 (`04 06` 1.65–1.80) |
| `05 00` | 1.77–1.98 |
| `11 00`–`11 08` | 0.43–1.50 (`11 05` 1.50–1.62, `11 08` 1.49–1.61) |

The mean over the 64 types is 0.68 %. `05 00`, the heaviest, puts sixteen units at about 30 % of one core. The figure includes the two resampler pairs around the 32 kHz graph. Settings away from power-on are not measured.

## Current state

The section below is written by the generator.

<!-- BEGIN GENERATED: tools/gs/classic_models.py -->

### Archive revision

- `archive_revision`: `bda68ccb3463361a64dd9783ffb8dd8adc8a203f`
- `archive_revision_source`: git rev-parse HEAD
- `archive_inputs_dirty`: false

### Realization differences

Per type: the archive's leading candidate, the Parallel-2 arrangement read off its graph (0 not Parallel-2, 1 side-by-side, 2 in-series) and how each half's pan acts, printed slots, printed slots no node reads, states the renderer refuses in the reference, and the candidate's stage-8 gross residual.

| type | candidate | topology | pan | printed | unbound | unrenderable | gross |
|---|---|---|---|---|---|---|---|
| `01 00` | built-at-the-gain | 0 | - | 11 | 0 | 0 | 0.006 |
| `01 01` | blended-with-the-dry | 0 | - | 11 | 0 | 0 | 0.768 |
| `01 02` | first-order-tone-pair | 0 | - | 5 | 0 | 0 | 0.998 |
| `01 03` | band-passes-in-parallel-drive-before | 0 | - | 8 | 0 | 1 | 0.837 |
| `01 10` | tanh | 0 | - | 7 | 0 | 0 | 1.000 |
| `01 11` | tanh | 0 | - | 7 | 0 | 0 | 0.932 |
| `01 20` | raised-sine-in-octaves | 0 | - | 8 | 0 | 0 | 0.019 |
| `01 21` | one-sided-raised-sine | 0 | - | 11 | 0 | 0 | 0.109 |
| `01 22` | doppler-and-amplitude-stops-short | 0 | - | 13 | 0 | 0 | 0.001 |
| `01 23` | triangle | 0 | - | 11 | 0 | 1 | 1.000 |
| `01 24` | triangle-held | 0 | - | 10 | 0 | 1 | 0.286 |
| `01 25` | a-multiplier-about-unity | 0 | - | 6 | 0 | 0 | 0.022 |
| `01 26` | a-sine-cosine-pair | 0 | - | 6 | 0 | 0 | 0.005 |
| `01 30` | peak-branching | 0 | - | 7 | 0 | 0 | 0.868 |
| `01 31` | rms-branching | 0 | - | 8 | 0 | 0 | 1.372 |
| `01 40` | triangle-pre-delay-dev-in-ladder-steps | 0 | - | 10 | 0 | 0 | 0.283 |
| `01 41` | raised-sine-tremolo-on-the-wet | 0 | - | 10 | 0 | 0 | 0.210 |
| `01 42` | raised-sine-filter-before-the-split | 0 | - | 10 | 0 | 0 | 0.245 |
| `01 43` | raised-sine-both-voices-crossed | 0 | - | 8 | 0 | 0 | 0.074 |
| `01 44` | raised-sine-one-modulator | 0 | - | 8 | 0 | 0 | 0.242 |
| `01 50` | phase-on-the-way-out-loop-a-sample-late | 0 | - | 11 | 0 | 0 | 0.721 |
| `01 51` | triangle-loop-on-the-ladder-entry | 0 | - | 12 | 0 | 2 | 0.406 |
| `01 52` | fed-after-the-centre-level | 0 | - | 12 | 0 | 0 | 0.879 |
| `01 53` | spread-in-order-fed-after-the-first-level | 0 | - | 14 | 0 | 0 | 1.121 |
| `01 54` | pan-on-the-delayed-copy | 0 | - | 9 | 0 | 1 | 1.018 |
| `01 55` | parallel-combs-time-byte-a-decay-time | 0 | - | 8 | 0 | 2 | 1.094 |
| `01 56` | diffused-taps-gate-five-ms-a-step | 0 | - | 7 | 0 | 1 | 0.995 |
| `01 57` | fed-before-the-centre-level-loop-a-sample-late | 0 | - | 13 | 0 | 0 | 0.946 |
| `01 60` | pair-a-window-apart-straight-crossfade | 0 | - | 14 | 0 | 8 | 0.080 |
| `01 61` | pair-a-window-apart-loop-through-the-pre-delay | 0 | - | 10 | 0 | 4 | 0.336 |
| `01 70` | head-shadow-and-a-rear-cue-each-side-less-the-other | 0 | - | 6 | 0 | 0 | 0.001 |
| `01 71` | head-shadow-alone-the-mid-restored | 0 | - | 3 | 0 | 0 | 0.972 |
| `01 72` | hold-of-two-at-rest-quantiser-after-the-pre-filter | 0 | - | 8 | 0 | 0 | 1.000 |
| `01 73` | one-pole-pan-places-only-the-mono-sum | 0 | - | 20 | 0 | 0 | 0.901 |
| `02 00` | dry-after-the-drive-tone-first | 0 | - | 11 | 0 | 0 | 0.806 |
| `02 01` | dry-after-the-drive-tone-first | 0 | - | 12 | 0 | 1 | 1.000 |
| `02 02` | dry-after-the-drive-tone-last | 0 | - | 11 | 0 | 0 | 0.982 |
| `02 03` | dry-before-the-drive-tone-first | 0 | - | 11 | 0 | 0 | 1.537 |
| `02 04` | dry-after-the-drive-tone-first | 0 | - | 12 | 0 | 1 | 1.000 |
| `02 05` | dry-after-the-drive-tone-last | 0 | - | 11 | 0 | 0 | 0.919 |
| `02 06` | raised-sine | 0 | - | 9 | 0 | 0 | 172.690 |
| `02 07` | raised-sine | 0 | - | 10 | 0 | 1 | 0.920 |
| `02 08` | loop-a-sample-late | 0 | - | 9 | 0 | 0 | 1.017 |
| `02 09` | dry-before-the-chorus | 0 | - | 11 | 0 | 0 | 0.290 |
| `02 0A` | dry-after-the-flanger | 0 | - | 12 | 0 | 1 | 0.852 |
| `02 0B` | dry-after-the-chorus | 0 | - | 12 | 0 | 1 | 1.000 |
| `02 0C` | drive-then-equaliser | 0 | - | 18 | 0 | 0 | 0.736 |
| `04 00` | compressor-then-drive | 0 | - | 20 | 0 | 0 | 0.815 |
| `04 01` | compressor-equaliser-drive | 0 | - | 20 | 0 | 0 | 0.979 |
| `04 02` | wah-then-drive | 0 | - | 20 | 0 | 0 | 0.992 |
| `04 03` | compressor-then-equaliser | 0 | - | 19 | 0 | 0 | 0.000 |
| `04 04` | mix-bytes-add-the-effect | 0 | - | 20 | 0 | 0 | 0.191 |
| `04 05` | compressor-drive-equaliser | 0 | - | 20 | 0 | 0 | 1.000 |
| `04 06` | mix-byte-crossfades | 0 | - | 20 | 0 | 1 | 0.158 |
| `05 00` | shifter-then-equaliser-mix-adds | 0 | - | 20 | 0 | 4 | 0.026 |
| `11 00` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 13 | 0 | 0 | 0.753 |
| `11 01` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 14 | 0 | 1 | 0.636 |
| `11 02` | side-by-side-pan-places-each-half | 1 side-by-side | a: places the mono sum; b: places the mono sum | 14 | 0 | 1 | 0.816 |
| `11 03` | side-by-side | 1 side-by-side | a: places the mono sum; b: places the mono sum | 13 | 0 | 0 | 0.958 |
| `11 04` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 19 | 0 | 0 | 0.002 |
| `11 05` | in-series-pan-places-each-half | 2 in-series | a: places the mono sum; b: places the mono sum | 14 | 0 | 0 | 0.008 |
| `11 06` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 16 | 0 | 0 | 0.188 |
| `11 07` | side-by-side-pan-places-each-half | 1 side-by-side | a: places the mono sum; b: places the mono sum | 20 | 0 | 0 | 0.001 |
| `11 08` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 17 | 0 | 0 | 2.005 |

Unbound printed slots: 0 of 770 in 0 types.

### Document values

`source: document` values passed, by form: cent ratio 20, constant 107, enumeration 182, few points 125, linear step 16, semitone ratio 20. Formula and constant columns are re-derived from the formula, not copied.

### Shipping size

The default configuration ships 232 de-duplicated map specs (180 in the raw configuration), 78 control curves, 2650 nodes and 8 point runs. Packed as the structs of `model_format.h`, gzip -9:

| pool | raw bytes | gzip bytes |
|---|---|---|
| map specs | 94347 | 35155 |
| control curves | 80496 | 71480 |
| node tables | 153366 | 39523 |
| total | 328209 | 146863 |

Expanding the specs at registry construction puts 118784 bytes of LUTs on the heap per configuration.

<!-- END GENERATED: tools/gs/classic_models.py -->
