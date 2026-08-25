# Generating a corpus to invert from — `dataset.py`

A fit searches from a start point every time it runs. An *amortized* inverse — a model that reads a measurement and predicts the knobs that made it — would instead be trained once and used to seed the search, and what it needs is pairs of (knob vector → measurement). Those do not come from an instrument; they come from libsonare, rendered at settings nobody chose for musical reasons. It is the only file in this directory that deliberately produces audio no one would want to hear.

**Every fit already produces that data and discards it.** `autofit.Evaluator` keys a cache by the knob vector and stores the measured terms against it, and the dict dies with the process. Recovering it would not have been enough anyway, for two reasons:

- **The distribution is wrong.** CMA-ES concentrates its samples wherever it is converging, which is the one region an inverse least needs described. Sampling here is uniform over each knob's own range.
- **The measurement is too small.** A fit collapses each render to about sixteen scalars, and inverting a hundred-odd knobs from sixteen numbers is badly underdetermined. What is written here is the full `probe_rows` output — 98 numbers per note, the harmonic ladder and onset skeleton and attack bands included.

```sh
python tools/voicematch/dataset.py --program 0 --samples 100000 --workers 8
```

`--bank`, `--pattern`, `--notes` and `--velocities` describe the probe exactly as they do for a fit; `--seed` fixes the draw, `--build-dir` defaults to `build-tuning`, and `--out` overrides the destination.

Sampling is uniform in each knob's **own** scale: a knob declared `log` is drawn uniformly in log space. That is the same convention `--spec auto` derives its ranges with, so a unit of sample density means the same thing as a unit of search step, and a magnitude knob spanning three decades is not sampled almost entirely in its top decade.

It resumes rather than restarts, appending to a JSONL whose first line is the manifest carrying the knob order every row's bare value list depends on. `.gz` output is compressed. A render that fails or falls silent is written with `ok: false` and its reason and never dropped — omitting it would teach a model that the region does not exist, when what is true is that it sounds like nothing.

## Two things to know before spending the hours

**Sample 0 is the compiled-in default, and it does not reproduce a default build exactly.** Overrides reach the library as text at six significant figures (`knobs.format_value`, which must also emit valid C++ literals), so a full-precision constant cannot be pushed back in. Measured on program 0 the worst field is the onset skeleton's decay slope at 1.9e-2 dB/s (5.5e-4 relative), the attack bands move 0.01 dB — one unit in their last reported place — and f0 agrees to 6e-9 relative. This is not a `SONARE_TUNING` behaviour difference; the two configurations render identically for identical values, and these are not identical values. The same six figures are why the recorded parameter vector is round-tripped through `format_value` first: a pair whose input is the drawn value while its audio came from the rounded one is mislabelled in every sample.

**The inverse problem is badly conditioned, and the corpus says so before you train on it.** Over 1000 program-0 samples the corpus is not vacuous — no two measurement rows are alike, and the only measurement dimensions with zero spread are each note's h1, which is 0 dB by definition. But only 14 of the 114 knobs reach a rank-correlation above 0.30 with any measurement dimension, 43 land between 0.10 and 0.30, and 57 show no marginal effect at all at that sample count. That is a statement about a *full-random* sweep, in which every other knob is varying at once, and not a claim that those 57 are inert — `--screen`, which moves one knob at a time from the defaults, found 88 of the 114 move the loss. The two measure different things. What it does mean is that an inverse trained to regress parameters will be confidently wrong on the unidentifiable half, which is the argument for using it as an initialiser for the classical search rather than as the answer.

## Cost

Measured throughput on a three-note sustain probe at eight workers: 59 samples in 6.0 s, 2.6 kB per sample gzipped. A hundred thousand samples is therefore about three hours and 260 MB; a million, about a day and 2.6 GB. The corpus lives under the same untracked root the capture corpus uses (`SONARE_VOICEMATCH_ROOT`, else `.cache/voicematch/dataset`).
