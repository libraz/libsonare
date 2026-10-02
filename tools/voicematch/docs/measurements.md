# Voicematch measurements

The measurement contract is the data emitted by [`metrics.py`](../metrics.py), [`metrics_hit.py`](../metrics_hit.py), and [`metrics_texture.py`](../metrics_texture.py). Shape measurements use the mono normalized render; raw held-level fields are present only when a raw render is supplied. Run commands from the repository root with `rye run --pyproject bindings/python/pyproject.toml python ...`; use each command's `--help` for the complete option set.

## Pitched measurements

### Fields

| group | current fields | meaning |
| --- | --- | --- |
| identity | `note`, `velocity` | Keyboard note and velocity axes attached to each row. |
| pitch and partials | `f0_hz`, `f0_cents_err`, `harmonics_db`, `modal_hz`, `modal_db`, `modal_ratio`, `ladder_partials`, `inharmonicity_b`, `inharmonicity_partials` | Fundamental, harmonic or measured modal partials, and inharmonicity. |
| spectrum | `centroid_hz`, `odd_even_db`, `tnr_db` | Spectral shape and tonal-to-noise measures; `centroid_hz` is reported but is not a pitched loss term. |
| envelope | `attack_ms`, `attack_fine_ms`, `onset_ms`, `sustain_slope_db_s`, `release_ms`, `release_capped`, `sustain_rms_db`, `skeleton` | The score-anchored attack plus finer onset and envelope probes. `skeleton` contains `init_db`, `early_db_s`, `late_db_s`, and `tail_db_s`. |
| attack shape | `attack_hf_db`, `attack_lf_db`, `attack_peaks` | High/low attack balance and detected attack peaks. |
| modulation | `f0_width_cents`, `vib_cents`, `vib_rate_hz`, `trem_db`, `trem_rate_hz`, `beat_db`, `beat_rate_hz` | Pitch, amplitude, and beating movement. |
| raw level | `peak_dbfs`, `held_rms_dbfs`, `held_crest_db` | Absolute peak, held level, and held crest factor when raw audio is available; shape rows remain normalized. |

The independent profile axes are [`ToneClass`](../toneclass.py), [`ExcitationFamily`](../toneclass.py), and the keyboard interface. A `FitProfile` combines those axes; it does not infer one axis from another. The current classes are `sustained`, `struck-string`, `plucked-string`, `modal`, and `noise`; the excitation families are `bowed`, `plucked`, `hammered`, `struck-modal`, `percussion`, and `other`.

Stringlike classes may use a harmonic ladder and stiffness; modal classes use measured modes found in the signal. A modal voice has no harmonic ladder requirement. `modal_hz` and `modal_db` describe measured partials, and the `modes` term matches those partials when the reference provides them.

Pitch and envelope windows are bounded by the analyzed note duration. `attack_ms` remains score anchored; fine attack and modulation fields are additional probes. A silent or non-comparable row is unscorable rather than a zero measurement.

## Percussion measurements

### Fields

| group | current fields | meaning |
| --- | --- | --- |
| identity and bands | `note`, `velocity`, `bands_db`, `peak_band_hz`, `band_decay_db_s` | Hit coordinates, loudness-normalized one-third-octave bands, peak band, and octave-band decay rates. |
| onset and envelope | `onset_ms`, `attack_ms`, `attack_floored`, `decay_ms`, `decay_capped`, `crest_db`, `level_db` | Transient timing, floor/cap provenance, crest, and normalized hit level. |
| spectral shape | `centroid_hz`, `flatness_db`, `tilt_rise_db`, `tilt_strike_db` | Spectrum, flatness, and body-versus-strike tilt; `bright` is a loss term derived from the measured spectrum. |
| tone and modes | `tone_f0_hz`, `tone_lowest_hz`, `modal_hz`, `modal_db`, `modal_ratio`, `pitch_drop_ratio`, `pitch_drop_ms` | Strongest/lowest mode and mode ratios; pitch drop is measured data and may be represented through the mode terms. `tonal` is a loss term derived from flatness. |
| stereo and texture | `stereo_width`, `modal_density`, `prompt_late_db`, `evolution_db`, `window_flatness_db` | Stereo width, octave-cell density/prompt measures, and early spectral evolution. |
| masks and diagnostics | `modal_density_valid`, `prompt_late_valid`, `evolution_valid`, `evolution_measured`, `window_flatness_valid`, `window_flatness_measured` | Domain eligibility and finite geometry validity. Genuinely unmeasured slots use placeholders; below-floor measured slots retain finite values even when reference eligibility is false. `ring` is a loss derived from decay. |

The ordinary percussion profile uses one-third-octave centers from 50 Hz through 12.5 kHz and octave decay centers from 63 Hz through 8 kHz. `band_edge_hz` is capture/profile metadata, not a hit-row field. A reference-derived band edge and the measured geometry determine which cells can participate; the profile does not promise energy beyond that edge.

`evolve` uses three windows and eight bands (20–60 Hz plus the seven texture bands) over a common untapered 0–240 ms span: 0–15 ms, 15–60 ms, and 60–240 ms; `diffuse` uses the same windows for 1–8 kHz window flatness. The first window uses a falling half-Hann; later windows use Hann. Power is Parseval-normalized and bandwidth-limited. Actual unpadded FFT bins control eligibility; zero padding interpolates the spectrum and does not resolve slow, low-frequency bands. A low band also needs two cycles and two actual bins.

The relative `-60 dB` threshold is a reference-domain eligibility rule, not a measured SNR. `*_valid` identifies reference-eligible cells; `*_measured` identifies finite candidate geometry even below that floor. A finite candidate below the reference floor is compared, while an absent candidate is charged by the loss contract. This prevents a threshold cliff.

## Loss and weights

### Terms

| scope | current terms |
| --- | --- |
| shared | `env`, `mss`, `level`, `crest`, `dyn`, `modes` |
| pitched | `harm`, `cents`, `tnr`, `mod`, `init`, `slope`, `tail`, `hf`, `lf`, `stiff`, `hfdyn` |
| percussion | `band`, `bdecay`, `tilt`, `bright`, `tonal`, `rise`, `strike`, `ring`, `lf`, `kit`, `density`, `prompt`, `evolve`, `diffuse` |

The authoritative term definitions, caps, and reducers are in [`loss.py`](../loss.py), [`loss_dimensions.py`](../loss_dimensions.py), and [`loss_weights.py`](../loss_weights.py). Losses use fixed engineering units recorded by `TERM_UNITS`, not perceptual units: pitch is cents, spectral and level residuals are dB-like engineering quantities, envelope and decay terms retain their declared time/rate units, and texture terms retain their cell residual units. One quality unit means one declared engineering unit for the aggregate term; it does not mean perceptual equivalence.

Pitched harmonic, modal, pitch, noise, envelope, and modulation terms compare the finite cells appropriate to their measurement. Percussion band, decay, tilt, tone, ring, kit, density, prompt, evolution, and diffuse terms use their corresponding cells. `mss` compares the whole timeline. Level terms first remove the median model/reference gain offset, so a common gain does not turn normalized shape agreement into an absolute-level claim. Raw held levels are scored only when both sides provide the raw fields.

Evolution and diffuse reducers retain the cell mean and the worst-quarter cell mean, with at least one cell in the quarter; the aggregate keeps the larger residual. The robust grid reducer separately reports mean, upper-tail, worst row, and note/velocity provenance. It does not report or independently gate every individual cell. Grid terms such as `mss`, `dyn`, `hfdyn`, and `kit` keep their declared grid reducer.

Reference-unknown data supplies no target. Fixed reference-cell reducers charge missing candidates at the cap; other count-tracked terms protect losses against disappearing measurements through anchored coverage penalties in `LossWeights`. `CellCount` records compared, clipped, absent, and skipped cells; `<term>_capped` includes clipped plus absent cells. For evolution and diffuse, ineligible reference cells remain outside the denominator and are reported separately as `evolution_reference_unavailable` and `window_flatness_reference_unavailable`. They do not increment `<term>_skipped`.

Weights are resolved in this order: explicit CLI weight, specification weight, then the `FitProfile`/class defaults in [`toneclass.py`](../toneclass.py) and [`loss_weights.py`](../loss_weights.py). The exact resolved values belong in the current fit/report output; this page does not copy a default numeric weight table. A normalized loss is relative to its starting weighted score; `--raw-loss` keeps the raw weighted sum. Neither display is a perceptual score.

### Quality units

[`fit_quality.py`](../fit_quality.py) reports absolute weighted residuals in the fixed term units and uses one declared unit as the default target for each aggregate and each reported worst row/condition. A term meets that numerical target only with complete known coverage and both residuals at most one declared unit. Unknown or zero anchored counts, lost measurements, and capped, absent, or skipped evidence prevent complete known coverage. Use [`report.py`](../report.py) for resolved weights and quality fields; [Acceptance](acceptance.md#numerical-quality) defines how to read them.

## Coverage

Coverage is assessed against the anchored reference domain and counts. Missing candidate geometry stays charged and does not shrink that domain. Pitched modal and harmonic domains are independent: a missing modal reference does not invent a harmonic target, and a modal candidate is not judged by a harmonic ladder. Percussion edge and texture masks distinguish reference eligibility from finite measured candidate data.

Fit quality must identify the active terms, compared/capped/absent/skipped counts, aggregate residual, worst row/condition, and whether all required dimensions were measured. Use [`reference.md`](reference.md) for capture and probe procedures, [`fitting.md`](fitting.md) for the CLI workflow, and [`development.md`](development.md) for recovery and probe work; those pages own procedures rather than this measurement contract.
