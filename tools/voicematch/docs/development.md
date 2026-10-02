# Development commands and module map

The harness is maintained from the repository root. Use the rye invocation from the README and ask each script for `--help` before adding an option to a command; this page records command roles and invariants rather than copying parser tables.

## Commands

| command | purpose | build or data requirement |
|---|---|---|
| `status.py` | render the bank roll-up and next action; `--all` includes every voice | a tuning library adds engine and patch identity |
| `check_specs.py` | verify that every tracked spec key exists and still has a range | a tuning library |
| `liveness.py` | verify that spec knobs move a measured term; `--census` checks patch fields | a tuning library and renderable probes |
| `identity.py` | prove default identity across the bank and prove a new mechanism is reachable | base/head dylibs for cross-build identity, tuning head for reach |
| `reextract_check.py` | remeasure selected committed captures from scratch audio and report profile drift | captured WAV scratch data |
| `make_audition.py` | render model/reference listening pages, variants, and feedback targets | model library; plugin only for uncached references |
| `dataset.py` | generate knob-vector to full-measurement training pairs | a `BUILD_TUNING=ON` library and scratch output |
| `shape` | score, fit, prune, ablate, and inspect spectrogram-shape residuals | captured corpus and model library |

Examples:

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/status.py --all --lib build-tuning/lib/libsonare.dylib
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/check_specs.py --lib build-tuning/lib/libsonare.dylib
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/liveness.py --lib build-tuning/lib/libsonare.dylib --spec tools/voicematch/specs/piano.json
```

Synthetic recovery separates search and objective failures from physical-model claims. The fixtures use known parameters and production measurement paths; success does not prove that a C++ voice can reproduce an instrument.

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/fitting_recovery.py --optimizer cmaes --max-evals 72 --out recovery.json
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/family_recovery.py --family all --optimizer cmaes --max-evals 24 --out family-recovery.json
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/percussion_component_recovery.py --family snare --optimizer cmaes --max-evals 64 --out components.json
```

`percussion_component_recovery.py` checks controlled contact, rattle, hand-struck, cup, and withheld-velocity behavior in synthetic snare, metal, and kick families. `--without-evolution` removes the evolution terms for an objective comparison; either result remains a harness check, not a physical validation.

The native component probe measures whether selected contact, wire, and shell overrides are observable in an existing tuning-enabled build:

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/percussion_component_probe.py --build-dir build-autofit --drum-note 38 --out native-components.json
```

It compares ablations against the same patch and records overrides, hashes, waveform changes, and term coverage. Waveform differences use per-render RMS-normalized mono audio, so they do not measure raw component amplitude. All unchanged controls leave override reach inconclusive; a changed control establishes observability and does not establish agreement with an independent instrument reference.

The shape package uses `PYTHONPATH=tools/voicematch` and an explicit `BUILD_TUNING=ON` library. Produce `<tuning-dump>` by setting `SONARE_TUNING_DUMP=<path>` while rendering the selected patch with that same library; the file records consulted keys and compiled defaults.

```sh
PYTHONPATH=tools/voicematch rye run --pyproject bindings/python/pyproject.toml python -m shape --help
PYTHONPATH=tools/voicematch rye run --pyproject bindings/python/pyproject.toml python -m shape fit --capture <id> --corpus <capture-dir> --knobs <tuning-dump> --lib build-autofit/lib/libsonare.dylib --report shape-fit.json --out shape-overrides.json
PYTHONPATH=tools/voicematch rye run --pyproject bindings/python/pyproject.toml python -m shape prune --capture <id> --corpus <capture-dir> --knobs <tuning-dump> --lib build-autofit/lib/libsonare.dylib --overrides shape-overrides.json --report shape-prune.json --out shape-selected.json
```

`shape fit` and `shape prune` keep fit selection separate from final validation; inspect their reports before using an override set in an audition. `shape probe`, `struck`, `attack`, `onset`, `purity`, `admittance`, and `takes` identify residual structure without changing `autofit.py`'s write-back objective.

## Modules

| area | modules | contract |
|---|---|---|
| capture and profile | [capture.py](../capture.py), [corpus.py](../corpus.py), [profile.py](../profile.py), [bank.py](../bank.py), [patterns.py](../patterns.py) | define the reference grid, source identity boundary, model address, note map, room, rig, and profile rows |
| direct comparison | [voicematch.py](../voicematch.py), [render_oracle.py](../render_oracle.py), [render_model.py](../render_model.py), [au_oracle.py](../au_oracle.py), [smf.py](../smf.py), [wavio.py](../wavio.py), [room.py](../room.py), [rig.py](../rig.py) | render one probe, resolve an oracle, align audio, and place a model at the reference boundary |
| fitting | [autofit.py](../autofit.py), [build_lib.py](../build_lib.py), [catalogue.py](../catalogue.py), [knobs.py](../knobs.py), [optimizers.py](../optimizers.py), [staging.py](../staging.py), [eval_cache.py](../eval_cache.py) | resolve the library's knob space, search it, isolate the build, and cache only matching evaluations |
| measurements and objective | [metrics.py](../metrics.py), [metrics_hit.py](../metrics_hit.py), [metrics_texture.py](../metrics_texture.py), [loss.py](../loss.py), [loss_cells.py](../loss_cells.py), [loss_dimensions.py](../loss_dimensions.py), [loss_aggregate.py](../loss_aggregate.py), [loss_weights.py](../loss_weights.py), [fit_quality.py](../fit_quality.py) | measure valid cells, apply fixed engineering units and coverage rules, and report absolute quality beside relative loss |
| diagnosis and source edits | [diagnose.py](../diagnose.py), [liveness.py](../liveness.py), [writeback.py](../writeback.py), [report.py](../report.py), [identity.py](../identity.py) | distinguish connectivity from improvement, prove loader reach, and materialize only selected source values |
| bank operations | [status.py](../status.py), [signoff.py](../signoff.py), [calibration.py](../calibration.py), [calibrations.json](../calibrations.json), [make_audition.py](../make_audition.py), [tools/audition/heard.py](../../audition/heard.py) | track voice readiness, hand-authored signoff, listening variants, and untracked feedback |
| alternate objective | `shape/` | compare log-frequency spectrogram cells and inspect residual mechanisms without replacing the production fit objective |

The C++ synth and its tuning catalogue are the authority for patch fields, ranges, program-to-patch addresses, and shared engine constants. Python modules must read those reports instead of maintaining a second field list.

## Verification

Run focused checks after a relevant change and reserve native builds or renders for the commands that require them:

```sh
make spec-check
make voice-status-check
make voicematch-reextract-check
rye run --pyproject bindings/python/pyproject.toml python -m pytest tools/voicematch -q
```

`spec-check` catches deleted or renamed keys and clamp ranges that collapse to a point. `voice-status-check` checks the generated bank status. `voicematch-reextract-check` checks committed profile extraction against available scratch corpora. The Python suite covers capture loading, note mapping, room and rig decisions, objective coverage, fit quality, write-back, shape validation, identity guards, and recovery reports; it does not require licensed captured audio for ordinary unit cases.

Use `make voicematch-loss-sensitivity` to calibrate the meaning of term units against controlled audio perturbations and `make voicematch-loss-cells` to inspect comparison, cap, unavailable, and skipped cells in rendered output. These targets report measurement behavior and do not grant acceptance; [Acceptance](acceptance.md) owns gates and signoff.

For documentation-only changes, the scoped check is:

```sh
git diff --check -- tools/voicematch/README.md tools/voicematch/docs
```

## Knob connectivity

`check_specs.py` asks whether every spec key still exists. `liveness.py` asks the separate question of whether a sampled knob moves a valid measured result. A null can be caused by an inactive mechanism, an axis the probe does not exercise, a rejected or too-narrow range, missing reference cells, or an interaction outside the sampled conditions.

The catalogue's clamp probe covers every field exposed by the patch override layer, including integer and selector fields. A shared engine constant must be checked on a patch where its mechanism is live; `--program-only` prevents a single-voice fit from moving shared constants, but it does not make a shared constant program-local. Keep program fields and shared constants distinct in review and in bank status.

`--diagnose` is the detailed per-term connectivity report. It samples bounded individual and joint conditions and records `unreachable`, `measurement-limited`, `interaction`, `spent`, `partial`, `reachable`, `unscored`, `matched`, and `not computed` outcomes; see [Fitting](fitting.md#diagnosis) for how to act on them.

## Identity

`identity.py` checks raw render bytes between a base library and a head library with the new mechanism at its default, then requires a declared `--reach` override to change at least one selected render. `--isolate` checks that overrides addressed to one drum note do not alter neighboring notes and also rejects a vacuous string that changes nothing.

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/identity.py --base /tmp/base/lib/libsonare.dylib --head build-tuning/lib/libsonare.dylib --programs 0,19,40,56,73 --drums 35,36,38,47 --reach 36:d036.percussion.plate_gain=1.0
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/identity.py --head build-tuning/lib/libsonare.dylib --isolate d042.percussion.strike_r=0.4,d049.percussion.plate_gain=2.0
```

The reach half is required because a missing build flag, wrong loader path, unresolved key, or untaken branch can produce a perfect default identity table. Build the base and head libraries separately; loading both into one process is invalid because tuning overrides are read at library initialization.

## Dataset

`dataset.py` generates uniform knob-vector to full `probe_rows` pairs for an amortized inverse. It samples each knob in its declared scale, includes the compiled-in default as a check point, round-trips values through the formatter, and records failed or silent renders with reasons instead of dropping them. The output belongs under the scratch root and does not become a reference profile or a bank gate.

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/dataset.py --program 40 --pattern sustain --samples 1000 --build-dir build-tuning --out dataset.jsonl.gz
```

The dataset generator and recovery commands exercise measurement and search contracts with synthetic or model-generated audio. They do not decide whether a physical instrument is represented well; use the reference profile, independent validation, listening, and [Acceptance](acceptance.md) for that judgment.
