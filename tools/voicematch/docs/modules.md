# Modules — what each file is, and the two objectives they serve

## The comparison

- `voicematch.py` — CLI driver (`compare`, `export-probe`, `room-match`)
- `patterns.py` — note patterns + per-GM-program register table, the per-note drum probe spacing, and the drum phrase sequences
- `toneclass.py` — what kind of sound a GM program makes, and the three things that follow from it: which notes it is probed at, which metric set scores it, and which terms carry weight
- `render_model.py` / `render_oracle.py` — the model renderer, and the oracle (fluidsynth, an external WAV with score alignment, or a plugin)
- `au_oracle.py` — an AudioUnit instrument as the oracle, hosted by aubounce, with the guards a disk-streaming sampler needs and an on-disk render cache
- `metrics.py` — per-note analysis and deltas
- `room.py` — ambience: measure a reference's space, put the model in it, translate it back into libsonare's sends
- `rig.py` — whether a reference was recorded through an amplifier or a rotary: a cabinet's skirt, a rotor's anti-phase modulation, and the vacuous nulls of both. Shows a rig present and never absent, so it decides `baked` and leaves `none` to an A/B
- `smf.py` — minimal type-0 SMF writer (single source of truth for both sides)
- `wavio.py` — stdlib WAV I/O: 16-bit PCM out, any common format in
- `gm_names.py` — GM program labels and the percussion key map

## The reference

- `capture.py` / `profile.py` / `capture/` / `reference/` — capturing a reference corpus from a plugin and reducing it to committed measurements; the audio itself stays untracked. `profile.py render-grid` puts the model into the same corpus as one more timbre, so nothing downstream needs a special case for it
- `bank.py` — the library's own voice list as the audition index: every GM program, every variation bank, every kit, each resolved to a phrase set and to whichever capture covers it if any does. See [audition.md](audition.md)
- `phrases.py` — the phrase sets an audition page plays: three written for one instrument each, five generic ones filled in from the program's own compass, one per `ToneClass`
- `calibration.py` / `calibrations.json` — named calibration settings recorded per voice, so a batch across the bank carries per-voice candidates and a listening question outlives the shell history that asked it. Tracked, since an override string is knob names and numbers
- `make_audition.py` — the listening set for [`tools/audition`](../../audition/README.md)
- `render_corpus.py` / `compare_metrics.py` — a whole-bank phrase render and a diff of two of them; a broad before/after sweep rather than a per-note comparison
- `assets/`, `out/` — gitignored (soundfont download, render artifacts)

## The fitter

`autofit.py` plus the modules it drives, one per stage of a fit:

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
- `test_phrases.py` builds every set for all 128 programs, because a generic set fills its notes in from a register table and the way that fails is a note number MIDI has no room for — which renders as silence on one side and a transposition on the other, and reads as a voicing difference. It also holds the drum channel to the kit set alone.
- `test_bank.py` covers the index: that the whole bank is addressable with no two voices sharing a directory name, that program 0's two captures land on the piano and the kit respectively, and that a captured voice keeps its capture's phrase set — which is what keeps the reference archive reachable.
- `test_calibration.py` holds the shipped `calibrations.json` to voices the bank actually has, since a mistyped key renders the baseline alone and produces a page indistinguishable from a voice nobody has recorded a candidate for. It also covers the name rules, the file-then-flag order, and the refusal when one name is declared in both.
- `test_rig.py` covers the direction of each rig signature, and above all that a question which cannot be put reports as unanswerable rather than as a "no" — a vacuous negative is what would put `none` into a capture that never earned it.
- `test_shape.py`, `test_capture.py`, `test_dataset.py`, `test_room.py`, `test_smf.py`, `test_wavio.py` cover their own modules.
