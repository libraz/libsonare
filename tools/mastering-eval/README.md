# mastering-eval

One harness for the mastering chain (`src/mastering/`) and the restoration modules (`src/mastering/repair/`): one corpus, one runner, one ledger. [`docs/objective.md`](docs/objective.md) is the contract — what the measurements are for, which of them may decide anything, and what the pass criterion is. Where this page disagrees with it, it is right and this one is wrong.

## Running it

```sh
cd bindings/python
rye run python ../../tools/mastering-eval/corpus.py                    # write the corpus
rye run python ../../tools/mastering-eval/run.py                       # measure both families
rye run python ../../tools/mastering-eval/run.py --family restoration  # narrow to one
```

Both run from `bindings/python` so `rye` resolves numpy and `libsonare`. `run.py` imports the built Python binding, so a C-ABI change needs its dylib rebuilt first — point `SONARE_LIB_PATH` at the build you mean to measure, because the loader otherwise prefers `build/lib/libsonare.dylib` and will happily measure a stale one.

**What runs by default.** `run.py` with no arguments measures **both families** — 95 rows, 51 restoration and 44 chain. `corpus.py` with no arguments writes the **whole** corpus; there is no default subset of it. `--family` narrows a run; it is not there to keep the default cheap, because a default that leaves the chain unmeasured costs more than the seconds it saves.

Measured on this tree: `corpus.py` takes about 1.5 s and writes 125.5 s of audio across 22 items (53 WAV files — the noise items carry five draws each, plus one clean reference per item). `run.py` takes 11.5 s for both families, 6.5 s for restoration alone.

`corpus.py` also measures each item's short-term loudness spread through `metrics_chain`, which reaches the C library. That is the one thing in corpus generation that needs a built dylib, and it is not required: without one the manifest records the spread as null and says so, rather than claiming a value.

| Flag | |
|---|---|
| `corpus.py --output-dir` | where the audio and manifest go (default `tools/mastering-eval/audio/`) |
| `corpus.py --seed` | the base seed every item derives its generator from |
| `corpus.py --skip-listening` | leave out the four 4-second fixtures |
| `run.py --family` | `both` (default), `restoration` or `mastering` |
| `run.py --item` | restrict to these item ids (repeatable) |
| `run.py --preset` | the mastering preset the chain runs (default `pop`) |
| `run.py --out` | where the run ledger goes (default `runs/latest.json`) |
| `run.py --emit-baseline` | also write the run's numbers in the ledger's shape |

## What is in the corpus

Five kinds of item, and they differ in what may be concluded from them.

**`synthetic`** — sine and chord beds crossed with clicks, hum, clipping and noise. Every item ships a clean reference beside it and every planted defect's quantity in the manifest. These are the only items a restoration metric can be read on, because all four restoration metrics compare a clean signal against a processed one.

| id | bed | planted |
|---|---|---|
| `sine_click` | sine | 12 clicks |
| `chord_click` | chord | 40 clicks, wider |
| `sine_hum50` | sine | 50 Hz, 4 harmonics |
| `chord_hum60` | chord | 60 Hz, 3 harmonics |
| `sine_clip` | sine | ~6.5 % of samples, runs of ~4 |
| `chord_clip` | chord | ~8.5 % of samples, runs of ~47 |
| `sine_white_noise` | sine | white, 12 dB SNR, **5 draws** |
| `chord_pink_noise` | chord | pink, 12 dB SNR, **5 draws** |
| `chord_noise_hum_click` | chord | all three at once |

They are short on purpose — the restoration metrics are frame-averaged and read fine off half a second. They are also tonal, which is why STOI is not read on them (below).

**`speech`** — the same five defects again, on speech-bearing material: pitch pulses through three formant resonators under a 2.3 Hz syllable gate. **These are the only items STOI is read on.** They run 2.5 s rather than the tonal items' half second because STOI's silent-frame removal drops roughly a quarter of the signal and the measure returns NaN under about a second; a shorter speech item would be speech-bearing and still unreadable.

| id | planted |
|---|---|
| `speech_click` | 14 clicks |
| `speech_clip` | threshold 0.45, 1.36 % of samples, runs up to 22 |
| `speech_hum50` | 50 Hz, 4 harmonics |
| `speech_white_noise` | white, 0 dB SNR, **5 draws** |
| `speech_pink_noise` | pink, 0 dB SNR, **5 draws** |
| `speech_dynamic_hum50` | 16 s, section-varied level, 50 Hz + 4 harmonics |

`speech_dynamic_hum50` is the one item both families read at once, and it exists because nothing else in the corpus lets STOI and short-term loudness spread be crossed. STOI needs speech, a clean reference and a planted defect; spread needs length and slow level variation. The speech items are 2.5 s and the long `dynamics` items carry neither speech nor a defect, so before this item that pair of metrics could not be measured on any single material at all. Its defect is hum because hum is deterministic — a noise defect would pull a 16-second item into the draw ensemble — and because STOI moves clearly on it. Measured: spread 4.373 LU at the chain's input and 3.586 LU at its output, STOI 0.917 → 0.993 through the dehum.

Its sections step further than the chord item's and its hum sits lower, because both effects narrow the spread on speech. The 2.3 Hz syllable gate is averaged away by the 3 s window, and hum at a fixed level fills the quiet sections' floor — the first attempt, with the chord item's own levels, read 2.94 LU.

The noise items sit at 0 dB rather than the tonal items' 12 dB. STOI on this bed reads 0.98 at 12 dB, which leaves a denoiser nothing to win and only damage to lose; measured across the bed the denoiser's STOI delta is negative at 18 and 12 dB, positive at 6 and 0, and negative again at −6. A row placed where the metric cannot improve tests nothing in the improving direction.

The generator is the one the STOI implementation was checked against the reference implementation with, copied rather than rewritten — a speech-like signal written twice is two signals, and the agreement figures would then have been measured on material this corpus does not contain. It takes no seed: the bed is a closed function of length and rate, and the only randomness in a speech item is the noise a defect plants.

**This bed is not a substitute for recorded speech, and STOI reads optimistically on it.** It holds one vowel and one 2.3 Hz modulation line, so every band's envelope has the same shape and their correlations sit more stable than real speech would. Band-selective damage is still detected; it simply reads smaller than it would on a real voice. The fix, if STOI turns out to be too blunt here, is to vary the generator's envelope across bands — several vowels, a different modulation rate per resonator — not to change the metric. This is why the contract keeps recorded material as a separate half of the corpus.

One more property worth knowing before reading a number off these rows: the channels carry the same signal at a −0.5 dB offset, and `plant_noise` draws each channel's noise independently, so the downmix feed sits about 3 dB better in SNR than either channel feed. **The three feeds of one noise item are not three measurements of the same condition** and must not be pooled as repeats. The repeats are the draws, below.

**`dynamics`** — long beds with slow level variation, carrying no defect and no clean reference. They exist for one reason: short-term loudness is a 3 s window that emits no partial, so a feed under 3.1 s produces **no** spread rather than a small one, and a steady feed produces blocks that are all the same. Neither leaves the dynamics metric anywhere to move, and a check for over-compression needs somewhere for it to move to.

| id | length | shape | measured spread |
|---|---|---|---|
| `dynamic_sections` | 16 s | four 4-second sections at -6, -18, -10, -22 dB | 4.62 LU |
| `slow_swell` | 16 s | one ±9 dB swell, 8-second period | 5.27 LU |
| `program_build` | 12 s | kick/hat transients over bass and pad, slow build | 5.10 LU |

The spread column is measured, not intended: `corpus.py` calls `metrics_chain.short_term_spread` on each item's own audio and writes the result into the manifest. For reference, the chain at the `pop` preset takes `dynamic_sections` from 4.62 LU down to 3.24 LU.

**`listening`** — the four 4-second fixtures `tools/mastering_generate_listening_corpus.py` produces, imported rather than re-derived so there is one generator for them. They carry no clean reference and no recorded defect quantity, so they serve chain measurement and listening only. `run.py` records them in `skipped` for the restoration family rather than measuring them. Their measured spread runs from 0.00 to 0.35 LU — they reach the window but are close enough to steady that the dynamics metric has no range on them either, which is what the `dynamics` items are for.

**`recorded`** — real captures, for what synthesis does not reach. Empty in a clean checkout.

## Where the audio lives, and what is committed

Everything under `tools/mastering-eval/audio/` and `tools/mastering-eval/runs/` is regenerable or per-run output and **is not committed**: the generator, the runner, the metric modules and the committed baseline are what land in the tree. A single run's ledger is not a baseline and does not get committed. Captures follow the same convention the instrument-bank harness uses — they stay on the machine that made them.

`.gitignore` carries both directories, so a `git add` of `tools/mastering-eval/` stages the sources and nothing else — not the audio, not a ledger, not `__pycache__`.

Recorded captures go in `audio/recorded/` with a `manifest.json` beside them:

```json
{
  "items": [
    {
      "id": "vinyl_crackle_01",
      "audio": "vinyl_crackle_01.wav",
      "clean_reference": null,
      "defects": null,
      "source": "turntable, direct out",
      "notes": "no clean reference exists for this one"
    }
  ]
}
```

`corpus.py` folds them into the corpus manifest and fills in the rate, channel count and length from the files themselves. An entry with a `clean_reference` becomes usable for restoration; one without is a chain item only.

## The manifest

`audio/manifest.json`, schema 1. Per item:

| field | |
|---|---|
| `id` | the item's name, and the key a ledger row is built on |
| `role` | `synthetic`, `listening` or `recorded` |
| `bed` | the material the defects were planted on |
| `audio`, `clean_reference` | paths relative to the manifest; the reference is null when there is none |
| `sample_rate`, `channels`, `frames`, `duration_seconds`, `bit_depth` | |
| `seed` | the generator's derived seed, or null |
| `defects` | the planted quantities, or null when none were recorded |
| `realizations` | on a noise item, one entry per draw with its own seed, audio and measured quantities; absent on a single-draw item |
| `usable_for` | which families may read this item |
| `speech_bearing` | whether STOI may be read on this item |
| `chain_readiness` | whether the chain's dynamics metric can say anything about this item |

`chain_readiness` carries `duration_seconds`, `reaches_short_term_window` (is it over 3 s), `short_term_spread_lu` (measured, or null when it could not be measured) and `measured_with` (the function that produced it, or null). It is there so a later comparison can reject a too-short row by reading the manifest, instead of meeting the shortfall as a NaN in a ledger and having to guess what it meant. A NaN spread is not a row that scored badly; it is a row the metric could not be computed on.

The item's length is recorded once, as `duration_seconds`, and `chain_readiness.duration_seconds` is the same number repeated where a reader of that block needs it.

Each defect records what was planted:

- **`click`** — `count`, `positions_samples`, `amplitudes` (signed), `width_samples`, `channels`
- **`hum`** — `fundamental_hz`, `harmonic_count`, `harmonic_hz`, `harmonic_db`, `channels`
- **`clip`** — `threshold`, `threshold_in_file`, `clipped_samples`, `clipped_samples_per_channel`, `clipped_ratio`, `max_run_samples`
- **`noise`** — `kind`, `requested_snr_db`, `achieved_snr_db`, `noise_floor_dbfs`, `channels`

`threshold_in_file` is not a duplicate of `threshold`. The WAV writer scales by `2**23 - 1` and the reader divides by `2**23`, so a plateau written at the requested threshold reads back one step under it, and the declipper's detector is an inclusive `>=` against a level. The requested number finds nothing; the file's own plateau is what a caller passes.

The generator refuses to write an item whose planted signal exceeds full scale, because the writer's clamp would otherwise plant a second defect at an amplitude nothing records.

## Mono and stereo

The restoration entry points are mono and the corpus is stereo. Every restoration job therefore runs three times — on the **downmix**, on **left**, and on **right** — and the feed is part of the row's identity, alongside the entry point it called. A stereo restoration entry point, when one exists, adds rows rather than changing what an existing row means, so the two can be compared.

The downmix is `0.5 * (L + R)`, recorded per row as `mid_mean`. It is recorded because changing it invalidates every comparison across the change, and a comparison that silently spans two downmix rules is worse than one that fails.

The mastering chain has both entries, so it is measured on both: `master_audio_stereo` (feed `stereo`) and `master_audio` over the downmix (feed `downmix`). A mono item yields one feed named `mono` rather than three copies of the same numbers.

## Parameters handed to the processors

Dehum needs the fundamental, declip needs the plateau, and declick's default level gate sits above every click in this corpus. Handing them the planted quantity makes a row a measurement of **restoration quality given correct detection**, which is not a measurement of detection. `planted_params_supplied` names, per row, which quantities were supplied, and `params` records the values.

## The metric seam

`run.py` computes nothing. Every number comes from `metrics_repair` or `metrics_chain`, resolved by name at startup; a missing function names itself and the alternatives that were tried.

**Restoration** — `metrics_repair`, called as `f(a, b, sample_rate)` on 1-D arrays:

| ledger key | function | measured between |
|---|---|---|
| `seg_snr_db` | `segmental_snr` | clean, processed |
| `log_kurtosis_ratio` | `log_kurtosis_ratio` | **input**, processed |
| `stoi` | `short_time_objective_intelligibility` | clean, processed |
| `log_spectral_distance` | `log_spectral_distance` | clean, processed |

Log kurtosis ratio is the one measured against the unprocessed input rather than the clean reference: it asks what the processing created, not what survived.

**Mastering** — `metrics_chain`. Three are single-signal, called as `f(x, sample_rate)` and reported as an input/output pair; the fourth is inherently a pair, called as `f(input, output, sample_rate)`:

| ledger key | function | |
|---|---|---|
| `integrated_lufs` | `integrated_loudness` | single |
| `true_peak_dbtp` | `true_peak_dbtp` | single |
| `short_term_spread` | `short_term_spread` | single |
| `band_energy_delta` | `band_energy_delta` | pair |

Chain metrics receive the feed's own shape: `(frames,)` for a mono feed and `(frames, channels)` for the stereo one. Integrated loudness and true peak are channel-dependent by definition, so collapsing the stereo feed before measuring it would measure something else.

### `metrics` holds numbers that were computed, and nothing else

A reader iterating a row's `metrics` never meets a null. A metric missing from it is explained in exactly one of two places, and the two are different answers:

| where | means |
|---|---|
| the row's `inapplicable_metrics` | this material is outside the metric's domain |
| the ledger's `unavailable_metrics` | the implementation does not exist yet |

A single null in a shared field would collapse those into one state, and a pass criterion has to tell them apart: the first is a row that is complete without the metric, the second is a run that is not complete at all.

### Noise rows carry five draws; everything else carries one

Noise is the one planted defect whose metric movement is smaller than the spread between draws of the same condition. A denoiser's STOI delta at 0 dB averages about +0.002 with a standard deviation of about 0.003, so a single draw is enough to show that something got *worse* — a regression moves the sign on the identical waveform the baseline used — but not enough to claim something got *better*, which generalizes from one draw to the condition.

The four noise items therefore carry `NOISE_REALIZATIONS` (five) independent draws each: same bed, same kind, same requested SNR, different noise. Click, clip and hum stay single, because they are deterministic or move far above that spread, and multiplying their rows would buy nothing. `chord_noise_hum_click` also stays single: its noise is one of three defects on one waveform, so ensembling it would multiply its click and hum rows too, and its job is the chained case rather than a noise measurement.

**Five was fixed before any of this was measured.** Choosing the count by seeing which count produces the sign one wants is the same defect as choosing the seed that way.

An ensemble row carries:

| field | |
|---|---|
| `realizations` | how many draws are behind the row — `1` or `5`, on every row of both families |
| `metrics` | the mean across draws, and what the baseline records |
| `metrics_sd` | the population spread across draws |
| `delta`, `delta_sd` | processed minus untreated, taken **pairwise** per draw |
| `metrics_per_realization`, `untreated_per_realization` | the draws themselves |
| `saturation_basis` | `worst of the draws` — see below |

`delta_sd` is not derivable from `metrics_sd` and `untreated_sd`. A draw's processed and untreated numbers come from the same waveform and move together, so combining the two marginal spreads would overstate the spread of their difference. The delta is what an improvement claim rests on, so it is the one taken pairwise.

Saturation on an ensemble row is the worst draw's, not the mean. Averaging a ceiling fraction would report a row as half-crushed when one draw was fully crushed, which is the reading the fraction exists to prevent.

The chain family reads the first draw only and its rows say `realizations: 1`. Its metrics are level and spectrum statistics, not the small correlations a noise draw moves.

### Which numbers are a property of the algorithm, and which are one draw

**A `realizations: 1` row's absolute value is that one draw.** It is a sound basis for detecting a regression — the baseline and a later run read the identical waveform, so the draw cancels between them and any movement is the processor's. It is **not** a basis for a statement about the processor itself. "This denoiser costs 0.009 of STOI" cannot be read off a single-draw row; that row's −0.009 is one realization of a quantity whose spread across draws is of the same size.

On an ensemble row, `delta` with `delta_sd` and `realizations` beside it is what supports such a statement, and the arithmetic is the reader's: the standard error is `delta_sd / sqrt(realizations)`. Measured on this corpus at five draws, the pink-noise rows show the denoiser improving STOI by about two to three standard errors, and the white-noise rows show nothing distinguishable from zero — because the effect there genuinely is near zero, not because five draws were too few to find it.

The same spread was measured independently from the metric's side, with no processor in the path at all: STOI on this bed at a fixed SNR moves with a standard deviation of about 0.0046 across eight unpaired noise draws (0.0042 at +6 dB, 0.0046 at 0, 0.0049 at −6). Pairing a draw's processed and untreated numbers roughly halves that, which is where the 0.0026 above comes from. **A STOI improvement threshold on a noise row therefore has to sit above roughly 0.01 before the column decides anything.** Below that it is reporting the draw. This is a property of the material and the effect size, not of the implementation, which matches its reference exactly.

### Where STOI applies

STOI correlates short-time band envelopes against a model fitted to and validated on speech. A pure tone carries no such envelope, so the number it returns there is outside the domain the metric was established in — and reading it anyway is not the conservative choice. It has already scored a *correct* restoration as a regression on tonal items (`sine_click` −0.030, `sine_white_noise` −0.013), which under "no metric may get worse" rejects a good change.

Every item therefore carries `speech_bearing`, and every row repeats it. On a row where it is false, STOI is not computed and `inapplicable_metrics` says why; the other three metrics are read as usual. Of the 95 rows in a full run, 18 carry a STOI value and 33 record it as inapplicable (the rest are chain rows, which never read it).

### Saturation

Segmental SNR clips each frame to `[-10, +35]` dB before averaging. A row whose frames were mostly taken at the ceiling has lost most of its room to move downward, so reading it as "did not get worse" lets the clip hide a regression.

**The column to read is `ceiling_fraction`, not `saturated`.** The boolean says only that the *aggregate* landed on a bound, which happens only when every averaged frame did. A row can lose nine frames in ten to the ceiling and still report an aggregate nowhere near it: `sine_click` on the downmix reads 33.56 dB with 86 of its 92 frames taken at the ceiling, and `saturated` is false. Nine of the thirty-three restoration rows sit above 0.9 and only four of those are flagged by the boolean. Folding this column back down to a boolean is the likeliest regression here; it is a boolean that reads healthy on a crushed row.

A metric supplies these by exposing a companion `<name>_report` taking the same arguments and returning the same value alongside the counts — `segmental_snr_report` for `segmental_snr`. The harness prefers the companion where it exists, so the value and the counts always come from one call, and each row then carries a `saturation` block per metric: `active_frames`, `ceiling_frames`, `floor_frames`, `ceiling_fraction`, `floor_fraction`, `saturated`. A chain metric reported as an input/output pair carries a block per side.

The ledger's `saturation_probe` maps each family's metrics to the report function that produced their block. **A metric absent from it was never checked, which is not the same answer as one that reported a ceiling fraction of zero**, and a pass criterion has to tell the two apart. `--emit-baseline` carries the `saturation` block through, so a baseline row records whether it was crushed when it was recorded.

No threshold is applied here. The numbers are on the row; what counts as too much is for whatever reads them.

**Saturation carries no direction, and a renderer must not colour it as a fault.** `saturated: true` means one thing only: segmental SNR abstains on that row. It arises equally from processing that failed and from processing that succeeded too well — `sine_clip` reads 35.00 both before and after a declip that barely moved it, and `speech_click` reads 35.00 because the declick restored the signal almost exactly (log-spectral distance 5.75 → 0.026 dB). The same true from both directions is the flag working as designed. Direction comes from the other metrics, never from this one, so showing the flag in a warning colour reads a verdict into an abstention.

## Dependencies

The shipped Python wheel declares numpy and nothing else at runtime, and this harness does not get to grow that list. It also does not get a rye project of its own: the three that exist (`bindings/python`, `benchmarks`, `tests/librosa`) each have a different job, and hanging the harness's dependencies off one of them installs them for everyone working in that project instead.

So the harness is **numpy-only and carries its own implementations**, which is already the rule the other `tools/` harnesses follow. STOI in particular is Python-side only and is implemented in `metrics_repair` rather than taken from a package: the published implementation's only hard dependency is polyphase resampling, which is a windowed-sinc FIR. The obligation that comes with the choice is that a self-written STOI has to be checked against the reference implementation's published values before a number from it decides anything.

## The ledger

`run.py` writes a run to `runs/latest.json` (or `--out`): the corpus manifest and seed it ran against, the preset, the downmix rule, any unavailable metric, the saturation probe per family, one row per measurement, and the items it skipped with the reason.

Each restoration row carries `untreated` beside `metrics` — the same metrics between the clean reference and the *unprocessed* input. A processed number on its own says nothing about whether the processing helped. `untreated` carries values only; the `saturation` block describes the processed measurement, which is the one a comparison reads. Each mastering row carries the item's `chain_readiness`, so a row whose dynamics metric could not be computed says so in the row rather than only in the manifest.

`--emit-baseline` writes the same numbers in the shape `baseline.json` holds, keyed by `family/item/defect/entry_point/feed`. It does not write `baseline.json` itself: what the committed baseline says, and what improvement threshold each metric carries, are set by measuring what the current code achieves.
