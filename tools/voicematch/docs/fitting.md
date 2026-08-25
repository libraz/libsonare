# Fitting — `autofit.py`

Closes the tuning loop mechanically: set a voice's calibration constants, re-render the model, and minimise the model-vs-oracle mismatch. Pure Python + numpy; no scipy.

```sh
rye run --pyproject bindings/python/pyproject.toml \
    python tools/voicematch/autofit.py \
        --spec auto --program 40 --notes 55,67 \
        --optimizer cmaes --max-evals 400 --workers 6 \
        --screen --stages --w-env 1.0 --w-init 1.0 --w-slope 1.0 \
        --validate-notes 48,60
```

```
screening: 39/49 knobs move the loss by at least 0.002 over their range
== stage 'excitation': 6 knobs, 25 evaluations, weights {'init': 1.0, 'harm': 1.0} ==
== stage 'decay': 29 knobs, 125 evaluations, weights {'slope': 1.0, 'env': 1.0, 'harm': 0.5} ==
== stage 'all': 39 knobs, 153 evaluations, CLI weights ==

  initial 1.0000  ->  best 0.6996  over 400 evaluations
== held-out notes 48,60 ==
  start 1.0000  ->  best 0.7636   generalises
```

The objective is [loss.md](loss.md); the oracle is [oracles.md](oracles.md); the probe is [probes.md](probes.md). This page is everything else.

## Every instrument is already addressable

Ask the library what it has, rather than reading the source for knob names:

```sh
autofit.py --spec auto --program 40 --dump-knobs --program-only   # this program's voicing
autofit.py --spec auto --program 40 --dump-knobs                  # all ~11k knobs
```

A tuning build reports every key it consulted during a render, with its compiled-in default, so the list is produced by the code rather than by a parse that can go stale. Five kinds appear:

| key shape | what it is | scope |
|---|---|---|
| `violin.bowed_string.bow_force` | one **patch field** of the patch that voices a program | that program (and any that shares the patch) |
| `bowed_string_voice.kRosinDepth` | one **engine calibration constant** | every program on that engine |
| `fam0.piano.brightness` | a **family patch** field, for programs with no override of their own | the eight programs of that GM family |
| `d038.percussion.wire_buzz` | a **drum note's** patch field | that note |
| `gm_fallback_map.kSendsChurchOrganRev` | the program's default **ambience weighting** | that program group |

The patch prefix is the patch's own name, not the program number, because one patch often voices several programs — the dump's `# program NN is voiced by patch 'X'` header line resolves which is which.

`--spec auto` builds the knob list for a program from that catalogue: its patch fields plus its engine's constants. Good enough to start a fit on any of the 128 programs without writing a spec first.

`--bank N` selects a GS variation bank of `--program`. A variation is its own patch with its own knobs, so the flag selects both what is rendered and what `--spec auto` offers. Default 0, the capital tone.

## Where a knob's search range comes from

A patch field's range is **mostly not** a guess. `clamp_synth_patch()` bounds all but thirteen of them, and the dump reports those bounds (`--dump-knobs` prints them as the `min`/`max` columns), so `--spec auto` searches the interval the engine actually accepts:

| clamp bound | what `--spec auto` searches | why |
|---|---|---|
| `0 .. 1` (and any span ≤ 4) | the whole interval, linear | 0 and 1 are both meaningful settings; a window around the default would hide them |
| `1 .. 20000` ms | `default/8 .. default*8`, log, capped by the bound | four decades put the default in the first percent of a linear cube; the clamp still stops the window leaving the space |
| wide, default `0` | *not fitted* | the field is switched off and there is no magnitude to anchor a window on — picking one is the guess the bounds replace |
| none (an engine constant, or one of the thirteen fields the clamp leaves open) | `default/2 .. default*2`, or `0 .. 1` for a normalized-sounding name | the old heuristic, now the fallback rather than the rule |

The thirteen unbounded fields are the eight Karplus-Strong extensions (`ks.body_coupling`, `pluck_style`, `nail`, `pickup_pos`, `dispersion`, `tension_mod`, `octave_mix`, `keyoff_noise`), `bowed_string.stribeck` / `sympathetic` / `polarization`, `pipe_organ.keytrack` — each clamped by its own voice at `start()` rather than in the patch clamp, so the audio is safe and only the reported range is missing — and `percussion.strike_theta`, an angle that reaches a cosine and so only has to be finite.

Write a hand spec when a knob needs something else — a zero-default field you want switched on, or a range wider than eight times the default.

**A count is a knob too, and the ones that switch a mechanism on are the ones worth reaching first.** `unison`, `body`, `modal.num_modes`, `percussion.num_modes`, `percussion.shell_num_modes`, `percussion.noise_output` and `percussion.exclusive_class` are integers and enums rather than floats, and until they were in the override layer the questions they answer could not be asked without a rebuild. That is worse than it sounds, because a count is often the switch deciding whether the fields *around* it do anything: sweeping `shell_freq_hz` and `shell_mix` with `shell_num_modes` at 0 gives a clean structural negative — "the shell cannot reach this measurement" — when the finding is only that the shell was switched off. The override arrives as a float like every other and is rounded rather than truncated, so a search stepping across 2.5 lands on 3. Bounds come from `clamp_synth_patch` by probe exactly as a float's do; a field the clamp does not narrow states the range its own type defines, because a probe wide enough for an `int` would measure the wraparound instead.

### A range that does not contain the answer is the most expensive failure this tool has

Nothing about the result looks wrong: the search reports the best point it was allowed to visit, so the value pins to the bound, the loss goes down, the diff is small and the report is clean. `piano_voice.kTrebleDecayOct` was searched over `[0.5, 3.0]` while the value it wanted was 5.0, and 3.0 is what such a run reports.

So every run ends by naming any knob sitting on an end of its range, **including one that started there** — a spec whose range no longer covers a constant's default has that default clamped in on load, and the knob then reads as unchanged for the rest of the run because by a start-to-best measure nothing happened. A pinned knob is not automatically wrong; a clamp bound is a real end of the space. What is never safe is reading it as an interior optimum.

## Knobs: runtime or source

A **runtime knob** names any catalogue key — a `SONARE_TUNABLE` constant (`src/util/tunable.h`) or a per-program patch field:

```json
[{ "tunable": "piano_voice.kHammerWidthHarmonics", "min": 2.5, "max": 9.0, "scale": "log" },
 { "tunable": "trumpet.brass.brassiness", "min": 0.0, "max": 1.0, "scale": "linear" }]
```

The default is read from the catalogue (or from the source, for a `SONARE_TUNABLE`), and the fit sets it through the `SONARE_TUNING_OVERRIDES` environment variable — so **the library is built once for the whole run** and an evaluation costs a render instead of a rebuild. That is the difference between a fit that affords tens of evaluations and one that affords hundreds, which in turn is the difference between fitting one knob and fitting fifty. Use this form.

A bare constant name (`kHammerWidthHarmonics`) is accepted when it is unambiguous; the scoped form is required when several voices declare it, which they often do — `kBreathBase` exists in four.

A **source knob** points a regex at a numeric literal, for a value that cannot be a named constant at all (an array element, say):

```json
[{ "file": "src/midi/synth/piano_resonance.cpp",
   "pattern": "kDiffuserMs\\[2\\] = \\{([0-9.]+)f", "min": 2.0, "max": 8.0 }]
```

- `pattern` needs **exactly one capturing group** selecting the literal (the `f` suffix stays outside it), and must match the file exactly once — zero or multiple matches abort before anything is written.
- `scale` is `linear` or `log` (log optimises in log-space; needs `min > 0`).
- **Any source knob in the spec puts every evaluation back behind a rebuild**, and forces `--workers` back to 1, since a rebuild rewrites the tree every render reads.

Both kinds can be mixed in one spec. `specs/example.json` shows all three forms.

## Making a constant tunable

Replace the declaration:

```cpp
constexpr float kStrikeNoiseInject = 0.298027f;   // before
SONARE_TUNABLE(kStrikeNoiseInject, 0.298027f);    // after (+ #include "util/tunable.h")
```

In a normal build the macro expands to exactly the `constexpr float` it replaced — same storage, same codegen, no lookup. Only `-DBUILD_TUNING=ON` turns it into a runtime-overridable value, and `autofit.py` sets that flag itself when the spec needs it. A value written mid-expression (`ham_mu_ = 0.229431f;`) has to be lifted to a named constant first; that is a readability improvement anyway. Prefer file scope over function scope: a function-scope declaration is re-resolved on every call, which in a render loop means a table lookup per sample.

Two rules:

- **Never branch on `SONARE_TUNING`.** The two configurations must render identical audio for identical values, or a fitted value transfers nothing back to the shipped build. (Compiling a tuning-only override layer out of a normal build is not a branch on behaviour: with no override set the layer is the identity.)
- **Keys are scoped by file, values are not namespaced within one.** The scope comes from `__FILE__`, so `brass_voice.kBreathBase` and `flute_voice.kBreathBase` are separate knobs, but two declarations of one name in the same file would collide — `autofit.py` refuses to run when it finds that.

The knob machinery itself lives in the library, all of it behind the `BUILD_TUNING` CMake option: `src/util/tunable.h` (the `SONARE_TUNABLE` macro, the override table, and the `SONARE_TUNING_DUMP` catalogue) and `src/midi/synth/patch_tuning.h` (the per-program patch field layer).

The clamp bounds in the catalogue are measured rather than mirrored: `patch_tuning.cpp` fills every field with a value far outside any interval, runs `clamp_synth_patch`, and reads back what survived. It walks the same field list the override layer already has, so a field added there is bounded for free and no table can drift out of step with `clamp_synth_patch` itself. A field the clamp leaves open comes back at the probe value and is reported as unbounded rather than as a range of ±1e30. An integer field is probed the same way with the magnitude an `int` can hold rather than 1e30; a field whose own type is its range, like the mute group or the noise filter's output tap, states that range instead, because a probe wide enough to overflow it would measure the wraparound.

## Optimiser

- `--optimizer coord` (default) — coordinate descent, golden-section per knob (`--per-knob-evals`, default 6). Readable, and fine when a knob has an obvious optimum, but it stalls on knobs that trade against each other: a level and the taper that undoes it send it back and forth without either step being wrong on its own. Inherently serial — each probe is chosen from the previous one's result — so `--workers` does not help it.
- `--optimizer cmaes` — covariance-matrix adaptation. It learns that correlation and steps along it. Tune with `--population`, `--sigma0`, `--seed`; samples are clipped into each knob's range rather than penalised, so an optimum pinned to a bound is reported as such (widen the range, or accept that the model cannot go further in that direction). `--restarts N` restarts from a fresh random point with a doubled population when a run stalls, sharing `--max-evals` rather than multiplying it.

Use `cmaes` once the spec is runtime knobs only — that is the case where the evaluation budget is large enough for it to pay off.

**`--workers N`** renders a CMA-ES generation's whole population concurrently. The candidates are independent subprocesses, so the only thing serialising them was the loop that launched them; scoring stays on the main thread in submission order, so the trajectory and the log are byte-identical to a serial run at the same `--seed`. Measured on this machine, a 120-evaluation organ fit: **71 s at `--workers 1`, 30 s at `--workers 8`**, with identical losses. The speedup is well under 8× because a fixed ~12 s of build, catalogue dump and oracle resolution is not parallelised and the render itself is not single-threaded.

## Cutting the problem down

49 knobs for a violin, 21–114 across the 128 programs. CMA-ES learns a covariance whose cost grows with the square of the dimension, so a budget that would comfortably fit ten knobs does nothing at fifty. Three levers, all optional:

- **`--screen`** probes each knob at both ends of its range with everything else at its default, and fits only the ones that move the loss by at least `--screen-threshold` (default 0.002 — 0.2 % of the start). The dropped knobs are always listed with their measured effect; a silently narrowed search reads afterwards as a search that covered everything. It costs `2n+1` evaluations, so it pays for itself only when the budget is several times the knob count — the tool says so when it is not.

  **Below that budget it does not merely waste evaluations, it changes the verdict.** Two program-0 runs differing only in this flag, both 114 knobs over a seven-note probe at 600 evaluations and the same seed: without it the fit reached 0.8050 on the probe and **0.8541 on held-out notes, generalising**; with it, 0.8767 on the probe and **1.0535 held out — worse than the defaults on notes it never saw**. Screening spent 229 of the 600 evaluations and dropped 26 of the 114 knobs. One pair of runs is not a law, but read `--validate-notes` before trusting any screened result, and prefer raising `--max-evals` to narrowing the knob set.
- **`--stages`** fits the excitation knobs against the onset evidence (`--w-init`), then the decay knobs against the decay evidence (`--w-slope` / `--w-env`), then everything under the weights given on the command line. A brighter excitation with a faster decay and a duller one with a slower decay produce nearly the same average spectrum, so asking one search to set both at once sends it wandering along that ridge; `skeleton_note` already separates the evidence, and this is what uses the separation. Classification is by field name, and the final stage takes every knob, so a misclassification costs efficiency and never reach.
- **`--validate-notes 48,60`** scores the result on notes the fit never saw and reports both. It is the only thing in the run that can say whether the values generalise — the fit's own objective is the number it minimised. Three probe notes are enough to pin a physical voice into a configuration that is right at those three and wrong a fifth above. `--validate-velocities` is the drum equivalent, since a drum note has no register to hold notes out of. With `--oracle-wav`, `--validate-oracle-wav` is required: the held-out probe is a different score and needs its own reference, rendered the same way.

## Looking at the surface before searching it (`--grid`)

```sh
autofit.py --spec hat.json --program 0 --drum-note 42 --grid 7 --validate-velocities 48,112
```

A hold-out scored on the winner alone says whether that point generalises and nothing about whether it is a **peak or a plateau**. On a closed hi-hat, the best point of a coarse grid read −10.3 % on the fit and −0.8 % held out — a feature of the fit set — while re-cutting the same interval finer found a −22 / −19 region sitting between that grid's teeth. Reading one number would have taken the first.

`--grid POINTS` enumerates the product of every knob in the spec (at most three) and prints the fit and the hold-out loss at each point, sorted by fit. It replaces the fit rather than following it and writes nothing back. Run it before trusting a search over knobs that interact; a broad region good on both columns is worth more than a better isolated point.

## What calibration cannot reach (`--diagnose`)

A fit reports one number and that number cannot answer the question it raises. A loss of 0.62 says the values improved; it does not say whether the remaining 0.62 is constants still slightly off or a mechanism the voice does not have — and those call for opposite work.

```sh
autofit.py --spec auto --program 6 --diagnose --workers 8 --out diag.json
```

It runs the same `2n+1` probe `--screen` does and keeps the **per-term** mismatch instead of collapsing each render to one number. From that it reads two independent things per measurement, and the difference between them is the whole point:

- **Connectivity** — the largest change any single knob makes to the term, *in either direction*. Near zero means nothing this program exposes is wired to that measurement, and no budget reaches it.
- **Improvement** — the largest reduction any single knob makes. A term that moves but does not improve is a term the fit has already spent, or one whose improvement costs another term.

Reading improvement alone is the way this measurement lies. Once a fit has written back, every knob sits at its own optimum and nothing improves anything, so a perfectly well-modelled voice reports every measurement as structurally missing.

| verdict | what it means | what to do |
|---|---|---|
| `unreachable` | no knob moves it at all | the model is missing a mechanism — the finding this exists for |
| `spent` | knobs move it, none reduces it | a trade-off to price, or a converged fit |
| `partial` / `reachable` | one knob buys a tenth / half of the gap alone | keep fitting; the named knob is where to start |
| `unscored` | the term carries no weight | no fit has ever tried; weight it before calling it anything |
| `matched` | inside the smallest difference the term resolves | nothing |
| `not computed` | `mss` without `--w-mss` | it is an absence, not a match |

Three things it reports about itself, because each one turns a null result into a wrong conclusion:

- **A knob is only as live as the probe's axes.** The `sustain` pattern holds velocity fixed, so every dynamics control reads dead against it — and on a harpsichord, whose identity is what happens across velocity, that is the axis worth probing. The report names what the probe varied next to the knobs it called inert.
- **A null is only as strong as the range it was searched over.** No effect across the whole interval `clamp_synth_patch` accepts is strong evidence; no effect across a heuristic window around the default is almost none. The two are reported separately rather than averaged.
- **A one-at-a-time probe cannot see a knob that is inert alone and effective in combination.** `unreachable` is a hypothesis to test by adding the mechanism and watching the term move — and if it does not move, the mechanism was not the missing one either.

Probes whose render had nothing to measure are excluded and counted, since one end of any gain is silence and a silent render matches nothing.

## Proving the probe reached the code

Two guards, both unconditional, because a probe that never reached what it was aimed at produces a clean run with a plausible answer and nothing that reads as a failure.

- **The override table.** A model render puts `SONARE_TUNING_OVERRIDES` in the child's environment and used to take it on faith from there. A library built without `BUILD_TUNING` ignores the variable entirely, and so does a run that loaded a different dylib than the one just built: every candidate then renders the compiled-in defaults, every evaluation returns the same loss, and the fit reports its start point as the winner of a search it never ran. Before anything is searched — and before `--diagnose`, where an unreached override turns every knob into a structural finding about the voice — the whole spec is pushed to the far end of every range at once and the render has to move. One knob can be genuinely inert, which is a finding about that knob; the entire spec moving nothing is a finding about the plumbing.
- **`--screen` finding nothing.** Zero knobs moving the loss used to fall through to "keep them all", so a spec that moved nothing and a spec that moved everything continued into the fit with the same knob count and the same one-line message. Zero is now an error naming the three things that produce it: an engine switched off underneath the fields being swept, a range the clamp rejects, and a probe whose pattern does not exercise the axis the fields act on.

**Build isolation** — a dedicated build dir (`--build-dir`, default `build-autofit`) is configured with `-DBUILD_SHARED=ON`, plus `-DBUILD_TUNING=ON` when the spec has runtime knobs (a cache left at the other setting is reconfigured, since a library that ignores every override would fit a perfectly flat loss for no visible reason). Each model render runs in a fresh subprocess with `SONARE_LIB_PATH` pointed at that dir's dylib, so a rebuilding run never reads a dylib already mapped into the process, and a runtime-knob run gets its overrides into the environment before the library's static initialisers read them. `build-python-shared` is refused as a build dir.

**Safety** — the pristine text of every target file is snapshotted at startup and restored in a `finally` block, so an exception or Ctrl-C never leaves the tree perturbed. On a normal run the best values are then written back and a unified diff plus the loss trajectory are printed; `--dry-run` restores pristine, skips the write, and reports the diff it would have applied.

## Write-back

Everything a fit moves is written back to the source, in the form that value takes there:

| knob | where it lands |
|---|---|
| `SONARE_TUNABLE` | its declaration's literal, so it becomes the new compiled-in default |
| source knob | the literal the regex captured |
| patch field of a named patch | a new `o.violin.bowed_string.bow_force = 0.646063f;` line in the program table |
| `d038.` patch field | a new `t[38].percussion.wire_buzz = 0.646063f;` line in the drum table |
| `fam3.` patch field | *reported, not written* — the family patches are built by a loop with no per-patch site |

The patch-field case needs the extra line because the table builds most patches through helper lambdas taking positional arguments (`o.violin = bowed(0.12f, 0.55f, …)`), so no literal in it belongs to a named field. An explicit assignment after the call is the idiom that table already uses for its own exceptions, and a field that already has one is rewritten rather than duplicated.

A knob the fit left where it started is not rewritten at all: the two spellings of a value are not always the same text (`0.10f` against a formatted `0.1`), and a fifty-knob spec would otherwise bury the handful of real changes in a diff of lines that change nothing.

`--out result.json` records the whole thing — every knob's start and best, the losses, the held-out score, and a paste-ready `SONARE_TUNING_OVERRIDES` string for auditioning the result without rebuilding.

## Fitting a drum note

`--spec auto --drum-note N` offers that note's own patch fields (`d038.percussion.wire_buzz`, `d038.amp_env.decay_ms`, …) with the same clamp-derived ranges as a program patch — a bound belongs to the field, so `percussion.wire_buzz 0..4` covers every drum note that has one. `--stages` splits them the same way, with the noise burst, the strike position and the pitch drop as excitation and the mode decay, the wire buzz and the shimmer as decay.

**`--drum-note N` also narrows the grid to that one note** unless `--notes` says otherwise, which is what the `kit` term needs to be told. Scoring the tom series means `--notes 41,43,45,47,48,50` alongside the `--drum-note` whose knobs are being moved; without it the run has no family in the grid, the class default drops `kit` rather than scoring it 0.0, and an explicit `--w-kit` is refused with the reason.

What is **not** covered: a drum fit moves one note's patch. Mute groups (the hi-hats share an exclusive class), the kit assignment, and anything structural stay where the table put them.
