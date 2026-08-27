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
| `identify` | say what is loaded in each slot of a multitimbral rack | `--channels` (`1-16`), `--velocity` (100) |
| `calibrate` | measure the host settings this plugin needs | `--note` (60), `--velocity` (100) |
| `corpus` | render the note × velocity × timbre grid, resumable | `--no-resume`, `--limit` |
| `verify` | re-read the corpus and report what is wrong with it | — |

All four: `--config` (default `capture/piano.json`), `--out` (default `.cache/voicematch/capture/<id>`), `--verbose`.

**`identify` is for the case where the reference is a rack and a capture needs a second timbre out of it.** A rack answers on sixteen channels and publishes no slot names, so the only way to learn what is loaded in one is to play it. Two stages, in the order that makes the second cheap:

1. **Does the slot answer a key with that key's own pitch?** `metrics.harmonic_share` measures the share of the render's power sitting on the harmonic series of the note that was pressed, over an octave ladder. Near 1 and the slot is an instrument, so no drum render is spent on it. The discriminator is deliberately not a fact about drums: the tells recorded for a kit already captured describe *that* kit, and a rule written from them would find only more of it. Both ends are measured rather than assumed — the captured concert grand reads 0.95 to 1.00 across its whole compass, the captured kit's own slot 0.05 to 0.15 on the same three keys.
2. **How far are the slot's diagnostic hits from the kit already measured?** One note per family declared in `groups`, scored as the median absolute band-profile difference against the committed `reference/<id>.json` rows for the same note and velocity. The criteria come from the reference, not from prose about what a kick looks like.

**Leave the capture's own timbre channels in the probe.** They are the control, and they score 0.00 dB by construction — without a number for a slot that *is* the kit, a table of distances says nothing about what a small one means.

**A timbre's `channel` says what its note numbers MEAN; `slot_channel` says where the rack keeps it.** Channel 10 is what makes a note number select an instrument rather than a pitch, and `profile.is_percussion` reads it to choose the metric set for the whole capture. The two coincide right up until a rack holds a kit somewhere other than slot 10, and then no single field can serve: addressing the slot on 10 records whatever else lives there, and declaring the kit on 15 sends a drum map through the pitched measurements and reports it as a successful capture. Where a rack keeps a slot is a property of the product rather than of the method, so `slot_channel` goes in the untracked overlay beside the preset. Absent, `channel` addresses the slot, which is what every capture written before the split does.

**The `layout` column is the question a distance cannot answer: a kit, or one instrument mapped across the keys?** It counts how many distinct peak bands the diagnostic notes land on. A slot holding nothing but congas sits some number of decibels from the kit exactly as a rival kit does, so a table of distances alone will happily nominate it — and taking it as a second timbre would score a whole map against one drum. Stage one has already removed anything pitched, so a slot answering most of its notes with their own band is a kit and a slot answering all of them with one or two is a single piece. On the rack this was written against, all sixteen slots read as kits, which is not what the distances suggested when read on their own.

It reports and never edits the capture definition. Which slot becomes a second timbre is a decision, and the evidence for it belongs in front of a person.

## `profile.py` — corpus → reference profile, and the comparison against it

| subcommand | what it does | its own flags |
|---|---|---|
| `measure` | corpus WAVs → `reference/<id>.json` | — |
| `render-grid` | render the model over the capture's grid as one more timbre | `--timbre` (default `model`) |
| `compare` | diff the model's grid against the reference, per dimension — and, on a kit, per family relation | `--gate`, `--write-gate`, `--margin` (1.25) |
| `agree` | measure a second reference and report where the two agree | — |
| `dynamics` | tabulate the pp→ff swing rather than one velocity at a time | — |
| `takes` | measure the phrase set — what happens *between* notes, which a single-note grid cannot excite | `--only`, `--archive` |
| `status` | the readiness row, and the command each gap needs | `--all`, `--archive` |
| `room-match` | what libsonare's own CC91 send and GS tank would have to be to sit in the reference's room | `--take`, `--archive`, `--verbose` |

All: `--config`, `--corpus`, `--profile`, `--program`. `compare` / `agree` / `dynamics` also take `--timbre` and `--notes`.

**`takes` on a wet capture places the model in the reference's room first.** The model always renders dry — `write_smf` writes CC91 0 — so against a reference recorded in its building every tail figure would be a dry signal read against a wet one, and all of them would land outside the references' spread for that reason and not because the voice is wrong. Measured on the church organ before the correction: the tail fell at 78 dB/s against the references' 15 to 17, and its four band levels missed by 11 to 25 dB; after it, the decay is 23 dB/s and the tail tonality is inside their spread. A capture's own `dry` flag decides whether the correction runs at all — nothing in the audio can tell a building from an instrument's own long release, and guessing wrong convolves an invented room onto every figure.

One room per reference, because two references are two buildings and placing the model in one of them would score it against the other's; a figure is flagged only when it is outside their range in *every* room the model was placed in. One IR per reference for the whole run, measured on the first phrase that can support the measurement and reused by the ones that cannot: a room belongs to the session that recorded it, not to the phrase, and a phrase of short notes cannot measure one itself (`Room.gated`). A run that reaches no such phrase — `--only` on a staccato take — reports the take as skipped rather than printing figures that all describe the building.

**Ambience splits two ways, and the split is deliberate.** A *metric* has to read the instrument rather than the building, so `compare` and `takes` render the model dry and place it in the reference's measured space before measuring. A *listening page* has to be the product, so `make_audition.py` leaves the GS sends at their power-on values instead — see [audition.md](audition.md). `room-match` is the third question: not how to remove the room from a measurement, and not what the model sounds like in its own, but what libsonare would have to be told to sit in the reference's. It answers in the only terms the library takes — a CC91 value and a tank decay. Measured on the church organ's `single-long`: the two sampled references' naves are RT60 3.19 s and 3.69 s, the shipped path reaches 2.5 s, and `reverb_decay` 0.773 lands inside their span at 3.60 s with the send pulled back to CC91 30 — residual 0.44, well inside the 1.5 above which the tank is reported as unable to reach the space at all. The tank decay is a host-wide setting rather than a per-program one, so only the send half of that answer is something a bank can carry.

Three things about that number are worth knowing before it is written anywhere.

- **`send_factor` multiplies the shipped weight; it is not the weight.** The probe renders through the library, so the program's `gm_fallback_sends` weight is already inside every measurement — the search is moving a CC91 that has been through it. The organ ships at 2.2 and the search lands on CC91 30, which is 2.2 × 30/40 = 1.65, not the 0.75 an absolute reading would write. An absolute weight would need the shipped value, and the only non-mirror source for it is what the library reports under `SONARE_TUNING_DUMP`.
- **The search is scored against the span the references cover, not against one of them.** Aimed at whichever came first, it chose a 2.88 s tank over a 3.60 s one — and 2.88 is outside both, while a pairwise distance cannot see that because 2.88 genuinely is nearer to 3.19 than 3.60 is.
- **The decay grid is spaced uniformly in RT60 rather than in decay.** A tank is a feedback loop, so its RT60 goes as `-1/ln(decay)` and blows up near unity: on an even grid the shipped 0.70 and the next step 0.805 measured 2.53 s and 4.28 s with nothing in between, and both references sit in that gap.

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
| `struck` | per band over the aftersound: how many things ring, and where the top went | `--overrides` |
| `attack` | per band over the first quarter second: how far into the strike each band's energy sits | `--overrides` |
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
