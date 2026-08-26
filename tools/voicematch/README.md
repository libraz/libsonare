# voicematch — physical-model voice tuning harness

Renders the same MIDI through two synths and reports per-note timbre deltas, so physical-model voicing can be tuned against a reference ("oracle") instead of by ear alone.

- **Model side** — libsonare's GM fallback bank: `Project.import_smf` → `bounce_with_sf2_instrument` with **no SoundFont loaded**, which forces every program through `gm_fallback_map` → NativeSynth physical voices. This is exactly the code path under tuning. The dylib is loaded via `SONARE_LIB_PATH` (defaults to `build-python-shared/lib/libsonare.dylib`), so it always tests the working tree.
- **Oracle side** — fluidsynth over a GM SoundFont, a WAV you rendered yourself, a captured single-note corpus, or an AudioUnit plugin on this machine. All four go through one interface; see [docs/oracles.md](docs/oracles.md).

## Where to look

| you want to | go to |
|---|---|
| run something | [Command reference](#command-reference) below |
| know what a pattern plays, or how a drum probe differs | [docs/probes.md](docs/probes.md) |
| know what a reported field means | [docs/metrics.md](docs/metrics.md) |
| pick or set up a reference | [docs/oracles.md](docs/oracles.md) |
| know what a `--w-*` term prices, or what the defaults are | [docs/loss.md](docs/loss.md) |
| run, read or trust a fit | [docs/fitting.md](docs/fitting.md) |
| capture a new instrument, or read a reference profile | [docs/capture.md](docs/capture.md) |
| generate training pairs for an amortized inverse | [docs/dataset.md](docs/dataset.md) |
| compare spectrograms rather than summaries | [shape/README.md](shape/README.md) |

## How everything is invoked

Everything runs through the bindings' rye environment, **from the repo root**:

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/<script>.py …
```

Two exceptions and one precondition:

- **`shape` is a package, not a script.** It runs as `PYTHONPATH=tools/voicematch python -m shape …`, still from the repo root.
- **`tools/audition/serve.py`** is a plain server and needs no environment.
- **Rebuild the dylib before comparing** when the synth code changed. `autofit.py` builds its own isolated tree and does not need this; `voicematch.py` and `profile.py` read whatever `SONARE_LIB_PATH` points at.

```sh
cmake --build build-python-shared --target sonare_shared -j8
```

## The tuning loop

```sh
# 1. Edit voice code (src/midi/synth/...), then rebuild the shared library.
cmake --build build-python-shared --target sonare_shared -j8

# 2. Compare.
rye run --pyproject bindings/python/pyproject.toml \
    python tools/voicematch/voicematch.py compare --programs 40

# 3. Read the deltas, adjust the voice, repeat.
```

Outputs land in `out/p<NNN>_<pattern>/`: `model.wav`, `oracle.wav`, `notes.mid`, `report.txt`, `report.json` (machine-readable, for driving the loop from an agent). Listen with `afplay out/p040_sustain/model.wav`.

## Calibrating a voice, start to finish

The order matters, and each step exists because the one before it cannot answer the question the next one asks.

Steps 1–3 are the only ones that need the reference plugin, and they are already done for a captured instrument: `reference/<id>.json` is committed, so **a voice can be measured, fitted and diagnosed against the grand, the harpsichord, the kit or the organ from a plain clone**, on any platform, with nothing installed. Skip to step 4.

```sh
# 1. What the plugin needs from its host. Measured, not guessed: a sampler that
#    streams from disk drops notes at a settle time that looks generous.
capture.py calibrate --config capture/harpsichord.json

# 2. The grid, one note per process, resumable.
capture.py corpus  --config capture/harpsichord.json
capture.py verify  --config capture/harpsichord.json

# 3. The reference: WAVs -> the numbers a voice is fitted to. Committed, so
#    this is the last step that needs the plugin.
profile.py measure --config capture/harpsichord.json

# 4. The model over the same grid, as one more timbre of the corpus, so every
#    tool that reads a corpus reads the model with no special case.
profile.py render-grid --config capture/harpsichord.json

# 5. Where the voice stands, dimension by dimension.
profile.py compare --config capture/harpsichord.json --timbre baroque

# 6. Values. --spec auto derives the knobs from the library's own catalogue.
autofit.py --spec auto --program 6 --optimizer cmaes --workers 8 --validate-notes 41,65,84

# 7. What the fit could not reach, and whether anything could.
autofit.py --spec auto --program 6 --diagnose --workers 8

# 8. Record what is now worth holding, in the same change as the behaviour.
profile.py compare --config capture/harpsichord.json --timbre baroque \
    --write-gate reference/harpsichord_gate.json
```

Steps 6 and 7 alternate with physics work rather than repeating: a fit converges to the best point reachable from where it started, and a mechanism added to the model moves the anchor, so the next fit starts somewhere new.

**The capture definition names the instrument, and everything downstream reads it from there.** `program` is the GM program the model answers with, `takes` is the phrase set the audition page plays, and `dimensions` narrows what `compare` gates on. Splitting those across a config, a command line and a constant in the source is how a harpsichord ends up measured against program 0 without a word of complaint.

---

# Command reference

Every flag below exists in the code as written. `--help` carries the full wording; what follows is the index.

## `voicematch.py` — compare, export a probe, match a room

```sh
voicematch.py compare --programs 40                      # one program, 'sustain' pattern
voicematch.py compare --programs 40-47                   # a range
voicematch.py compare --programs 40,42,71                # a list
voicematch.py compare --programs 71 --pattern velocity
voicematch.py compare --programs 40 --notes 55,67,79     # override probe pitches
voicematch.py compare --programs 40 --render-only        # WAVs only, no analysis
voicematch.py compare --programs 0 --drum-note 38        # a percussion instrument, not a pitch
voicematch.py export-probe --programs 0                  # the probe SMF for an external synth
voicematch.py compare --programs 0 --oracle-wav ref.wav  # score against that rendering
voicematch.py room-match --programs 19 --oracle-wav ref.wav   # what sends reproduce its room
```

| subcommand | what it does |
|---|---|
| `compare` | render both synths and report timbre deltas |
| `room-match` | measure the oracle's room and report the libsonare sends that reproduce it |
| `export-probe` | write the probe SMF so an external synth can render the oracle side |

**Probe selection** — `compare`, `export-probe`, `room-match`:

| flag | default | |
|---|---|---|
| `--programs` | *required* | `'40'`, `'40-43'` or `'40,42,71'`. `room-match` uses only the first |
| `--pattern` | `sustain` | one of the eight in [docs/probes.md](docs/probes.md#patterns) |
| `--notes` / `--velocities` | the pattern's own | `compare` and `export-probe` only |
| `--drum-note N` | — | percussion instrument on the drum channel; `--programs` then selects the kit. `compare` and `export-probe` only |
| `--drum-gate-ms` | `50` | how long a drum note is held |

**Oracle** — `compare` and `room-match` (shared with `autofit.py`):

| flag | default | |
|---|---|---|
| `--sf2` | `assets/MuseScore_General.sf3` | oracle SoundFont |
| `--oracle-wav` | — | an externally rendered WAV of the probe |
| `--oracle-offset` | *estimated* | seconds of lead-in to strip |
| `--oracle-no-align` | off | take the WAV as-is instead of aligning it to the score |
| `--oracle-resample` | off | resample a WAV whose rate differs (an error otherwise) |
| `--au` | — | an AudioUnit instrument, by name or `type:subtype:manufacturer` |
| `--au-preset` | — | a `.vstpreset` path, or a unique fragment of one |
| `--au-param` | — | `Name=value`, repeatable |
| `--au-dry` | off | switch off every effect section the plugin advertises |
| `--au-settle-ms` | `4000` | main-thread time before the first note |
| `--au-no-realtime` | off | drive the plugin as fast as it will go (drops notes on a streaming sampler) |
| `--au-no-warmup` | off | record the plugin's first note instead of discarding one |
| `--au-gm` | off | keep the probe's program change (stripped by default) |
| `--au-no-cache` | off | re-render instead of reusing an identical earlier render |

**`compare` only:**

| flag | default | |
|---|---|---|
| `--room` | `auto` | `auto`: measure the oracle's reverberation and place the model in a matching space. `none`: compare as rendered |
| `--model-sends R,C,D` | `0,0,0` | CC91/93/94 for the model render. `gs` keeps libsonare's power-on ambience |
| `--render-only` | off | write WAVs but skip analysis |

**`room-match` only:** `--verbose` prints every search point.

## `autofit.py` — fit calibration constants against the oracle

```sh
autofit.py --spec auto --program 40 --notes 55,67 \
    --optimizer cmaes --max-evals 400 --workers 6 \
    --screen --stages --validate-notes 48,60
```

Full treatment in [docs/fitting.md](docs/fitting.md); the terms it minimises are [docs/loss.md](docs/loss.md).

**Target:**

| flag | default | |
|---|---|---|
| `--spec` | *required* | a knob-spec JSON, or `auto` to derive the list from the library's catalogue |
| `--program` | *required* | the GM program, or the drum kit with `--drum-note` |
| `--bank` | `0` | GS variation bank — its own patch, with its own knobs |
| `--drum-note` / `--drum-gate-ms` | — / `50` | fit a percussion instrument instead |
| `--pattern` | `sustain` (`drum` with `--drum-note`) | |
| `--notes` / `--velocities` | the pattern's own | |
| `--dump-knobs` / `--program-only` | off | list every knob and exit; `--program-only` narrows to this program's patch fields |

**Oracle:** the shared oracle block above, plus

| flag | default | |
|---|---|---|
| `--corpus` | — | score against a captured grid: the directory `capture.py corpus` wrote |
| `--corpus-timbre` | first in the manifest | which timbre of that corpus |
| `--room` | `auto` | as for `compare` |
| `--mono-mode` | `mean` | `mean` / `left` / `loudest` — how a stereo render is reduced |
| `--band-edge-hz` | *resolved from the oracle* | analysis bandwidth handed to every model render |

**Search:**

| flag | default | |
|---|---|---|
| `--optimizer` | `coord` | `coord` (golden-section coordinate descent) or `cmaes` |
| `--max-evals` | `30` | raise it well past this when the spec is runtime knobs only |
| `--per-knob-evals` | `6` | golden-section budget per knob per pass (`coord` only) |
| `--population` / `--sigma0` / `--seed` / `--restarts` | `4+3·ln(n)` / `0.25` / `0` / `0` | CMA-ES |
| `--workers` | `1` | concurrent model renders; helps `cmaes` only, and a source knob forces it back to 1 |
| `--screen` / `--screen-threshold` | off / `0.002` | drop knobs that do not move the loss over their range |
| `--stages` | off | fit excitation, then decay, then everything |
| `--grid POINTS` | off | enumerate the product of every knob (≤3) instead of fitting |
| `--diagnose` | off | report what the residual is made of and what no knob reaches |

**Hold-out and output:**

| flag | default | |
|---|---|---|
| `--validate-notes` | — | score on notes the fit never saw; must be disjoint from `--notes` |
| `--validate-velocities` | — | the drum equivalent, since a drum note has no register |
| `--validate-oracle-wav` | — | the reference for the held-out probe; required with `--oracle-wav` |
| `--out` | — | write knob values, losses, overrides and validation to a JSON path |
| `--dry-run` | off | restore pristine and skip writing the best values |

**Weights** — every one defaults to `None`, and [`toneclass.py` fills the gap from what the instrument is](docs/loss.md#a-weight-nobody-gave-comes-from-what-the-instrument-is):

`--w-harm` `--w-modes` `--w-mod` `--w-cents` `--w-tnr` `--w-env` `--w-init` `--w-slope` `--w-tail` `--w-crest` `--w-level` `--w-hf` `--w-lf` `--w-stiff` `--w-dyn` `--w-mss` `--w-band` `--w-bdecay` `--w-kit`

| flag | default | |
|---|---|---|
| `--n-harm` | `10` | harmonics counted in the L1 timbre term |
| `--max-level-drift-db` | `6.0` | how far the winner may move the whole-grid level before `level` charges it |
| `--flat-partial-weighting` | off | score every partial equally, without audibility weighting |
| `--raw-loss` | off | weight terms in their own units instead of normalising each to the start point |

**Build:** `--build-dir` (default `build-autofit`, and `build-python-shared` is refused), `--jobs` (default 8), `--cmake`.

## `capture.py` — record a reference grid from a plugin

macOS + the plugin + [aubounce](https://github.com/libraz/aubounce). See [docs/capture.md](docs/capture.md).

| subcommand | what it does | its own flags |
|---|---|---|
| `calibrate` | measure the host settings this plugin needs | `--note` (60), `--velocity` (100) |
| `corpus` | render the note × velocity × timbre grid, resumable | `--no-resume`, `--limit` |
| `verify` | re-read the corpus and report what is wrong with it | — |

All three: `--config` (default `capture/piano.json`), `--out` (default `.cache/voicematch/capture/<id>`), `--verbose`.

## `profile.py` — corpus → reference profile, and the comparison against it

| subcommand | what it does | its own flags |
|---|---|---|
| `measure` | corpus WAVs → `reference/<id>.json` | — |
| `render-grid` | render the model over the capture's grid as one more timbre | `--timbre` (default `model`) |
| `compare` | diff the model's grid against the reference, per dimension — and, on a kit, per family relation | `--gate`, `--write-gate`, `--margin` (1.25) |
| `agree` | measure a second reference and report where the two agree | — |
| `dynamics` | tabulate the pp→ff swing rather than one velocity at a time | — |

All: `--config`, `--corpus`, `--profile`, `--program`. `compare` / `agree` / `dynamics` also take `--timbre` and `--notes`.

### The gated dimensions, and what a bound may not go under

A pitched `compare` reduces to seven dimensions: `stretch` (tuning), `decay`, `attack`, `stereo`, `balance` (the h2–h6 stack against h1), `centroid_pct` (brightness) and `vel_range`. `tnr` joins them on any profile measured since the tone-to-noise column existed. Two of them answer questions the rest of this file cannot, because everything else here is computed on a mono mix of a sustain window:

- **`attack`** is the time to the envelope's peak. On a drum that is the strike; on a struck string the hammer is over milliseconds before the board reaches full level, so the number is the bloom after it — which is what separates a note that sinks in from a thump.
- **`stereo`** is `1 - |channel correlation|` over the held note. A voice that returns one signal to both legs scores exactly `0.0` and is mono however wide the reverb around it is. Nothing else measured here can see that, and a listening test finds it immediately.

`--write-gate` puts a floor under every bound, because a bound tighter than its dimension's own noise fails on the noise and then gets switched off. Where the reference holds several instruments of one kind, that floor is measured rather than chosen: the median disagreement **between** them is the tightest any model can be held to without failing on which one it was compared against. On the three concert grands that is 0.15–0.27 of width and 12.5–40 ms of attack.

### `make voice-gate`

Runs every gate that exists — one per `reference/<id>_gate.json` — against its capture, reading each gate's own timbre back out of it, and reports every voice outside its bounds rather than stopping at the first. It builds its own `build-autofit`, so it disturbs neither the Debug tree `ctest` reads nor the shared `build-python-shared`.

It is not in CI and not in `ci-local`. It renders a full grid per instrument, and re-recording a bound is a judgement about a trade that a CI job cannot make. **A gate is invalidated by its reference as well as by the voice**: re-measuring `reference/<id>.json` moves the numbers the bounds were recorded from, so a gate re-recorded before that re-measure is comparing against an instrument that no longer exists in the file.

## `shape` — compare the spectrogram instead of the summary

**A package, not a script** — it needs `tools/voicematch` on `PYTHONPATH`:

```sh
PYTHONPATH=tools/voicematch python -m shape score --capture piano --corpus <dir>
PYTHONPATH=tools/voicematch python -m shape fit   --capture piano --corpus <dir> --knobs dump.txt
```

| subcommand | what it does | its own flags |
|---|---|---|
| `score` | one number and its seven terms | `--overrides` |
| `fit` | search the knob space against the spectrogram loss | `--knobs` *(required)*, `--namespaces`, `--deny`, `--start`, `--out`, `--passes` (4) |
| `ablate` | which of a fitted set actually earned its place | `--knobs` *(required)*, `--namespaces`, `--overrides` *(required)* |
| `probe` / `purity` / `admittance` | per-cell, per-partial and driving-point readings | `--overrides` |
| `takes` | read a rendered audition page rather than the corpus | `--page` *(required)* |

Common: `--capture` (`piano`), `--corpus` *(required)*, `--timbre`, `--notes`, `--velocities`, `--lib`, `--cache` (`/tmp/voicematch-shape`), `--no-bed`, `--workers` (7). Details in [shape/README.md](shape/README.md).

## `dataset.py`, `make_audition.py`, `render_corpus.py`

```sh
dataset.py --program 0 --samples 100000 --workers 8      # (knob vector -> measurement) pairs
make_audition.py                                          # the listening set
python tools/audition/serve.py <audition-dir>             # serve it
render_corpus.py --programs 0,40,73 --tag baseline        # a whole-bank phrase render
compare_metrics.py <dir-a> <dir-b>                        # diff two render_corpus.py runs
```

| script | flags |
|---|---|
| `dataset.py` | `--program` *(required)*, `--bank`, `--pattern`, `--notes`, `--velocities`, `--samples` (1000), `--seed`, `--workers` (8), `--build-dir` (`build-tuning`), `--out` |
| `make_audition.py` | `--config`, `--out`, `--timbres`, `--model-only`, `--only`, `--takes`, `--program` |
| `render_corpus.py` | `--out` (`/tmp/voicematch_corpus`), `--programs`, `--fluidsynth`, `--tag` (`model`) |

## Environment

| variable | what it does |
|---|---|
| `SONARE_LIB_PATH` | the dylib every model render loads (default `build-python-shared/lib/libsonare.dylib`) |
| `SONARE_VOICEMATCH_ROOT` | moves the untracked scratch root (captures, auditions, datasets) off the checkout |
| `VOICEMATCH_SF2` | the oracle SoundFont, when `--sf2` is not passed |
| `AUBOUNCE` | the aubounce binary; otherwise `PATH`, then a sibling `../aubounce` checkout |

`SONARE_TUNING_OVERRIDES` and `SONARE_TUNING_DUMP` are set by `autofit.py` and `catalogue.py` for their own child processes. Setting them by hand affects only what you launch yourself.

---

# Files

- `voicematch.py` — CLI driver (`compare`, `export-probe`, `room-match`)
- `patterns.py` — note patterns + per-GM-program register table, the per-note drum probe spacing, and the drum phrase sequences
- `toneclass.py` — what kind of sound a GM program makes, and the three things that follow from it: which notes it is probed at, which metric set scores it, and which terms carry weight
- `render_model.py` / `render_oracle.py` — the model renderer, and the oracle (fluidsynth, an external WAV with score alignment, or a plugin)
- `au_oracle.py` — an AudioUnit instrument as the oracle, hosted by aubounce, with the guards a disk-streaming sampler needs and an on-disk render cache
- `metrics.py` — per-note analysis and deltas
- `room.py` — ambience: measure a reference's space, put the model in it, translate it back into libsonare's sends
- `smf.py` — minimal type-0 SMF writer (single source of truth for both sides)
- `wavio.py` — stdlib WAV I/O: 16-bit PCM out, any common format in
- `gm_names.py` — GM program labels
- `capture.py` / `profile.py` / `capture/` / `reference/` — capturing a reference corpus from a plugin and reducing it to committed measurements; the audio itself stays untracked. `profile.py render-grid` puts the model into the same corpus as one more timbre, so nothing downstream needs a special case for it
- `make_audition.py` — the listening set for [`tools/audition`](../audition/README.md)
- `render_corpus.py` / `compare_metrics.py` — a whole-bank phrase render and a diff of two of them; a broad before/after sweep rather than a per-note comparison
- `assets/`, `out/` — gitignored (soundfont download, render artifacts)

The fitter is `autofit.py` plus the modules it drives, one per stage of a fit:

- `autofit.py` — the CLI and the loop: probe resolution, the oracle, the `Evaluator` the optimisers minimise, the held-out check, and the `_render_metrics` subprocess each evaluation runs in
- `build_lib.py` — configuring and building the isolated library a fit renders through
- `catalogue.py` — what the library reports about its own knob space under `SONARE_TUNING_DUMP` (defaults, program→patch map, clamp bounds), plus the `SONARE_TUNABLE` declaration scan the write-back needs
- `corpus.py` — the captured single-note grid as a probe timeline and as the oracle for one: the bridge between the capture `profile.py` reports against and the search `autofit.py` runs
- `knobs.py` — what a fit may move and over what range: the spec forms, the clamp-derived search ranges, `--spec auto`, and the one rule for what counts as sitting on a bound
- `loss.py` — from a render to the number being minimised: `probe_rows`, `skeleton_note`, the harmonic and percussion term sets, the level terms, and the start-point normalisation
- `optimizers.py` — coordinate descent with a golden-section line search, and CMA-ES with IPOP restarts
- `staging.py` — cutting the problem down: knob screening and the excitation/decay/all staged fit
- `diagnose.py` — the same probe read per term instead of per loss: what the residual is made of, and which of it no knob reaches
- `dataset.py` — the corpus of (knob vector → measurement) pairs an amortized inverse would train on
- `writeback.py` — putting a fitted value back: literal splicing, the program table, the drum table
- `report.py` — the end-of-run report and the diff it applies
- `specs/` — knob spec JSONs; `example.json` shows all three knob forms, the rest are hand-tuned per-instrument sets. `--spec auto` needs none of them.
- `shape/` — the second objective: two log-frequency spectrograms compared cell by cell. Its own CLI and its own README.

## Two objectives over one corpus

`loss.py` reduces each render to about twenty scalar summaries per note and `shape/` compares the two log-frequency spectrograms cell by cell. They are two objectives over the same corpus, and which one a value should be fitted to is a decision this harness has not made.

That is deliberate for now, and the reason is that neither is a superset of the other. `ShapeLoss` is bound to a corpus, a measured noise bed and one analysis geometry, and two of its terms — `invariance` and `recurrence` — need the whole note set at once, which is a different shape from an evaluator that scores one probe render at a time. Merging them would change what every existing fit minimises, and the merged objective would have to be re-validated against every reference in `reference/` before anything could be written back through it.

What has been made consistent is the ruler. Both now weight frequency logarithmically — `--w-mss` by 1/f per bin, `shape/spectro.py` by construction — so the two no longer read the same renders through different frequency scales, which was the one way they could disagree without either being wrong.

The division of labour, until that decision is made:

- `autofit.py` is the objective of record. It is what writes values back, what `--diagnose` reads, and what a gate is held against.
- `shape/` is where a residual goes to be *identified*: which cell, which partial, whether a ring recurs across notes. Its terms name repairs; they do not currently set constants.

## Tests

```sh
python -m pytest tools/voicematch/
```

One file per module, and none of them renders anything.

- `test_toneclass.py` covers everything that is not a stiff string — the measured partial series, the movement set, audibility weighting, the drum pitch and its overshoot, the band-validity rule, the capture's measured bandwidth and the low-end balance — all on synthesised signals, since the captured corpora cannot be committed and a test that needed one would be a test that never runs.
- `test_autofit.py` covers the range rules, loss normalisation, stage classification, write-back path translation and the two guards that prove a probe reached the code.
- `test_diagnose.py` gives every verdict a case, including the two that look identical when only improvement is measured.
- `test_profile.py` covers the measurements that are not tied to one instrument, the captured-to-model note map, and the dimension that can see gain.
- `test_shape.py`, `test_capture.py`, `test_dataset.py`, `test_room.py`, `test_smf.py`, `test_wavio.py` cover their own modules.
