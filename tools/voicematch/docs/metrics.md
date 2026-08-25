# Metrics — what a render is reduced to

`metrics.py`. Per analyzable note, computed on a mono mix after both renders are normalized to equal overall RMS. Every field named here appears in `report.json`, and the loss terms in [loss.md](loss.md) are built from these same fields.

Which set a note gets is decided by `toneclass.py` from the GM program, or by `--drum-note`, which switches to the percussion set.

## Pitched notes

- `f0_hz` / `f0_cents_err` — measured fundamental vs equal temperament
- `harmonics_db` — h1..h12 magnitudes in dB relative to h1 (the harmonic profile; the most directly actionable signal for voicing)
- `centroid_hz` — spectral centroid of the sustain window (brightness)
- `odd_even_db` — odd (h3,5,7,9) minus even (h2,4,6,8) balance (e.g. clarinet-ness)
- `tnr_db` — tonal-to-noise ratio (breathiness / bow noise)
- `attack_ms` — 10%→90% envelope rise
- `sustain_slope_db_s` — sustain-window level trend
- `release_ms` — note-off to −40 dB (`+` suffix = capped by the render tail)
- `sustain_rms_db` — per-note level after global RMS alignment (register balance)

### The ladder is a stiff string, and most instruments are not one

Three of those are anchored on `n·f0·√(1+B·n²)`, which describes a **stiff string and nothing else**. For everything else the partials are found rather than predicted:

- `modal_hz` / `modal_db` / `modal_ratio` — the partials **as measured**: prominent isolated peaks, their level relative to the strongest, and their ratio to the note's nominal frequency. This is the only pitched reading a bar, a bell, a plate or a membrane has. libsonare voices GM 8 at 1 : 2.756 : 5.404 : 8.933, GM 10 at 1 : 1.004 : 6.267 : 6.29 : 17.5, GM 14 at 0.5 : 1 : 2.76 : 5.4 : 8.9, and a timpano at 1 : 1.5 : 2 : 2.44 — not one of those lands inside the ladder's ±40 cent window around an integer multiple.
- `ladder_partials` — how many of the twelve ladder bins found a partial rather than the note's own noise floor. A stiff string reports close to twelve; a glockenspiel reports one, and that is the reading which says the ladder is the wrong ruler for it. The `-120 dB` sentinel the ladder writes marks a bin above Nyquist and nothing else, so a bin that found nothing passed every `> -120` test carrying whatever the floor happened to be. Measured on a synthesised celesta against a slightly differently tuned one: ten of twelve bins at the floor on **both** sides, and a confident `harm` of 15.09 made almost entirely of the difference between two noise floors.
- `inharmonicity_b` — the fitted stiffness itself, reported per note and deliberately **not** scored by the timbre terms. See [loss.md](loss.md#every-partial-term-searches-the-strings-own-series).

### Whether the note moves

- `vib_cents` / `vib_rate_hz` — vibrato, as peak-to-peak cents of the tracked fundamental and its rate
- `trem_db` / `trem_rate_hz` — the same for amplitude, 3–9 Hz
- `beat_db` / `beat_rate_hz` — amplitude modulation from 0.3 to 3 Hz, which is what a unison pair, a string section and a chorused pad all are
- `f0_width_cents` — how wide the fundamental is. One string radiates one frequency; several a few cents apart radiate a band, and the width is how far apart they are. It is the one property of an ensemble patch a single note can carry.

### Two attack readings, on different grids

`attack_fine_ms` measures the attack on a 0.5 ms grid. `attack_ms` keeps its 5 ms hop and 10 ms window because every committed profile in `reference/` was measured through it — and measured on synthetic rises that grid reports **5.0 ms for both a 0.5 ms and a 1 ms attack**, and 10.0 for 2, 3 and 5. A struck or plucked string reaches its peak inside that. The percussion path fixed exactly this for itself and the pitched path did not inherit it.

## Percussion hits

In place of the harmonic set:

- `bands_db` — 1/3-octave levels from 50 Hz to 12.5 kHz, dB relative to the loudest band. The percussion analogue of the h1-normalized harmonic ladder: level-blind, so it measures the shape of the spectrum rather than how hard the hit was. Bands more than 60 dB down are floored, so two noise floors do not read as a difference.
- `band_decay_db_s` — decay slope per octave band, each fit from **its own** peak frame. Bands do not peak together; a snare's wire buzz arrives after its shell, and anchoring every band on the broadband onset would read that as a rising slope. A snare and a rimshot can share an onset spectrum and differ entirely in how fast the top of it dies.
- `attack_ms` — onset to envelope peak (not 10→90%: a hit's peak *is* its attack)
- `decay_ms` — peak to −20 dB (`+` suffix = still above it when the window ended)
- `crest_db` — peak-to-RMS over the hit
- `centroid_hz` — broadband centroid. Unlike for a pitched voice, this is worth reading: the register is fixed, so nothing confounds it.
- `level_db` — hit RMS after global RMS alignment

### Most of a kit does have a pitch

"A drum note has no fundamental" is true of a cymbal and false of most of a kit. Six toms, two congas, two bongos, two timbales, an agogo pair, a cowbell, a woodblock, a triangle and a taiko all have a definite pitch, and whether the six toms make an ascending series is the first thing a drummer hears. The 1/3-octave profile cannot report it — a band is four semitones wide, so a tom two semitones out of tune barely moves `bands_db` — while the model exposes the pitch directly through `base_freq_hz`, `mode_ratios` and `pitch_drop`, all of which `--spec auto` offers.

- `tone_f0_hz` — the **strongest** mode: the pitch a listener assigns. Taking the lowest instead was tried and is wrong on exactly the drums it matters for. Against the six tom fundamentals `capture/drums.json` records by hand, the lowest-mode rule reproduces four and reports the two smallest toms at 55.8 and 61.7 Hz against hand measurements of 163–170 and 183–190, inverting the kit's pitch order; those notes carry a shell or air component within 2 dB of the head's tuned mode at about a third of its frequency. The strongest-mode rule reproduces all six and the ascending order `45, 47, 48, 50, 41, 43` exactly.
- `tone_lowest_hz` — the lowest mode, which is a different fact
- `modal_hz` / `modal_db` / `modal_ratio` — as for a pitched note, with the ratios against the lowest mode so the column reads directly against the patch's `mode_ratios` (an ideal circular head is 1 : 1.59 : 2.14 : 2.30 : 2.65)
- `pitch_drop_ratio` / `pitch_drop_ms` — the strike's pitch overshoot and how fast it falls back. A struck head is stretched by the strike and relaxes, which is the difference between a kick drum and a sine blip; `pitch_drop` is a patch field classified into a staged fit's excitation stage, and nothing scored it. Measured on the captured kit, 92 of 282 rows show a drop over 5 %.

### Band decay is a Schroeder integration, and it refuses rather than guesses

`band_decay_db_s` comes from a **Schroeder backward integration** over the −5…−25 dB span, not from a regression on the log envelope, and it refuses rather than guessing when the curve does not fall far enough or is not straight enough (R² under 0.90).

The old estimator was dominated by which frame the peak landed in, and the reference says so: struck at six velocities, the same physical instrument gave rates spanning a median of 43 to 249 dB/s per band with a 90th percentile over 1200, against a loss cap of 60 dB/s. `bdecay` was a saturated constant with no gradient in it. Re-measured through the integration the same spread is **0.36 to 0.62 octaves** against a cap of 3.

### A capture has a bandwidth, and it is not the analysis range

`measure_band_edge` finds it and `profile.py measure` records it as `band_edge_hz` in the reference; a fit resolves it from its own oracle and hands it to every model render (`--band-edge-hz`), so both sides are measured against the same set of bands.

What marks the edge is not where the energy stops — a capture rolling off still has energy above it — but where a band stops **discriminating**. Below the edge a crash, a cowbell, a closed hi-hat and a cabasa read tens of dB apart because they are different objects; above it they converge, because what is left is one shared transfer function. Measured across `reference/drums.json`, the across-instrument spread holds between 21 and 30 dB from 63 Hz to 8 kHz, falls to 9 dB at 10 kHz and to nothing at 12.5 kHz. A level test cannot find that boundary: 57 % of rows are still above the band floor at 8 kHz and 36 % at 10 kHz, so a floor count puts the edge wherever its threshold was chosen.

The edge is applied when the profile is **measured**, not when it is scored, and that is the part a skip in the loss cannot replace. A band profile is normalised to its own loudest band, so a model whose loudest band lands above the reference's ceiling drags every band below it down by however far the wash stood over them — an error the loss then charges across the whole profile. Excluding the band from the normalisation is the only place that can be fixed.

Above the edge, both sides are reported at the floor, which is what `percussion_terms` already skips on: a band the reference has floored is skipped exactly as an absent oracle value is skipped everywhere else here, and `band_bins` reports how many cells the comparison actually charged for. What this does not reopen is the defect the model's radiated upper bound exists to fix — an open-topped wash peaking at 12.5 kHz where the reference peaks at 2.5 — since `peak_band_hz` deliberately stays full-range and the 4–8 kHz bands are well inside the reference's range and still scored.

## Reading a delta

Deltas are model − oracle.

- The oracle is sampled from real players: sustained strings/winds carry **natural vibrato**, which lowers the oracle's TNR and wobbles its sustain slope. A model reading "cleaner, flatter" than the oracle often means "add vibrato/breath movement", not "the oracle is worse". `--w-mod` is the term that can say so.
- MuseScore General quality varies per program; for a suspect program, cross-check with another SoundFont (`--sf2`) before trusting a delta.
- Harmonic deltas are h1-normalized on both sides, so they are immune to level differences but *not* to which harmonic dominates — check `harmonics_db` absolutes in `report.json` when a delta looks extreme.
- `centroid_hz` is deliberately excluded from the loss: it depends on the probe note set (register weighting) and has been an unreliable signal in this harness.

## Fixed resonances — a diagnostic `compare` prints, not a term

`--w-hf` prices the top end four kilohertz at a time, which is the right width for a tilt and the wrong one for a mode. A single lightly-damped resonance rung by the strike is a spike tens of hertz wide; the band average charges for it — on the piano it charges the cap on 41 of 90 cells — but the number it produces says "the 8–12 kHz band is hot", which nobody can act on. So `compare`'s report carries a **fixed resonances** section naming the frequency instead. It stays a diagnostic on purpose: the energy is already priced, and a peak list is a jumpy thing to hand an optimiser besides.

A peak is called a fixed resonance only when it meets **both** conditions, and the reason is that neither alone survives contact with a reference. Recurrence alone — the same frequency on two notes — reported **fifteen resonances on a sampled reference that has none**, because with twenty peaks per note over eighteen kilohertz two notes coinciding somewhere is not evidence of anything. Off-partial alone is per-note and cannot be corroborated. Together they gave three on the model and zero on the reference.

- **Not a partial of the note that played it** — at least 0.35 of the gap to the nearest predicted partial, so a peak pushed off its place by the window's resolution or by the stiffness fit's own error is not mistaken for a free ring. A note whose stiffness fit was too thin to place its partials contributes nothing, and a frequency past `MAX_EXTRAPOLATED_PARTIAL` is reported as unanswerable rather than as an ordinary partial — B enters the frequency as n², so the prediction's error grows as the square of how far past the fit it reaches.
- **Found at the same frequency on at least two notes**, within 150 Hz.

The peak search runs from **40 Hz**, not from the top of the modelled harmonic range. A piano's case and frame modes live between 40 Hz and 5 kHz, so a floor at 4 kHz left this able to see fold-back and undamped filters and nothing structural — which is most of what it was built to find. Lowering it does not cost precision: against a sampled reference, the sub-4 kHz frequencies that survive every other gate came back four in number and on the reference side only. It does need the local baseline to be a real local mean rather than a zero-padded smoothing, since the span is 2 kHz wide and at the bottom of the spectrum that is most of the window; these are dB, so padding pulls the baseline towards 0 and the prominence down with it.

One blind spot stays and one is now addressable. On a voice with no stiffness probed in **octaves**, a frequency exactly midway between the lower note's partials is a quarter of the way between the upper note's, so it can never be off-partial for both — a piano escapes this because stiffness stretches the two series by different amounts, a harmonic voice does not, and there the measure reports nothing rather than guessing.

The other was a soundboard mode **driven** by the string: it appears at a partial and the off-partial test rejects it, so a ring that is genuinely present on every note gets filtered out on exactly the notes that would have proved it. `fixed_resonances(..., recurrence_only=True)` drops that test and buys its corroboration elsewhere — the frequency has to appear on **nearly the whole grid** rather than on two notes of it, which is a stronger claim than either original condition and is what keeps the fifteen-false-positive result from coming back. Whether each finding sat on a partial is reported as `on_partial_notes` rather than used as a filter, and `compare` prints the two lists separately because they are findings of different strength.
