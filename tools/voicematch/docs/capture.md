# Capture and reference profiles — `capture.py`, `profile.py`

The oracle routes in [oracles.md](oracles.md) compare one probe at a time. A corpus is the other half: the whole note-by-velocity grid of a real instrument, measured once, so a voice is fitted to properties rather than to three notes.

**Capturing needs macOS, the commercial plugin being captured, and `aubounce`, and a fresh clone has none of the three.** That is how the committed references in `reference/` were produced and it is not how they are consumed: `profile.py compare`, `autofit.py` and `--diagnose` all read `reference/<id>.json`, which is committed, so a voice can be scored, fitted and diagnosed against a captured instrument with no plugin present. What needs the plugin is re-capturing — measuring an instrument nobody has measured yet, or re-measuring one after a plugin update.

Without it, the reference comes from somewhere else and everything downstream is unchanged: `--sf2` plays a SoundFont through fluidsynth on any platform, and `--oracle-wav` takes a WAV of the probe rendered by anything at all. `export-probe` writes the score to render. The audition page has the same shape: `make_audition.py --model-only` needs no plugin and produces a set that plays rather than compares.

## Where the audio lives

Five places, and which one a file belongs in follows from whether it can be committed and whether it can be regenerated.

| | what is there | committed |
|---|---|---|
| `capture/<id>.json` | the capture **method**: the grid, the gate, the timbre ids, the GM program the model answers it with | **yes** |
| `capture/<id>.local.json` | the capture **identity**: the plugin's component triple and each timbre's preset | no — gitignored |
| `reference/<id>.json` | the measurements taken from that capture — the actual calibration target | **yes** |
| `.cache/voicematch/capture/<id>/` | the captured WAVs, one per (timbre, note, velocity), plus their manifest | no, and cannot be |
| `.cache/voicematch/audition/<name>/` | the listening sets rendered from a capture | no |

**The identity is split out on purpose.** `capture.py` reads `<id>.json` and folds `<id>.local.json` over it (`merge_overlay`, matching timbres by `id`), so `plugin` and `timbres[].preset` reach the loader without being named in a tracked file. Everything a fit, a comparison or a diagnosis needs is on the tracked side; only re-capturing needs the overlay. A definition's own `_identity` key says which sibling holds it.

**The captured audio cannot enter the repository**: a sample library's licence covers what is rendered from it. It also should not, even where a licence would allow it — what a physical model has to reproduce is a handful of measured properties, not four hundred megabytes of one particular instrument. `reference/<id>.json` is those properties, and it is what a fit and a gate are held against.

`SONARE_VOICEMATCH_ROOT` moves that whole scratch root off the checkout, which is worth doing: a capture is expensive — several seconds per note, in real time, with retries — so one worth re-measuring is kept rather than re-rendered, and `profile.py --corpus <path>` reads a corpus from wherever it was kept.

Renders that are cheap to reproduce stay in `out/` and `out/au_cache/`, both gitignored: probe WAVs, comparison output, and the plugin render cache keyed on everything that defines a sound.

## Shipped capture definitions

`capture/piano.json`, `capture/harpsichord.json`, `capture/drums.json`, `capture/pipe_organ.json`. The grand's holds a long gate and a short tail because on a piano note-off is the damper — the free decay a model has to match happens while the key is still down. The harpsichord's is shorter, because its jack drops the moment the key is released and what follows is a fast damping plus the jack's own noise.

Committed references: `reference/piano.json`, `harpsichord.json`, `drums.json`, `pipe_organ.json`, plus the gate files `drums_gate.json` and `pipe_organ_gate.json`.

## Writing a capture definition for a new plugin

Every field is discoverable, and none of them should be guessed. `aubounce` answers the first four questions and `capture.py calibrate` answers the fifth.

```sh
cargo install --git https://github.com/libraz/aubounce   # once

# 1. What is installed, and its component triple.
aubounce list

# 2. What is inside it. A rack keeps its contents in a chunk only it can parse;
#    --strings decompresses what is compressed and reports what is readable, so
#    it says what a rack contains without knowing the format. Unlabelled: it
#    does not say which slot holds what.
aubounce info "<rack plugin triple>" --settle-ms 4000 --strings harpsi

# 3. Which MIDI channel plays which slot. This is the one that cannot be
#    guessed and the one a multitimbral rack never publishes: --probe-slots
#    raises one solo control at a time and plays the channels that sound.
#    --probe-channels is the fallback when a plugin has no solo per slot.
aubounce info "<rack plugin triple>" --settle-ms 4000 --probe-slots

# 4. What it advertises as an effect section, which is what `dry` switches off.
aubounce info "<rack plugin triple>" --params reverb

# 5. What the host has to give it. Measured, and reported rather than asserted.
capture.py calibrate --config capture/<id>.json
```

### Fields

Tracked `capture/<id>.json`:

| field | what it is | default |
|---|---|---|
| `id` | names `.cache/voicematch/capture/<id>/` and `reference/<id>.json` | — |
| `label` / `audition_title` | how the capture and its audition page are titled | — |
| `program` | the GM program the model answers this reference with | `0` |
| `bank` | the GS variation bank, where the reference is one (the pipe organ) | — |
| `takes` | the audition phrase set (`piano`, `harpsichord`, `drums`, `sustained`) | `""` |
| `dimensions` | which `compare` columns the gate holds; empty means all of them | `[]` |
| `timbres[]` | `id`, `label`, and `channel` — the slot, from `--probe-slots` | — |
| `notes` / `velocities` | the grid. Velocity is an axis in its own right on anything plucked | — |
| `note_map` | oracle-side note correspondence, where the kit is not laid out as GM ([probes.md](probes.md#a-sampled-kit-need-not-lay-its-instruments-out-the-way-gm-does)) | — |
| `groups` | which of the notes are one instrument — the tom series, the hi-hat trio — as `name: [notes]`, for the kit-relation term ([loss.md](loss.md#percussion-terms)) | `{}` |
| `settle_ms` / `realtime` | from `calibrate`, at twice the measured minimum | `4000` / `true` |
| `gate_ms` / `tail` | how long the key is held and how long is recorded after it lifts | `8000` / `"2s"` |
| `tail_by_note` | a longer tail for the notes that need one — the cymbals | `{}` |
| `preroll_ms` / `sample_rate` | render lead-in, and the rate everything is measured at | `100` / `48000` |
| `dry` | switch off every effect section the plugin advertises | `true` |
| `params` | anything else, as `Name=value` | `[]` |

Untracked `capture/<id>.local.json`, folded over the above:

| field | what it is |
|---|---|
| `plugin` | the triple from `aubounce list` |
| `label` | the product name, if the tracked label is generic |
| `timbres[]` | `id` (matching the tracked one) plus `preset` and a product-specific `label` |

**A rack's slot names are not a description of what the slot does.** On the harpsichord rack the slot called `Digi` decays over 36 seconds and the one called `Ambient` decays in 3. That is why that capture sets `dry: false` and measures each timbre's room instead: the switch cannot be trusted where the effects are per-slot sends rather than a section the plugin advertises. Measure before believing a name, and record which slots came out dry — that decision is what a later `compare` depends on, since it does not correct for a room.

**A `groups` entry is a set, never a sequence.** The kit-relation term compares the sorted contrasts inside a family, so the order the notes are written in is not read and a kit whose layout disagrees with GM's needs no map to be grouped correctly. Which relations a family holds is measured from the reference's own rows rather than declared beside it, so a family whose members this capture cannot tell apart quietly scores nothing — which is why the drum capture declares thirteen families and leaves out the whistle pair, whose lengths differ by 0.31 doublings and are the 50 ms gate rather than the instrument. A test refuses a family that names an uncaptured note, and one that holds no relation at all in its own reference.

**A `_`-prefixed key is a note to the next reader and is ignored by the loader.** Every shipped definition uses them to record why a value is what it is — which slots were left out and why, what the velocity axis is there to settle, why the gate is the length it is. That is the right place for it: the reasoning belongs next to the number, not in a commit message.

## `capture.py`

| command | what it does |
|---|---|
| `calibrate` | measures the host settings this plugin needs (`--note`, `--velocity` pick the probe) |
| `corpus` | renders the note × velocity × timbre grid, one note per process, resumable (`--no-resume`, `--limit`) |
| `verify` | re-reads the corpus and reports what is wrong with it |

All three take `--config`, `--out` (default `.cache/voicematch/capture/<id>`) and `--verbose`.

`capture.py`'s own finding is worth keeping: the piano sampler fails to load its samples on roughly one render in three, and *more* settling time does not help — 4 s and 16 s both produced the real peak while 8 s in between produced silence. So the capture retries, and decides "too quiet" from the loudest velocity already recorded for that same note rather than from an absolute floor, since a real note at the top of the keyboard at velocity 24 is quiet enough that any fixed threshold either lets a failure through or rejects real notes.

### A render can come back loud, plausible, and not the note

The quiet retry above catches a render whose samples did not arrive. The drum kit turned out to have the opposite failure, which every level test passes.

Re-rendering 120 slots of that grid and correlating each against its original, 39 came back a **completely different signal** — correlation 0.000, 4 to 25 dB *louder* than the real note, beginning 150 to 843 ms into the file after a stretch of digital silence, where every correct render begins at exactly the 100 ms preroll. Whole instruments were affected: all six velocities of the closed hi-hat and all six of the mute triangle, whose peaks then read −7.99, −8.03, −8.04, −0.57, −0.79, −0.32 dBFS against a clean ramp that ascends smoothly. Every slot that was *not* one of these reproduced bit for bit, so the plugin is deterministic and the corrupt renders are not a round robin.

Nothing could have caught it from the level, because the failure is loud, and nothing did: it sat in a committed reference profile and in every number derived from it. What separates the two populations perfectly is **when** the audio starts. So `_render_note` now reads the written WAV back, finds the first sample over an absolute `ONSET_FLOOR_DBFS` (−80 dBFS, far under anything an instrument radiates and far over a preroll that is exactly zero), and retries a render that begins more than `ONSET_SLACK_MS` past the preroll. The measured onset is recorded per slot as `onset_ms`, so a manifest can be audited without re-rendering anything.

A slow-attack instrument does not defeat it: the test is the first sample over a floor, not the peak, so what it finds is where the render stopped being silence — which is the note-on however long the swell after it takes.

**The general shape is worth carrying to the next capture.** A guard written against one failure mode says nothing about its opposite, and a corpus is exactly where that goes unnoticed: 282 files nobody listens to, reduced to a committed table of numbers that all look like numbers. `calibrate` renders one note twice and reports agreement, which is the right check and cannot see a failure that strikes a seventh of the grid.

## `profile.py`

| command | what it does |
|---|---|
| `measure` | corpus WAVs → `reference/<id>.json`, the committed calibration target |
| `render-grid` | renders the model over the capture's own grid, as one more timbre of the corpus (`--timbre`, default `model`) |
| `compare` | diffs the model's grid against the reference, per dimension (`--gate`, `--write-gate`, `--margin`) |
| `agree` | measures a second, independent reference over the same grid and reports which dimensions the two agree on |
| `dynamics` | tabulates the pp→ff swing rather than one velocity at a time |

All take `--config`, `--corpus`, `--profile` and `--program`; all but `measure` and `render-grid` take `--timbre` and `--notes`.

`render-grid` is what puts the model into the same corpus as one more timbre, so nothing downstream needs a special case for it.

On a kit, `compare` follows its table with one line per family and relation — the reference's spread against the model's, in doublings, sorted by what is costing most. It exists for the same reason `dynamics` does, on the other axis: `compare` scores one instrument at a time, so a kit whose every piece is individually plausible can still have its tom series half as wide as the reference's. Printed rather than gated, because the gate is a median across all 282 hits and folding a six-tom finding into that loses it; `--w-kit` is where the same measurement drives a search ([loss.md](loss.md#percussion-terms)).

`dynamics` exists because `compare` scores one velocity at a time, so a model whose every velocity is individually plausible can still have the wrong **dynamics** — the axis a pianist actually plays. It tabulates the swing rather than the absolute value: level in dB, and the two partial bands relative to the fundamental, so a swing that is merely "louder" is separated from one that is "brighter". A physical model gets this out of its excitation solver rather than out of a curve, so a mismatch here is structural and `--diagnose` is where to take it.

### What `profile.py measure` measures that `metrics.py` does not

Six things, none of which the harmonic-series assumption can express. All six are measured for every instrument, and what varies between instruments is which of them is the headline rather than which are taken:

- **Inharmonicity** — a stiff string puts partial *n* at `n·f0·√(1+Bn²)`. Fitted, not assumed, by iterating the search window down as the estimate improves. On the captured C7, B is smallest around C2 (7.6e-5) and rises toward both ends — the shape a real piano has. The harpsichord capture reads 1.5e-6 to 5.7e-6, three orders of magnitude lower, which is the measurement saying its strings are essentially harmonic. Above roughly C6 there are too few partials left under the Nyquist frequency to fit two parameters, and those notes are reported as unfitted rather than as a number; the summary curve names the notes it stopped at, because a curve that quietly stops short reads afterwards as one that covered the keyboard.
- **Stretch tuning** — cents from equal temperament. The grand capture reads the Railsback curve, −14.5 c at A0 rising to −2.7 c at C4; the harpsichord reads inside ±1.6 c everywhere, which is the same measurement reporting a plain equal temperament. A model tuned to exact equal temperament beats against a reference that is not, and the beating is the first thing anyone hears.
- **Double decay** — the knee where the fast initial fall gives way to the aftersound, found by searching the breakpoint rather than assuming one. Where it falls is set by how fast the strings of a unison drift apart, so a model with one string cannot have it.
- **Damper release** — note-off as a damper landing on a moving string. A row that never fell 40 dB inside the capture's tail is shown and left out of the median, since the number recorded for it is the length of the window.
- **Tone-to-noise** — the mechanism against the string. A model with the partial stack right and no action noise reads cleaner than any recording, and every spectral metric scores that as an improvement. The partial window is the wider of ±2 % and three FFT bins, because a relative-only window collapses below one bin in the bass and reports a clean low note as a noise burst.
- **Velocity response** — the level range from the softest blow to the hardest, and whether it is monotonic. On a plucked instrument this is most of the identity: the harpsichord capture reads 5.9–6.6 dB on three of its four slots against a piano's thirty, and a model given the wrong one of those is wrong by 20 dB in a way no timbre metric reports, since every one of them normalises each note by its own fundamental.

### `profile.py compare` renders the model dry and does not correct for the reference's room

That correction exists, and it lives in the `--oracle-wav` path ([Ambience](oracles.md#ambience-references-that-come-with-a-room)). So a comparison against a timbre recorded in a space reads the space: the model comes back short in decay and fast in damper release by an amount that is the room, not the voice. Compare against a slot known to be dry, and treat a wet one as a listening reference rather than a numeric one.

## Holding the comparison to what it measured (`--gate`)

`profile.py compare` ends with a per-dimension summary, and it reports **two** numbers per dimension rather than one:

```
                                                  median  |median|  rows
  tuning vs the reference (cents)                    -0.15      0.91    12
  partial stack h2-h6 vs h1 (dB)                     +4.04     13.58    12
  brightness (% of the reference centroid)           -3.50     37.27    12
```

The second column exists because the first cannot fail on a defect that is symmetric across the keyboard. The brightness row once read +0.16 % while individual notes were between 26 and 660 % out — the bass dark by as much as the treble was bright — and the median of the signed errors said the voice was correct to a sixth of a percent.

```sh
profile.py compare --notes 36,60,84 --write-gate reference/piano_gate.json
profile.py compare --notes 36,60,84 --gate reference/piano_gate.json   # exit 1 on a regression
```

Both statistics are bounded, and the gate exits 1 when either is exceeded. What it exists to catch is not a bad voice but **a change that improves one dimension by breaking another**, which is the shape most of this work takes: widening the prompt-decay profile fixes the level on every note between F#2 and F#5 and brightens the same notes by 35 to 60 points of centroid. Both are real, and whoever makes that trade should decide it rather than discover it in a later listening test. Bounds are written at `--margin` (1.25× by default) with a floor under each, deliberately loose — a gate that fails on measurement noise gets switched off, and a switched-off gate catches nothing. Re-record only in the same change as the behaviour that justifies the new numbers.

**One of the dimensions has to be able to see gain.** Every other column is normalised — a band profile against its own loudest band, a crest against its own RMS, a decay against its own peak — which is what makes them measure timbre and also what makes all of them blind to output level. Rewriting eighteen of the kit's levels moved not one of them by a digit. `level` is the per-hit `peak_dbfs` against the reference's, and `vel_range` is not a substitute: it is a span, and a span cancels an offset by construction.

## A second reference, and where the two agree (`agree`)

```sh
profile.py agree --config capture/drums.json          # the capture vs fluidsynth
```

Every bound in a `_gate.json` was decided by hand from one reference, and one reference cannot say which of its numbers are the instrument and which are its own voicing. That is not a documentation gap: working from a single reference, a tambourine was on its way to being pushed 31 dB down — inaudible — because that is where the one reference put it.

`agree` measures a second, independent reference over the same grid (fluidsynth over the bundled SoundFont, already wired for the fit's oracle) and reports, per dimension, how many hits the two agree on within a stated tolerance. It writes nothing. It is not a better reference and is never a target; the only question asked of it is where it and the capture say the same thing. **A dimension the two agree on has a target in it and is worth gating. One they do not is a voicing decision — measure it, print it, and do not fit to it**, because the number a single reference gives there is that library's opinion and a fit handed it will follow it as far as it goes.

## Listening to the result

```sh
make_audition.py                                    # render the phrases
python tools/audition/serve.py <audition-dir>       # the directory it wrote
```

`--model-only` skips the reference renders and auditions the model against itself; `--timbres`, `--only`, `--takes` and `--program` narrow or override what the config named.

Four phrase sets, named by the capture definition's `takes`: `piano`, `harpsichord`, `drums` and `sustained`. A set belongs to an instrument rather than to the tool, and each covers what its own instrument is hard to get right and no metric here reports.

- **`drums`** — the mute group (an open hat choked by a closed one and by the pedal), sixteenths landing on their own ring, a flam and a roll, a tom fill in the capture's *measured* pitch order, ten seconds of cymbal wash, and a bar of eights. Every one of those lives in the relation between hits, which a probe striking one note every two seconds cannot produce — not because the search failed but because the question was never put.
- **`sustained`** — what happens *between* notes on anything bowed or blown: a slurred scale, repeated notes each articulated, leaps across the compass, and a swell under CC11. The whole harness measures isolated held notes, which is the least characteristic thing a wind or a bowed string does. The register comes from the capture's own program, so the set is a shape rather than one instrument's notes.

`make_audition.py` renders the same phrases through libsonare and through each captured timbre, at one shared gain per take so the level difference between them survives, and writes the manifest [`tools/audition`](../../audition/README.md) reads. The phrases are chosen for what a metric will not report: a chord that exposes inharmonicity as beating, an arpeggio, a note struck again while it is still ringing, and a passage under the sustain pedal — couplings between strings rather than properties of one, which the per-note analysis in `metrics.py` never sees.
