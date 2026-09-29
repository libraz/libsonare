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

The generator stops on a `source: document` value that is none of the four document forms (at most three points, at most five states, a semitone-ratio, cent-ratio or linear-step formula, a constant column), on a node kind or reference type the engine does not draw (the renderer's `lti` and `response` kinds among them), on an `x-` kind written by the archive, and on an overlay entry that rewrites an existing node without `replaces`. A formula or constant column is re-derived from the formula rather than copied.

`load_graph()` validates each model with `_check_rows`, which reads soundings' own `documents/<doc>/effect-list.json` to confirm the model accounts for every printed row; no value from that file enters any generated output.

## Settings the renderer cannot draw

Two kinds of setting are refused by the archive's renderer and drawn by the engine anyway: a byte a `states` map names no state for, which the engine gives the nearest named state's value when the protocol accepts the byte (the lower on a tie) and the power-on byte's value when it does not; and a loop whose delay falls under one sample, which the engine draws at one sample. Both are the engine's own reading, and the reference records such states as `unrenderable` rather than comparing them.

## Invented behaviour that changes the power-on sound

An overlay that replaces a constant `p0` fitted is anchored so the power-on byte still reads that constant. An overlay that adds structure `p0` has no path for is not, and it sounds at power-on whenever the power-on bytes turn it on. `01 73` (Lo-Fi 2) is one such case: its Lo-Fi Type powers up at state 1, which the overlay draws as the first Lo-Fi type's hold of three samples (10.7 kHz), so the default configuration's power-on drawing differs from `p0`'s. This is invented, not measured. Its noise generators stay silent at power-on, because R.Detune, W/P Level, Disc Nz Lev and Hum Level all power up at 0.

## Reference digests

The stimulus, digest and state list are stated in the header lines of `tests/midi/gs_classic_reference.tsv`, which is the definition the conformance test follows. The states are the power-on state and each printed slot at its lowest and highest accepted byte; a slot no raw-model node reads is recorded as `unbound`, and a byte equal to the power-on byte as `same-as-power-on`, instead of being drawn again.

## Current state

The section below is written by the generator.

<!-- BEGIN GENERATED: tools/gs/classic_models.py -->

### Archive revision

- `archive_revision`: `fcef26df0d03d05725e39747e6cfd8e0a817fbcc`
- `archive_revision_source`: git rev-parse HEAD
- `archive_inputs_dirty`: false

### Realization differences

Per type: the archive's leading candidate, the Parallel-2 arrangement read off its graph (0 not Parallel-2, 1 side-by-side, 2 in-series) and how each half's pan acts, printed slots, printed slots no node reads, states the renderer refuses in the reference, and the candidate's stage-8 gross residual.

| type | candidate | topology | pan | printed | unbound | unrenderable | gross |
|---|---|---|---|---|---|---|---|
| `01 00` | built-at-the-gain | 0 | - | 11 | 0 | 0 | 0.006 |
| `01 01` | blended-with-the-dry | 0 | - | 11 | 0 | 0 | 0.899 |
| `01 02` | first-order-tone-pair | 0 | - | 5 | 0 | 0 | 1.000 |
| `01 03` | band-passes-in-parallel-drive-before | 0 | - | 8 | 1 | 1 | 0.850 |
| `01 10` | tanh | 0 | - | 7 | 0 | 0 | 1.000 |
| `01 11` | tanh | 0 | - | 7 | 0 | 0 | 0.937 |
| `01 20` | raised-sine-in-octaves | 0 | - | 8 | 0 | 0 | 0.019 |
| `01 21` | one-sided-raised-sine | 0 | - | 11 | 0 | 0 | 0.110 |
| `01 22` | doppler-and-amplitude-stops-short | 0 | - | 13 | 0 | 0 | 0.001 |
| `01 23` | triangle | 0 | - | 11 | 0 | 1 | 1.000 |
| `01 24` | raised-sine-held-at-twice | 0 | - | 10 | 0 | 1 | 0.263 |
| `01 25` | a-multiplier-about-unity | 0 | - | 6 | 0 | 0 | 0.019 |
| `01 26` | a-crossfade-on-the-amplitudes | 0 | - | 6 | 0 | 0 | 0.021 |
| `01 30` | peak-branching | 0 | - | 7 | 0 | 0 | 0.929 |
| `01 31` | peak-branching | 0 | - | 8 | 0 | 0 | 2.200 |
| `01 40` | triangle-pre-delay-dev-in-ladder-steps | 0 | - | 10 | 0 | 0 | 0.283 |
| `01 41` | triangle-tremolo-after-the-balance | 0 | - | 10 | 0 | 0 | 0.198 |
| `01 42` | triangle-filter-before-the-split | 0 | - | 10 | 0 | 0 | 0.134 |
| `01 43` | raised-sine-both-voices-crossed | 0 | - | 8 | 0 | 0 | 0.105 |
| `01 44` | triangle-one-modulator | 0 | - | 8 | 1 | 0 | 0.298 |
| `01 50` | phase-on-the-way-out-loop-a-sample-late | 0 | - | 11 | 0 | 0 | 0.766 |
| `01 51` | triangle-loop-on-the-ladder-entry | 0 | - | 12 | 0 | 2 | 0.242 |
| `01 52` | fed-after-the-centre-level | 0 | - | 12 | 0 | 0 | 0.890 |
| `01 53` | spread-in-order-fed-after-the-first-level | 0 | - | 14 | 0 | 0 | 1.121 |
| `01 54` | pan-on-the-delayed-copy | 0 | - | 9 | 0 | 1 | 1.022 |
| `01 55` | parallel-combs-time-byte-a-decay-time | 0 | - | 8 | 0 | 2 | 1.095 |
| `01 56` | diffused-taps-gate-five-ms-a-step | 0 | - | 7 | 0 | 1 | 0.997 |
| `01 57` | fed-before-the-centre-level-loop-a-sample-late | 0 | - | 13 | 1 | 0 | 0.959 |
| `01 60` | pair-a-window-apart-raised-cosine-crossfade | 0 | - | 14 | 0 | 8 | 0.080 |
| `01 61` | pair-a-window-apart-loop-through-the-pre-delay | 0 | - | 10 | 0 | 4 | 0.336 |
| `01 70` | head-shadow-and-a-rear-cue-each-side-less-the-other | 0 | - | 6 | 0 | 0 | 0.001 |
| `01 71` | head-shadow-alone-the-mid-restored | 0 | - | 3 | 0 | 0 | 1.526 |
| `01 72` | hold-of-two-at-rest-quantiser-after-the-pre-filter | 0 | - | 8 | 0 | 0 | 1.000 |
| `01 73` | one-pole-pan-places-only-the-mono-sum | 0 | - | 20 | 0 | 0 | 1.319 |
| `02 00` | dry-after-the-drive-tone-first | 0 | - | 11 | 2 | 0 | 0.810 |
| `02 01` | dry-after-the-drive-tone-first | 0 | - | 12 | 2 | 1 | 1.000 |
| `02 02` | dry-after-the-drive-tone-last | 0 | - | 11 | 2 | 0 | 0.983 |
| `02 03` | dry-before-the-drive-tone-first | 0 | - | 11 | 2 | 0 | 1.221 |
| `02 04` | dry-after-the-drive-tone-first | 0 | - | 12 | 2 | 1 | 1.000 |
| `02 05` | dry-after-the-drive-tone-last | 0 | - | 11 | 2 | 0 | 0.925 |
| `02 06` | raised-sine | 0 | - | 9 | 2 | 0 | 171.962 |
| `02 07` | raised-sine | 0 | - | 10 | 2 | 1 | 0.920 |
| `02 08` | loop-a-sample-late | 0 | - | 9 | 2 | 0 | 1.019 |
| `02 09` | dry-before-the-chorus | 0 | - | 11 | 0 | 0 | 0.331 |
| `02 0A` | dry-after-the-flanger | 0 | - | 12 | 0 | 1 | 0.852 |
| `02 0B` | dry-after-the-chorus | 0 | - | 12 | 0 | 1 | 1.000 |
| `02 0C` | drive-then-equaliser | 0 | - | 18 | 3 | 0 | 0.372 |
| `04 00` | compressor-then-drive | 0 | - | 20 | 5 | 0 | 0.573 |
| `04 01` | compressor-equaliser-drive | 0 | - | 20 | 4 | 0 | 0.979 |
| `04 02` | wah-then-drive | 0 | - | 20 | 3 | 0 | 0.992 |
| `04 03` | compressor-then-equaliser | 0 | - | 19 | 3 | 0 | 0.000 |
| `04 04` | mix-bytes-add-the-effect | 0 | - | 20 | 1 | 0 | 0.176 |
| `04 05` | compressor-drive-equaliser | 0 | - | 20 | 4 | 0 | 1.000 |
| `04 06` | mix-byte-crossfades | 0 | - | 20 | 2 | 1 | 0.173 |
| `05 00` | shifter-then-equaliser-mix-adds | 0 | - | 20 | 3 | 4 | 0.031 |
| `11 00` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 13 | 0 | 0 | 0.825 |
| `11 01` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 14 | 0 | 1 | 0.714 |
| `11 02` | side-by-side-pan-places-each-half | 1 side-by-side | a: places the mono sum; b: places the mono sum | 14 | 0 | 1 | 0.891 |
| `11 03` | side-by-side | 1 side-by-side | a: places the mono sum; b: places the mono sum | 13 | 4 | 0 | 0.962 |
| `11 04` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 19 | 5 | 0 | 0.164 |
| `11 05` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 14 | 2 | 0 | 0.002 |
| `11 06` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 16 | 3 | 0 | 0.187 |
| `11 07` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 20 | 3 | 0 | 0.164 |
| `11 08` | side-by-side-pan-moves-each-pair | 1 side-by-side | a: moves each channel; b: moves each channel | 17 | 1 | 0 | 2.011 |

Unbound printed slots: 67 of 770 in 27 types.

### Document values

`source: document` values passed, by form: cent ratio 20, constant 107, enumeration 182, few points 125, linear step 16, semitone ratio 20. Formula and constant columns are re-derived from the formula, not copied.

### Shipping size

The default configuration ships 206 de-duplicated map specs (183 in the raw configuration), 27 control curves, 1879 nodes and 8 point runs. Packed as the structs of `model_format.h`, gzip -9:

| pool | raw bytes | gzip bytes |
|---|---|---|
| map specs | 86999 | 28968 |
| control curves | 27864 | 21274 |
| node tables | 104720 | 30405 |
| total | 219583 | 81681 |

Expanding the specs at registry construction puts 105472 bytes of LUTs on the heap per configuration.

<!-- END GENERATED: tools/gs/classic_models.py -->
