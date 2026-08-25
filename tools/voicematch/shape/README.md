# shape — comparing the picture instead of the summary

`loss.py` in the parent directory reduces each render to about a dozen scalar summaries per note: harmonic ladder, cents error, tone-to-noise, envelope slopes, band levels. That is the right shape for judging whether a voice is broadly in the right place, and it cannot see three of the things an instrument mostly is — a partial that is absent, a decay whose curvature is wrong rather than whose rate is wrong, and a transient with the wrong spectrum. An average over a region has already thrown all three away.

This package compares the two log-frequency spectrograms cell by cell, which is what a person does with the two pictures side by side. It exists because that is what the ear kept asking for, and because doing it by eye per iteration does not scale.

Nothing in it is instrument-specific. The note grid, the velocities, the gate length and the GM program come from the capture definition; each note's inharmonicity is fitted from the reference rather than assumed. Pointing it at a different instrument is a matter of naming that instrument's capture.

## Running it

```
PYTHONPATH=tools/voicematch python -m shape score  --corpus <capture dir> --capture piano --lib build-autofit/lib/libsonare.dylib
PYTHONPATH=tools/voicematch python -m shape fit    --corpus <capture dir> --knobs dump.txt --out fitted.txt
PYTHONPATH=tools/voicematch python -m shape ablate --corpus <capture dir> --knobs dump.txt --overrides fitted.txt
PYTHONPATH=tools/voicematch python -m shape probe  --corpus <capture dir> --overrides fitted.txt --notes 36,60,84
PYTHONPATH=tools/voicematch python -m shape purity --corpus <capture dir> --overrides fitted.txt --notes 36,60,84
PYTHONPATH=tools/voicematch python -m shape takes  --corpus <capture dir> --page <audition dir>
```

`--knobs` is a `SONARE_TUNING_DUMP` file, so the coordinate list and its defaults come from the library rather than from a parse of the source. `--lib` needs a build configured with `-DBUILD_TUNING=ON`; without it the override layer compiles out and every candidate renders identically.

## The five terms

They are kept apart because they name different repairs, and combined as energies so none can be bought with another at a discount. A fit that improves the tone by dulling the strike should read as no improvement, and under a single blended number it reads as progress.

| term | question |
| --- | --- |
| `spectrum` | the two pictures, cell by cell — the tone |
| `onset` | band level and rise time through the first 350 ms |
| `residue` | energy away from the played note's own partials, as a ratio inside one render |
| `invariance` | what is left of every note's residue once each is levelled — a frequency that answers whatever you strike |
| `release` | what the damper leaves behind after note-off |

The last four each exist because the first was measured to be blind to what they ask. The onset occupies six tenths of one percent of the spectrogram's cells, so a fit run on the picture alone trades the strike for the tail every time. The residue escapes through the bed mask, which lets a low ring sit under a treble note for nearly nothing. The release escapes through the weight floor, since a note ringing seventy decibels under its own peak is weighted at three percent. And a resonator that answers every note equally is never a large error on any single one of them, which is what `invariance` is for.

### The seventh term: recurrence

The six above all price a *level*. A bank of resonators at fixed frequencies is not a level — it is a coincidence, the same pitches answering whatever is struck, and it is what the ear calls a bell. Three separate changes to the voice improved every one of the six while making it audibly a chime, and each time the ear caught it after the numbers had not.

`invariance` was written for exactly this and cannot see it. It takes a row-wise minimum across notes of each note's own normalised residue, and a minimum prices invariant energy by its *weakest relative appearance* — so a bank that is a small fraction of a loud note and a large fraction of a quiet one has no weak appearance to be found by. That is the level pattern any fixed bank produces against a keyboard whose notes are not equally loud, which is every keyboard.

So `recurrence` counts instead of measuring: per spectral row, how much more often the model's aftersound peaks there than the instrument's, in percentage points of the note set. It needs the whole note set at once, which is why it is a term and not a probe. It is one-sided at the clip — a real body does recur, and recurring *less* is what `residue` already charges — though the detector underneath is contextual, so the asymmetry is about two and a half to one rather than absolute.

Two things it cannot see, both structural. A bank tuned to the note grid is invisible, because its pitches *are* note pitches and the partial notch removes them; the undamped-treble population is exactly that, and had to be caught by a level measurement instead. And the notch must be capped at `RESOLVABLE_PARTIAL` — a quarter-tone mask fuses into a continuous band above about the sixteenth partial, so an uncapped one marks the whole upper spectrum of a bass note as "the string" and leaves the term nothing to look at.

## The recorded floor

A sampled reference carries its session's noise floor under every note, gated by the sampler: it opens at note-on, scales with the velocity layer, and fades with the release. It passes every test for being part of the instrument except that its spectrum is the same for the lowest note and the highest.

It is also most of the plane, so a comparison that does not know about it spends its budget teaching the model to hiss. `bed.py` freezes the shape, subtracts it from the reference in the power domain, drops the cells it dominates, and charges the model one-sidedly for exceeding it — one-sided because below the floor the reference's true level is unknown and any value is consistent with the capture.

The estimator has to earn its use. `Bed.measure` reports the across-note agreement it found and refuses a shape whose notes disagree, which is what should happen for a reference that has no recorded floor. Measure it at the softest velocity layer the capture holds: the floor scales with the layer while its shape does not, and at the loudest layer the notes genuinely radiate up in the anchor band. On the corpus this was written against that is 1.3 dB of spread at the bottom of the velocity range against 3.7 at the top, and 3.7 is correctly refused.

## Hold-out, ablation, pruning

`search.py` splits the capture's notes into a set the descent sees and a set it never does, alternating so that neither is a register. A move that improves the fit while leaving the hold-out alone has learnt the notes it was shown, and a scalar fit on this corpus was caught doing exactly that twice.

Descent accepts a move against the state at the moment it was tried, which is not the state it ends in — so by the end a move may be carrying nothing, or may be compensating for one made after it. `ablate` reverts each move alone in the final state and prices it honestly; `prune` keeps only those that still pay on the held-out notes. That has consistently improved the hold-out while roughly halving the number of constants a change would have to justify.

## Probes

`probes.py` holds measurements that answer a question rather than drive a search. Every one of them started as a complaint in words, and each was added after the loss was shown to be structurally unable to see what the complaint named:

- `tail_residue` — "a bell-like ring is left." Energy in a band, late, that the struck string cannot account for, normalised by how loud the note was rather than by how loud its floor is.
- `sustain_colour` — "it sounds metallic." Partials 3–10 against partials 1–2 a second and a half in. A note can be entirely made of its own partials and still read as hard.
- `decay_profile` — "it rings too long." Decay rate as a function of partial frequency, which is the slope metal has and wood does not.
- `onset_profile` — "the attack has no richness." Band level and rise time through the strike.

A probe withholds a number rather than guessing one. A rate fitted where the reference's partial has fallen into the recorded floor is a property of the recording, and a metric that skips its unusable points and averages the rest scores an empty set as perfect — a shape this harness has been caught in before.

## How much, versus what kind

The loss counts how much energy sits off the played string's partials. It has no term for what that energy *is*, and two things that measure the same there sound nothing alike: a bank of resonators at fixed frequencies answers every note at the same pitches, which is a cymbal under the music, while a dense body ring is what the instrument does. Three modules separate the two.

- `density.py` — how many things are ringing. Peaks per octave with the played note's own partials notched out, and an envelope statistic that needs no peak-picking threshold at all: a diffuse field's envelope is flat because it is the sum of many independent contributions, and a handful of resonances beat instead. Read against `diffuse_floor`, which measures where the bottom of the scale actually sits by putting noise through the identical path — not against Rayleigh's 0.523, which applies to an unsmoothed envelope and is roughly twice what this returns. The envelope is detrended first, because an exponential decay spreads an envelope wide on its own and without that step a band that merely decayed faster reads as a sparser one.
- `purity.py` — how much of a render is the played string. On-partial against off-partial energy, per window, per note. The number has no absolute zero: how much of the band the partial mask covers depends on the note, so white noise reads as +5 dB for a middle C rather than 0. Every comparison is therefore between two renders of the *same* note, where that offset cancels. Kept per note rather than pooled, because pooling it changed sign depending on whether the ratios or the powers were averaged — which is what notes disagreeing looks like, and the disagreement was the finding.
- `reach.py` — which residuals any parameter can move and which none can. Connectivity and improvement are different claims and only the first is structural; reading improvement alone inverts the answer after a fit has been written back, when every knob sits at its own optimum. Choose the buckets narrowly: a band wide enough that every note has something in it is a band wide enough to hide a hole at one end, and a 30–250 Hz bucket reported healthy while the bottom of it was 18 dB down.

## Phrase takes

`takes.py` reads what `make_audition.py` wrote. It is the only material in the harness that has more than one string moving, and the capture corpus by construction cannot show any of it — one note, struck alone, pedal up. A held chord's bloom, what a pedal adds, and what happens when a string is struck again before it has stopped are all invisible to every other measure here, so a fit against the corpus cannot produce them: not because the search failed, but because the question was never put.

Windows come from the take's own schedule, which the manifest now carries, rather than from a number typed against a phrase the reader cannot see. That is not tidiness — the pedal take's resonance lives between the last note-off and the pedal lifting, and a window a little further on measures the dampers landing instead, which is the opposite mechanism at the other end of the same take.

`band_error` removes one gain measured over a body window and reports the per-band difference in a later one, so "the total is right and the contents are tilted" becomes readable — which is what thinness is. `relative_to` compares each side against its own rendering of a simpler take, so whatever a side gets wrong about how one note decays cancels and what is left is what the phrase added; a raw level difference cannot separate "eight strikes stacked up" from "one strike decays wrong", and that ratio can.

## Tests

`../test_shape.py` runs entirely on synthesised signals. The captured corpus cannot be committed, so a test that needed it would be a test that never runs; synthesis is also the only way to grade an estimator against an answer that is known rather than measured. Both defects the first test run found were real: the stiffness fit was being dragged negative by search windows that held no partial at all, and the cell comparison charged every sub-floor cell at the clip when no bed had been measured.
