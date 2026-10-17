# Fitting — `autofit.py`

`autofit.py` renders a model candidate, measures it against the selected oracle, and searches the declared calibration space. It is the path that can write values back to the synth source; numerical quality and bank acceptance are separate decisions described in [Acceptance](acceptance.md).

## Fit a voice

Run from the repository root through the rye environment; use `--help` for the complete current option set.

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/autofit.py \
    --spec auto --program 40 --pattern sustain --notes 48,60,72 \
    --optimizer cmaes --max-evals 200 --workers 8 \
    --validate-notes 55,67,79 --dry-run --out fit.json
```

`--spec auto` asks the tuning-enabled library for the program's own catalogue and clamp bounds. A JSON spec is required when the run needs a hand-selected subset or range. `--program` selects the GM program, `--bank` selects a GS variation patch, and `--drum-note` changes the probe to one percussion instrument on channel 10. A variation is its own patch, so its bank belongs in both the render and the catalogue query.

Use `--corpus <capture directory>` and `--corpus-timbre <id>` when a captured grid exists. The corpus supplies the notes, velocities, gate, tails, room, rig, note map, and oracle audio, so fitting and profile comparison share one stimulus. Without a corpus, the probe is generated from `--pattern`, `--notes`, and `--velocities`; the oracle is fluidsynth unless `--oracle-wav`, `--au`, or another explicit source is selected.

The fit must compare the model at the same instrument boundary as the oracle. `rig: baked`, an unclassified rig, and a `rig: none` that rests on no `rig_evidence` on a rig-capable family are refused, as is a `shape fit` or `shape prune` against the same references; a `shape` subcommand that does not fit prints that it is a diagnostic run. `--allow-rigged-oracle` carries a refused reference through as `unverified`: the fit runs and reports, and its values are never written into the source. See [Reference](reference.md#rig-and-room) for the direct, acceptance, and override routes. Room correction may be applied before scoring because a room is measured and convolved, while a rig is nonlinear.

## Knobs and ranges

The tuning catalogue is produced by the library under `SONARE_TUNING_DUMP`; it reports defaults, program-to-patch addresses, field types, and values surviving `clamp_synth_patch`. Use the source to understand a field and the catalogue to know the address the loaded library actually exposes.

| key shape | scope | fit behavior |
|---|---|---|
| `<file>.<constant>` | `SONARE_TUNABLE` engine calibration | runtime override; shared by every patch on that engine |
| `<patch>.<field>` | named program patch | runtime override for every program sharing that patch |
| `famN.<field>` | family fallback patch | runtime override; written back into the family table |
| `dNNN.<field>` | percussion note patch | runtime override for that drum note |
| `gm_fallback_map.<field>` | shared ambience or program-group setting | affects the address group named by the catalogue |

`--program-only` keeps only the selected program or drum note's own patch fields and excludes shared engine constants. Use it for a voice-by-voice write-back pass; adopt a shared knob only when references from more than one voice on that engine hold it. The catalogue's clamp probe must cover shared fields too, but a liveness result from one inactive patch is not evidence that the engine constant is unreachable.

Runtime knobs are set through `SONARE_TUNING_OVERRIDES`, so one tuning library serves the whole runtime-only search. Source knobs use a regex to replace one numeric literal, require exactly one match, and rebuild for every candidate; source-knob searches therefore run with one worker. Both forms may appear in one spec.

Ranges come from the clamp where the field has a meaningful bound. Small normalized intervals, and every field in decibels (`*_db`), are searched linearly over the whole bound; positive time-like ranges are searched around the default in log space; a zero-default field with no useful bound is omitted from `--spec auto` until a hand spec declares how to enable it. Fields left open by the clamp use the documented fallback range or require a hand spec. Integer, selector, count, and boolean fields are exposed through the same override layer and rounded to their declared type.

A best value on either range endpoint is a boundary result, including when the starting value is already there. Widen a hand range or inspect the model before calling it an optimum; a search cannot find a value outside the interval it was given.

## Search

Coordinate descent (`--optimizer coord`) performs a serial line search per knob and moves a knob only when the search strictly lowers the loss, so a knob the objective cannot see keeps its start value instead of landing on the line search's first probe. CMA-ES (`--optimizer cmaes`) samples correlated candidates and can use `--workers` for concurrent renders; `--restarts` spends the budget on new starts. `--max-evals` counts candidate evaluations, while a warm cache can reduce the number of fresh renders; `--evals-per-knob K` sets it to `K` times the knob count `--spec auto` resolved, held in [400, 2000]. The cache is keyed by library bytes, harness source, probe, and oracle; `--no-cache` disables it.

`--screen` probes each range endpoint and drops knobs whose weighted loss change is below `--screen-threshold`. It can miss an effect that exists only in the interior or behind a switch, so check the mechanism, range, and probe before treating a screened knob as inert. `--stages` fits excitation, decay, and then the full objective. `--grid` enumerates a small product of knob values and is useful for interactions; it evaluates the fit objective and is subject to the same reference-boundary rules, so it is not a rig-refusal exemption.

The objective resolves class defaults and explicit `--w-*` overrides, then records each term in its fixed engineering unit. The grid mean and upper tail are both retained for per-note terms, while dynamics, kit relations, and whole-render terms keep their own reducers. Missing reference cells are unavailable; a candidate cell missing where the reference is finite is charged rather than silently removed. See [Measurements](measurements.md) for term definitions, coverage, eligibility, and the percussion measurements.

For percussion, the normal terms include band profile, band decay, tilt, brightness, tonality, strike, ring, density, promptness, and the kit relations named by the capture. `evolve` and `diffuse` are consumed like other measured terms; their windows, shared hit-power reference, validity masks, worst-quarter aggregation, and controls are documented only in [Measurements](measurements.md#percussion-measurements). A pitched percussion reference may remain pitched. `--drum-note` narrows the grid to that note unless `--notes` supplies a family; an explicit kit weight requires a grid covering at least one declared family.

The shape objective is a separate spectrogram path. `shape fit` and `shape prune` accept `--report` for fit, selection, and final-validation records and `--out` for the override set; use it to identify cell-level residuals, not to silently replace the `autofit.py` objective used for write-back.

## Validation

`--validate-notes` and `--validate-velocities` score the selected result on conditions outside the fit grid. Each requested validation value must be disjoint from the corresponding `--notes` or `--velocities`; otherwise the report is training evidence labelled as validation. An external `--oracle-wav` needs `--validate-oracle-wav` for the held-out reference because one WAV cannot supply unseen conditions.

The report distinguishes `search_winner` from `selected`. Inspect `selected.loss`, `selected.quality`, absolute residuals, measurement coverage, target attainment, and the independent validation result before writing anything. A validation refusal may select the defaults even when the search winner improves the training probe. An unavailable validation set is reported as unavailable, not as a pass.

Level balance needs at least two distinct note or velocity conditions with finite reference levels. A single usable condition cannot establish a relative level response; the default weight is dropped and an explicit level weight is refused or reported unknown as appropriate. Held-out quality retains the same coverage requirement.

`--dry-run` restores the pristine source and skips write-back while still producing the report and diff. Use it with `--out` when reviewing selected values. Numerical target attainment is an engineering result; it does not grant listening acceptance or change bank status. Follow [Acceptance](acceptance.md#numerical-quality) for those decisions.

## Diagnosis

`--diagnose` reads the current source and reference instead of fitting or writing. It starts with individual knob endpoints and can add bounded interior, all-low, and all-high joint conditions when active residuals and evidence justify them; the sampled budget is finite and the report records it. A null can reflect an inactive mechanism, a missing or capped cell, a range, or an untested interaction.

Diagnosis separates connectivity from improvement: connectivity is the largest valid measured movement a knob or joint condition produces, while improvement is the largest reduction in the active term. Read both after the fit has reached the source state being investigated.

| verdict | interpretation |
|---|---|
| `unreachable` | no valid sampled individual or joint condition moved the term |
| `measurement-limited` | caps, missing cells, or incomplete probe evidence prevent a conclusion |
| `interaction` | a joint condition moved a term that individual samples did not |
| `spent` | sampled knobs move the term but none reduces it |
| `partial` / `reachable` | a sampled knob reduces part or most of the gap |
| `unscored` | the term has no weight in this run |
| `matched` | aggregate and reported worst note satisfy the declared numerical unit with known coverage |
| `not computed` | this probe or retained audio cannot compute the term |

`--diagnose` is exempt from the rigged-oracle fit refusal because it reports what the current model and reference contain. Existing unweighted terms can be reported as `unscored`; an unimplemented measurement axis is invisible to diagnosis. Check measurement coverage and weights before attributing a remaining deficiency to a model mechanism.

## Write-back

Runtime tunables replace their declaration literal, source knobs replace the captured source literal, named patch fields receive or rewrite an explicit assignment in the program table, and `dNNN.` fields land in the drum table. `famN.` values land in the family table: a field set from a `SONARE_TUNABLE` rewrites that constant's default, any other is written as a `t[N].` line. Switch fields are reported rather than written, because a search lands one in a bucket rather than on a value. Values that remain at their starting point are not rewritten.

The fit snapshots target source files, restores them on failure or interruption, and writes only selected values after validation. Selection can retain defaults for measurement dropout, held-out regression, or an objective that went blind. A normalized candidate worse than the start also retains defaults; this ratio guard does not apply under `--raw-loss`. `--out` records search and selected values, losses, validation, quality, source provenance, the objective fingerprint, every point the best-so-far moved through (`path`), and an override string for auditioning without a source edit.

`--spec auto` includes the selected patch fields and the engine constants it exposes. A source write to a shared engine constant can move every program using that engine, while a patch override reaches only the patch name in its key. When fitting a bank grid, use `--program-only` for per-voice fields and inspect the catalogue's patch prefix before deciding that a key was inert.

## Refitting the bank

A change to the objective leaves every earlier fit answering a question nobody asks any more. [`objective.py`](../objective.py) fingerprints the scoring code (the modules `loss`, `loss_aggregate` and `fit_quality` import, with comments, docstrings and whitespace removed), every `--out` report carries that fingerprint, and [`fits.json`](../fits.json) records per patch which fingerprint its last finished fit answered and whether the values were adopted or kept. `status.py` reads it into each voice's `fit` axis and names a refit as the next action before a listening round when the fit is stale or was never recorded. A behaviour-preserving refactor of the scoring code moves the fingerprint too; that is the conservative direction.

[`refit.py`](../refit.py) runs a bank pass. `plan` lists the capitals (`--banks variations` the variations, `--banks kits` every drum note a kit's machine captures hold) whose capture answers at the instrument boundary and has a gate; `run` fits each against its corpus with alternate notes held out (a drum note, held at one velocity by the module, is judged by the gate alone) and compares `profile.py compare --json` before and after the fit's overrides. A fit is adopted when values moved, the held-out notes improved, no gated statistic newly exceeds its bound and the summed excess over the bounds did not grow. A fit that improves the held-out notes while newly exceeding a bound, or moving a switch, is a trade: `resolve` bisects the points the search's best-so-far moved through (the report's `path`) for the furthest one the gate accepts, and failing that refits once with the terms carrying the broken bounds weighed up and searches its path the same way. The objective and the gate measure some dimensions through different windows, which is why weighing a term up does not reliably settle a trade and the path does. `adopt` writes the adopted values into the program, family and drum tables and records every finished fit.

The tuning layer applies a patch's overrides after the program tables have copied it, so an override on a capital never reaches `o.<copy> = o.<capital>;`. Every render repeats each patch-field override on each gate patch that still inherits that field. A field the copy, or a copy between them, assigns by name belongs to the copy. The baseline `pre` is the state before the first adoption that reached the voice, not the tables as they stand when it is measured. Each knob of an existing fit is pinned to its `start`. Each gate patch is also walked back through the git history of `tools/bank-versions.json` and `fits.json`: the trailing run of commits that moved its values while recording a unit of its struct-copy chain is the refit write-backs its shipped values descend from. Every field in that run owned by the voice's own chain goes back to its value before the oldest such commit. A copy's own adoption stays. `pre.json` lists each pin with its source, and an adoption `fits.json` records but `bank-versions.json` does not yet carry is listed as unresolved rather than guessed. `rejudge --only <slugs>` deletes a voice's `pre`, `post`, path renders and verdict, re-measures them over the fits on disk without running a fit, and leaves a trade that needs the reweighed refit open for `resolve`.

Run it in a worktree at a fixed commit: the evaluation cache and the build both follow the tree, so another session's edits would otherwise change what is measured mid-pass.

## Build and loader isolation

`autofit.py` configures an isolated `--build-dir` with `BUILD_SHARED=ON` and enables `BUILD_TUNING=ON` when runtime knobs need it. Each candidate render runs in a fresh subprocess with `SONARE_LIB_PATH` pointing at that build's dylib, so the override table is read before library initialization and a rebuild cannot reuse an already loaded artifact. `build-python-shared` is refused as the fit build directory because it is the normal shared-library tree.

Before a runtime search or diagnosis, the harness checks that a whole-spec override changes a render; a single genuinely inert knob remains a diagnosis result, while a flat whole-spec result indicates an unavailable tuning build, an ignored key, a rejected range, or a loader path that did not select the build. The fit records the build and source state in its report.

For recovery, native component observability, identity, liveness, and the exact verification commands, see [Development](development.md).
