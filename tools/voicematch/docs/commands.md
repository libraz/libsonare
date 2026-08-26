# Command reference

Every flag below exists in the code as written. `--help` carries the full wording; what follows is the index. Everything is invoked from the repo root through the bindings' rye environment — see [the README](../README.md#how-everything-is-invoked) for the wrapper and its two exceptions.

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
| `--pattern` | `sustain` | one of the eight in [probes.md](probes.md#patterns) |
| `--notes` / `--velocities` | the pattern's own | `compare` and `export-probe` only |
| `--drum-note N` | — | percussion instrument on the drum channel; `--programs` then selects the kit. `compare` and `export-probe` only |
| `--drum-gate-ms` | `50` | how long a drum note is held |

<a id="oracle-flags"></a>

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

What each of those references is, and what it brings with it, is [oracles.md](oracles.md).

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

Full treatment in [fitting.md](fitting.md); the terms it minimises are [loss.md](loss.md).

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

**Oracle:** the [shared oracle block](#oracle-flags) above, plus

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

**Weights** — every one defaults to `None`, and [`toneclass.py` fills the gap from what the instrument is](loss.md#a-weight-nobody-gave-comes-from-what-the-instrument-is):

`--w-harm` `--w-modes` `--w-mod` `--w-cents` `--w-tnr` `--w-env` `--w-init` `--w-slope` `--w-tail` `--w-crest` `--w-level` `--w-hf` `--w-lf` `--w-stiff` `--w-dyn` `--w-mss` `--w-band` `--w-bdecay` `--w-kit`

| flag | default | |
|---|---|---|
| `--n-harm` | `10` | harmonics counted in the L1 timbre term |
| `--max-level-drift-db` | `6.0` | how far the winner may move the whole-grid level before `level` charges it |
| `--flat-partial-weighting` | off | score every partial equally, without audibility weighting |
| `--raw-loss` | off | weight terms in their own units instead of normalising each to the start point |

**Build:** `--build-dir` (default `build-autofit`, and `build-python-shared` is refused), `--jobs` (default 8), `--cmake`.

## `capture.py` — record a reference grid from a plugin

macOS + the plugin + [aubounce](https://github.com/libraz/aubounce). The procedure, the capture-definition fields and the failure modes are [capture.md](capture.md).

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

What each subcommand measures, which dimensions a `compare` gates on, and what `make voice-gate` runs are in [capture.md](capture.md#profilepy).

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

Common: `--capture` (`piano`), `--corpus` *(required)*, `--timbre`, `--notes`, `--velocities`, `--lib`, `--cache` (`/tmp/voicematch-shape`), `--no-bed`, `--workers` (7). Details in [shape/README.md](../shape/README.md).

## `dataset.py`, `make_audition.py`, `render_corpus.py`

```sh
dataset.py --program 0 --samples 100000 --workers 8      # (knob vector -> measurement) pairs
make_audition.py --program 40                             # the listening set for one voice
python tools/audition/serve.py                            # serve everything under the root
render_corpus.py --programs 0,40,73 --tag baseline        # a whole-bank phrase render
compare_metrics.py <dir-a> <dir-b>                        # diff two render_corpus.py runs
```

| script | flags |
|---|---|
| `dataset.py` | `--program` *(required)*, `--bank`, `--pattern`, `--notes`, `--velocities`, `--samples` (1000), `--seed`, `--workers` (8), `--build-dir` (`build-tuning`), `--out` |
| `make_audition.py` | `--program`, `--programs`, `--banks`, `--kits`, `--config`, `--out`, `--timbres`, `--model-only`, `--only`, `--calibrations`, `--variant`, `--lib`, `--title`, `--note`, `--reference-from`, `--archive-references` |
| `render_corpus.py` | `--out` (`/tmp/voicematch_corpus`), `--programs`, `--fluidsynth`, `--tag` (`model`) |

What `dataset.py` costs and what it is for is [dataset.md](dataset.md); what `make_audition.py` renders, and how a voice with no captured reference is auditioned anyway, is [audition.md](audition.md).

## Environment

| variable | what it does |
|---|---|
| `SONARE_LIB_PATH` | the dylib every model render loads (default `build-python-shared/lib/libsonare.dylib`) |
| `SONARE_VOICEMATCH_ROOT` | moves the untracked scratch root (captures, auditions, datasets) off the checkout |
| `VOICEMATCH_SF2` | the oracle SoundFont, when `--sf2` is not passed |
| `AUBOUNCE` | the aubounce binary; otherwise `PATH`, then a sibling `../aubounce` checkout |

`SONARE_TUNING_OVERRIDES` and `SONARE_TUNING_DUMP` are set by `autofit.py` and `catalogue.py` for their own child processes. Setting them by hand affects only what you launch yourself.
