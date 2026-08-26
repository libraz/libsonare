# shape — comparing the picture instead of the summary

`loss.py` in the parent directory reduces each render to about a dozen scalar summaries per note: harmonic ladder, cents error, tone-to-noise, envelope slopes, band levels. That is the right shape for judging whether a voice is broadly in the right place, and it cannot see three of the things an instrument mostly is — a partial that is absent, a decay whose curvature is wrong rather than whose rate is wrong, and a transient with the wrong spectrum. An average over a region has already thrown all three away.

This package compares the two log-frequency spectrograms cell by cell, which is what a person does with the two pictures side by side. It exists because that is what the ear kept asking for, and because doing it by eye per iteration does not scale.

Nothing in it is instrument-specific. The note grid, the velocities, the gate length and the GM program come from the capture definition; each note's inharmonicity is fitted from the reference rather than assumed. Pointing it at a different instrument is a matter of naming that instrument's capture.

## Running it

```
PYTHONPATH=tools/voicematch python -m shape score  --corpus <capture dir> --capture piano --lib build-autofit/lib/libsonare.dylib
PYTHONPATH=tools/voicematch python -m shape fit    --corpus <capture dir> --knobs dump.txt --out fitted.txt
PYTHONPATH=tools/voicematch python -m shape ablate --corpus <capture dir> --knobs dump.txt --overrides fitted.txt
PYTHONPATH=tools/voicematch python -m shape prune  --corpus <capture dir> --knobs dump.txt --overrides fitted.txt --out kept.txt
PYTHONPATH=tools/voicematch python -m shape probe  --corpus <capture dir> --overrides fitted.txt --notes 36,60,84
PYTHONPATH=tools/voicematch python -m shape purity --corpus <capture dir> --overrides fitted.txt --notes 36,60,84
PYTHONPATH=tools/voicematch python -m shape admittance --corpus <capture dir> --overrides fitted.txt
PYTHONPATH=tools/voicematch python -m shape takes  --corpus <capture dir> --page <audition dir>
```

`--knobs` is a `SONARE_TUNING_DUMP` file, so the coordinate list and its defaults come from the library rather than from a parse of the source. `--lib` needs a build configured with `-DBUILD_TUNING=ON`; without it the override layer compiles out and every candidate renders identically. That is checked rather than assumed — the first render that carries an override probes the loaded library with `SONARE_TUNING_DUMP` and refuses to go on when nothing comes back, because the failure is otherwise silent and reads as a result: the descent accepts no move, and an ablation prices every constant at exactly zero, which looks like a finding about the voice and is a fact about the build directory.

## Struck pieces, where the note number is not a pitch

A kit answers on MIDI channel 10, where a note number selects an instrument. Two
assumptions that hold everywhere else stop holding there, and neither fails
loudly enough to notice:

**The renderer had no channel.** It rendered every model note on channel 1, so a
capture of a drum kit was compared against `program`'s pitch — note 42 as F#2 on
a grand piano, against a hi-hat. Both sides rendered, every term returned a
number, and `spectrum` read 28.1 where the same comparison on the right channel
reads 16.3. `Signals` now takes the channel and `__main__` reads it off the
capture through `profile.is_percussion`, which is where that distinction already
lived.

**Six of the seven terms mask by the played note's partials, and every window is
an absolute number of seconds.** `note_hz(42)` is a real frequency, so the mask
gets built and notches a pitch that is not in the signal. And a closed hi-hat
has fallen 60 dB 0.67 s after the strike while the aftersound windows begin at
2.0 and 2.5 s, so they measure silence and the floor discipline quietly drops it.

So `pitched=False` selects a different term set and takes every window from the
piece's own decay — body from the strike to 20 dB down, aftersound from there to
60 dB down, which mean the same thing for a hat and for a ride without either
being told how long it is. A kit's pieces are an order of magnitude apart in
length; one set of absolute windows cannot serve them.

| term | struck | why |
| --- | --- | --- |
| `spectrum` `onset` `balance` | kept | need no partial series |
| `invariance` `recurrence` | kept, unnotched | mean MORE here: a kit whose pieces all answer at the same frequencies is one plate wearing several names |
| `residue` | gone | asks how much of a render is *not* the played string; with no played partials the ratio's denominator is empty |
| `release` | gone | straddles a note-off a one-shot voice does not have |
| `density` | new | how many things are ringing |
| `prompt` | new | how the strike separates from the sustain |

### Which density estimator, and the one that does not work here

`density.py` offers two. Graded against fields of 2 to 256 partials in one band,
`modal_density`'s count reads 2, 4, 7, 12, 24, 43, 72 with white noise at 30, and
moves by under 5 % when the same fields are given a decay. `envelope_diffuseness`
over the short, stationary window a struck piece allows reads between 0.22 and
0.25 for every one of them **and** for the noise — the diffuse floor. What varies
when its window is lengthened is the decay inside it, not the texture: forty
decibels of decay in one transform smears every peak by however fast the piece is
dying. So the count is what `struck.py` uses, over a fixed-length slice of the
aftersound rather than the whole of it, and the slice is fixed precisely so that
one piece's count is comparable with another's.

The count saturates. A field denser than roughly a hundred partials in a band
reads about the same as one of seventy, and noise reads about thirty — so a
reading near thirty says "not resolvable as separate things", not "thirty modes",
and two pieces both at the ceiling are not thereby alike.

`modal_density` also needs `notch=False` here rather than a nominal note number:
its notch is uncapped, so a low note's mask fuses into a continuous band and
covers the whole upper spectrum, and the count comes back zero for a signal full
of resonances.

### A gate that threw away what it was for

`prompt` compares each band's share of the strike against its share of the
aftersound, so the overall decay divides out and what is left is how the colour
moved. Its first version gated on both windows — and a band that goes from 40 dB
down at the strike to 163 down afterwards then has no readable aftersound level,
so the estimator dropped it. That band vanishing *is* the measurement: it is the
closed hi-hat that had every band level right over the whole hit and sounded like
a small drum. The aftersound level is floored instead, so vanishing reads as the
largest movement there is, and the mask that decides which bands are asked about
is the reference's — a model silent where the instrument is not is a finding and
not a question withdrawn. Same discipline `admittance` states for its own bands.

### The pieces a gated term cannot be read for

A term the whole set fails is left out of the total and named. A term that
survives on *some* pairs is a harder case: it enters the total at a value
averaged over whatever could be read, and the pairs it could not be read for
leave no trace at all. On a kit that is not a rounding matter, because the pairs
a floor gate refuses are the long ones — a piece still ringing when its
recording ends has no stretch of noise to measure a floor in — and those are the
pieces whose identity *is* the thing the refused term measures.

Measured on the nine metallic pieces of this kit, `density` reads 35 of 45
note-and-layer pairs. The ten it cannot read are both rides, and the reason is
the capture rather than the estimator: the grid's tail was sized on `decay_ms`,
the time to fall 20 dB, and a floor needs the piece to reach its noise floor and
then stay there. Notes 49–59 were recorded to 8 s on that criterion and the two
rides still ring at the end of it. The open hi-hat was in the same position at
the kit's 2 s and is the reason the capture now names it at 5 s.

So every score line carries a `[density 35/45]` when a term is short, and a fit
whose knobs for those pieces cost nothing on that term is reading a silence
rather than an agreement.

Both new terms are gated on the recording's own floor, and that floor cannot be
read from a fixed window either: the plane is padded to the longest piece in the
grid, and padding is exact zeros, which passes as an infinite signal-to-noise
ratio and is then refused as non-finite. Every band drops out, the term averages
an empty set, and an empty average scores as a perfect match. `struck.floor_window`
finds the stretch between the piece's t60 and the last non-zero sample instead,
and a term nothing could be read for is left out of the total and **named** in
the score line rather than entered at zero.

## Three analysis scales, and what each one is for

`spectro.DEFAULT_SCALES` carries 8192, 1024 and 32768 samples. The first two divide the usual way — one resolves partials, the other resolves the attack — and the third exists for a question neither can answer.

**Whether two components come apart is decided by the window length and by nothing else.** A piano's low register is a field of individual resonances rather than a diffuse bottom end, and two of them can sit 3.58 Hz apart. That separation needs at least 0.28 s of signal; 8192 samples is 0.17 s, so at that scale the two are one peak no matter how the peak is treated afterwards. Nothing about transform size, zero-padding or interpolation reaches it — not even the direct-projection refinement `loss.py` uses to place a partial off the bin grid, which is effectively infinite frequency *precision* and still cannot turn one peak into two. 32768 samples is 0.68 s, which resolves them with margin. Precision and resolution are different axes, and only the second one is a window.

A noise bed is measured against one analysis geometry, so **adding a scale invalidates every cached bed**; `Bed.load` says so by name rather than failing on a missing array.

## The six level terms

They are kept apart because they name different repairs, and combined as energies so none can be bought with another at a discount. A fit that improves the tone by dulling the strike should read as no improvement, and under a single blended number it reads as progress.

| term | question |
| --- | --- |
| `spectrum` | the two pictures, cell by cell — the tone |
| `onset` | band level and rise time through the first 350 ms |
| `residue` | energy away from the played note's own partials, as a ratio inside one render |
| `invariance` | what is left of every note's residue once each is levelled — a frequency that answers whatever you strike |
| `release` | what the damper leaves behind after note-off |
| `balance` | each band's share of the render's own energy, over an early and a late window |

`balance` is the one term here that is two-sided everywhere, and that is why it exists. Every other measure gives the model a way to be *less* than the reference for free: the cell comparison weights a cell by the louder of the two sides and floors it eighty-five decibels under the note's peak, which puts the whole aftersound at or near the weight floor; `residue` is gated off once the reference has decayed; `invariance` and `release` only charge for excess. Reading each band against the render's **own** total rather than against an absolute level also means two renders compare with no gain removed — a model can be thirteen decibels loud and still be thin, and every measure that starts by aligning levels answers a different question.

The last five each exist because the first was measured to be blind to what they ask. The onset occupies six tenths of one percent of the spectrogram's cells, so a fit run on the picture alone trades the strike for the tail every time. The residue escapes through the bed mask, which lets a low ring sit under a treble note for nearly nothing. The release escapes through the weight floor, since a note ringing seventy decibels under its own peak is weighted at three percent. And a resonator that answers every note equally is never a large error on any single one of them, which is what `invariance` is for.

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

Descent accepts a move against the state at the moment it was tried, which is not the state it ends in — so by the end a move may be carrying nothing, or may be compensating for one made after it. `ablate` reverts each move alone in the final state and prices it honestly; `prune` keeps only those that still pay on the held-out notes, which roughly halves the number of constants a change would have to justify.

Leave-one-out pricing is the only thing available at that point and it is not additive, so the selection cannot be trusted on the strength of the numbers it was computed from. `prune` therefore renders and scores each candidate set end to end, and works down a ladder of thresholds until one of them gives up no hold-out against the full set — the last rung keeping everything, which is always available. A hi-hat fit is where the ladder came from: every one of eighteen dropped moves priced inside two hundredths of a decibel on its own, and dropping them together cost a quarter of a decibel. Every rung lost, and the honest answer was all forty-six moves. A set of moves that share one mechanism carry nothing apart and a great deal together, and that arrangement is invisible to the ablation.

`prune` is also a subcommand, so a saved override set — a fit's output, or one assembled by hand from several runs — can be re-priced without repeating the descent that produced it.

## What a candidate costs

Rendering dominates: on the drum corpus, three quarters of a candidate's time is the subprocess that renders the grid and the rest is the spectrogram. Both were being paid for every note of every candidate, and on a kit most of that is answering a question nobody asked — each piece is its own patch, so trying a value of `d049.cutoff_hz` re-rendered the hi-hat, the ride and the china to find out that they had not changed.

`scope_overrides` cuts the override string down to the part one note can read: the keys addressed to that note, plus everything addressed to no note at all. `ShapeLoss` keys its per-note analysis on that string, so a candidate that moves one piece re-renders one piece and reads the rest back. A melodic patch field carries its patch's name rather than a note number and so survives scoping for every note, which makes the cache correct on a keyboard and worthless on one — the win is a kit's, because a kit is what has independent patches.

The independence is checked rather than assumed. `identity.py --isolate` renders each piece under a multi-piece override string and under only its own keys and requires the raw bytes to match, and separately requires each piece to hear its own key — a string nothing reads passes the first half perfectly. Everything the cache serves is a decibel value the gain frame cannot move; the gain is a mean over the whole note set, changes whenever any note in it does, and is recomputed every score.

The budget is in megabytes rather than notes, with a floor of two grids. Below that the cache evicts the notes the next candidate is about to ask for, which costs an analysis per note, saves none, and looks exactly like caching.

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
- `admittance.py` — the first half second against the rest of the note. A piano partial has two decay rates, not one: it falls quickly while the component that moves the bridge is still there and slowly once only the component that does not is left. Every other measure here averages across that boundary, so a voice can match the aftersound to within three decibels per second at every partial and still be twenty out on the prompt rate — which is the half second a listener uses to decide what the instrument is, and where "muddy" lives. The module reports both rates pooled by ABSOLUTE frequency, and `collapse` is the test of whether that pooling is legitimate: a termination-side loss does not know which string is driving it, so partials of different notes landing at the same frequency should agree, and where they do not the prompt rate is a per-note quantity and there is no curve to design against. It stops short of quoting a bridge admittance. The conversion exists — a partial makes f0 traversals a second — but the prompt window's rate is the bridge term plus the string's own internal and air losses together, and nothing measurable from a recording separates them.

  **The reference decides which bands appear, and a model's silence is a count rather than an absence.** The rate estimator refuses a window whose envelope dips under the floor, and the floor is the reference's — so a refusal on the model side is not a measurement failure, it is the model having gone quiet where the reference is still sounding. Dropping that partial removed it from the comparison, took its band out of the model's `collapse`, took the band out of the intersection the table was built from, and printed a shorter table whose every remaining row still agreed. Now every band the reference answered is printed whether the model could answer it or not, `n/a` where it could not, with a per-band `quiet` count of the partials it let fall and a closing line naming the bands it missed entirely. Same failure shape as the `-120 dB` ladder sentinel and the empty `loss_terms` average, one level along: a comparison narrowed by the thing being measured cannot report what narrowed it.
- `reach.py` — which residuals any parameter can move and which none can. Connectivity and improvement are different claims and only the first is structural; reading improvement alone inverts the answer after a fit has been written back, when every knob sits at its own optimum. Choose the buckets narrowly: a band wide enough that every note has something in it is a band wide enough to hide a hole at one end, and a 30–250 Hz bucket reported healthy while the bottom of it was 18 dB down.

## Phrase takes

`takes.py` reads what `make_audition.py` wrote. It is the only material in the harness that has more than one string moving, and the capture corpus by construction cannot show any of it — one note, struck alone, pedal up. A held chord's bloom, what a pedal adds, and what happens when a string is struck again before it has stopped are all invisible to every other measure here, so a fit against the corpus cannot produce them: not because the search failed, but because the question was never put.

Windows come from the take's own schedule, which the manifest now carries, rather than from a number typed against a phrase the reader cannot see. That is not tidiness — the pedal take's resonance lives between the last note-off and the pedal lifting, and a window a little further on measures the dampers landing instead, which is the opposite mechanism at the other end of the same take.

`band_error` removes one gain measured over a body window and reports the per-band difference in a later one, so "the total is right and the contents are tilted" becomes readable — which is what thinness is. `relative_to` compares each side against its own rendering of a simpler take, so whatever a side gets wrong about how one note decays cancels and what is left is what the phrase added; a raw level difference cannot separate "eight strikes stacked up" from "one strike decays wrong", and that ratio can.

### The drawn envelope, and why the gain is a finding

`drawn` keeps the level that everything else here removes. Peak, body and floor are read separately from one absolute peak per 15 ms column — the same thing a waveform pane draws — and the interval between them, `spike-body`, is the part a level offset cannot move. A row that is uniformly high with `spike-body` near zero is a version that is **louder**; a row whose `spike-body` has collapsed is a version whose transients no longer stand out of what it is sustaining, which is what "the wave is filled in" means. They look identical on an audition page, because every version of a take is written at one shared gain set by the loudest of them: a model that plays hot draws as a solid slab filling its pane and pushes the reference down it, and the reference then draws as a thin core with the transients standing clear.

Nothing else would have caught it. The note metrics are h1-normalised and level-blind by construction, `band_error` divides the offset out on purpose, and the page draws from the raw buffer — so an output level wrong by the same amount on every take was visible on screen, absent from every table, and shaped exactly like a defect it is not.

The references keep their own rows and are compared against their **median**, with the spread among them printed underneath. Reading that line is not optional: on the arpeggio take one of three captured grands sits 7 dB above the other two, and against that one alone the model's `spike-body` is 12.8 dB out where the median puts it 1.2 dB out — a difference decided entirely by which reference happened to come first in the manifest.

`--reference` names the sources to measure against. A page carrying `role: reference` in its manifest needs no flag; one with several non-`model` sources and no roles is refused rather than guessed at, because `--variant` renders sit beside the references and no rule tells a variant from a reference by its key.

## Tests

`../test_shape.py` runs entirely on synthesised signals. The captured corpus cannot be committed, so a test that needed it would be a test that never runs; synthesis is also the only way to grade an estimator against an answer that is known rather than measured. Both defects the first test run found were real: the stiffness fit was being dragged negative by search windows that held no partial at all, and the cell comparison charged every sub-floor cell at the clip when no bed had been measured.
