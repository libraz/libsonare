# voicematch — physical-model voice tuning harness

Renders the same MIDI through two synths and reports per-note timbre deltas, so physical-model voicing can be tuned against a reference ("oracle") instead of by ear alone.

- **Model side** — libsonare's GM fallback bank: `Project.import_smf` → `bounce_with_sf2_instrument` with **no SoundFont loaded**, which forces every program through `gm_fallback_map` → NativeSynth physical voices. This is exactly the code path under tuning. The dylib is loaded via `SONARE_LIB_PATH` (defaults to `build-python-shared/lib/libsonare.dylib`), so it always tests the working tree.
- **Oracle side** — either fluidsynth fast-rendering a GM SoundFont (`assets/MuseScore_General.sf3`, downloaded from the OSUOSL MuseScore mirror; override with `--sf2` or `VOICEMATCH_SF2`), rendered **dry** (`-R 0 -C 0`) since reverb tails would contaminate release and noise metrics — or **your own WAV** of the probe (`--oracle-wav`), rendered by a VST, a plugin host, or a real instrument. See [Oracle from a WAV](#oracle-from-a-wav).

## The tuning loop

```sh
# 1. Edit voice code (src/midi/synth/...), then rebuild the shared library:
cmake --build build-python-shared --target sonare_shared -j8

# 2. Compare (from the repo root):
rye run --pyproject bindings/python/pyproject.toml \
    python tools/voicematch/voicematch.py compare --programs 40

# 3. Read the deltas, adjust the voice, repeat.
```

Outputs land in `out/p<NNN>_<pattern>/`: `model.wav`, `oracle.wav`, `notes.mid`, `report.txt`, `report.json` (machine-readable, for driving the loop from an agent). Listen with `afplay out/p040_sustain/model.wav`.

## Usage

```sh
voicematch.py compare --programs 40            # one program, default 'sustain' pattern
voicematch.py compare --programs 40-47         # a range
voicematch.py compare --programs 40,42,71      # a list
voicematch.py compare --programs 71 --pattern velocity
voicematch.py compare --programs 40 --notes 55,67,79   # override probe pitches
voicematch.py compare --programs 40 --render-only      # WAVs only, no analysis
voicematch.py compare --programs 0 --drum-note 38      # a percussion instrument, not a pitch
voicematch.py export-probe --programs 0                # write the probe SMF for an external synth
voicematch.py compare --programs 0 --oracle-wav ref.wav  # score against that rendering
voicematch.py room-match --programs 19 --oracle-wav ref.wav  # what sends reproduce its room
```

Patterns (`patterns.py`):

| pattern | what it probes | analyzed per-note |
|---|---|---|
| `sustain` | steady-state timbre at low/mid/high register (per-program ranges) | yes |
| `velocity` | dynamics curve at one pitch (vel 40/70/100/127) | yes |
| `staccato` | attack transients and release | no (too short for spectra) |
| `drum` | one percussion instrument struck at vel 64/100/127, on the drum channel | yes (percussion metrics) |
| `drum-holdout` | the same at vel 48/88/112 — the generalisation check for a drum fit | yes (percussion metrics) |
| `room-probe` | short notes, 4 s gaps — measures a reference's reverberation | no |
| `scale` | legato musicality, listening check | no |

## Metrics (`metrics.py`)

Per analyzable note, computed on a mono mix after both renders are normalized to equal overall RMS:

- `f0_hz` / `f0_cents_err` — measured fundamental vs equal temperament
- `harmonics_db` — h1..h12 magnitudes in dB relative to h1 (the harmonic profile; the most directly actionable signal for voicing)
- `centroid_hz` — spectral centroid of the sustain window (brightness)
- `odd_even_db` — odd (h3,5,7,9) minus even (h2,4,6,8) balance (e.g. clarinet-ness)
- `tnr_db` — tonal-to-noise ratio (breathiness / bow noise)
- `attack_ms` — 10%→90% envelope rise
- `sustain_slope_db_s` — sustain-window level trend
- `release_ms` — note-off to −40 dB (`+` suffix = capped by the render tail)
- `sustain_rms_db` — per-note level after global RMS alignment (register balance)

Deltas are model − oracle. Interpretation caveats:

- The oracle is sampled from real players: sustained strings/winds carry **natural vibrato**, which lowers the oracle's TNR and wobbles its sustain slope. A model reading "cleaner, flatter" than the oracle often means "add vibrato/breath movement", not "the oracle is worse".
- MuseScore General quality varies per program; for a suspect program, cross-check with another SoundFont (`--sf2`) before trusting a delta.
- Harmonic deltas are h1-normalized on both sides, so they are immune to level differences but *not* to which harmonic dominates — check `harmonics_db` absolutes in `report.json` when a delta looks extreme.

## Drums

A drum note has no fundamental, so every metric above that is anchored on one measures a frequency the sound does not contain: MIDI note 38 is the acoustic snare, and reading its "expected f0" as 87 Hz would build a harmonic ladder out of a noise burst. `--drum-note N` therefore switches two things at once — the probe and the metric set.

```sh
voicematch.py compare  --programs 0 --drum-note 38          # snare, model vs oracle
voicematch.py export-probe --programs 0 --drum-note 38      # the same probe for an external kit
autofit.py --spec auto --program 0 --drum-note 38 \
    --optimizer cmaes --max-evals 200 --workers 8 \
    --validate-velocities 48,88,112
```

**The probe moves to MIDI channel 10.** That is what makes a note number select an instrument rather than a pitch; `--programs` / `--program` then selects the *kit* (0 is the standard kit), not a melodic instrument. libsonare routes the channel through `drum_note_table()` and a GM reference synth through its drum bank, so both sides play the same instrument from the same file.

**Velocity is the axis, because a drum note has no register.** The probe strikes one instrument at three velocities and the held-out set is three more (`--validate-velocities`) — the drum equivalent of fitting a violin on three pitches and checking it on three others. `--validate-notes` does nothing for a drum fit; a different drum note is voiced by a different patch, whose knobs this fit never touched.

**The hits are 50 ms long, and for a few notes that is the measurement rather than the instrument.** A drum is a one-shot, so the note-off carries nothing and the gate can be as short as the score allows — true of 58 of the 61 notes on a a sampled GM kit, which render bit for bit identically at every gate from 50 ms to 1600 ms. The exceptions are the record scratch and the two whistles, which that kit voices as held instruments whose length *is* the gate; libsonare voices all three as fixed one-shots, so their envelopes are being compared against a number the probe chose. `--drum-gate-ms` moves it. Nothing else in the kit hears the difference.

Per hit, in place of the harmonic set:

- `bands_db` — 1/3-octave levels from 50 Hz to 12.5 kHz, dB relative to the loudest band. The percussion analogue of the h1-normalized harmonic ladder: level-blind, so it measures the shape of the spectrum rather than how hard the hit was. Bands more than 60 dB down are floored, so two noise floors do not read as a difference.
- `band_decay_db_s` — decay slope per octave band, each fit from **its own** peak frame. Bands do not peak together; a snare's wire buzz arrives after its shell, and anchoring every band on the broadband onset would read that as a rising slope. A snare and a rimshot can share an onset spectrum and differ entirely in how fast the top of it dies.
- `attack_ms` — onset to envelope peak (not 10→90%: a hit's peak *is* its attack).
- `decay_ms` — peak to −20 dB (`+` suffix = still above it when the window ended).
- `crest_db` — peak-to-RMS over the hit.
- `centroid_hz` — broadband centroid. Unlike for a pitched voice, this is worth reading: the register is fixed, so nothing confounds it.
- `level_db` — hit RMS after global RMS alignment.

The fit weights these as `--w-band` / `--w-bdecay` / `--w-env`, and `--w-mss` works unchanged. `--w-env` defaults to **1** for a drum and 0 for a pitched voice: the gesture is most of what tells two drums apart and only a refinement for a sustained note. A run prints the weights it resolved, so the difference is never silent.

`--spec auto --drum-note N` offers that note's own patch fields (`d038.percussion.wire_buzz`, `d038.amp_env.decay_ms`, …) with the same clamp-derived ranges as a program patch — a bound belongs to the field, so `percussion.wire_buzz 0..4` covers every drum note that has one. `--stages` splits them the same way, with the noise burst, the strike position and the pitch drop as excitation and the mode decay, the wire buzz and the shimmer as decay. A fitted value is written back into the drum table as `t[38].percussion.wire_buzz = ...f;`, the idiom that table already uses for its own per-note corrections.

What is **not** covered: a drum fit moves one note's patch. Mute groups (the hi-hats share an exclusive class), the kit assignment, and anything structural stay where the table put them.

## Oracle from a captured corpus (`--corpus`)

When a capture exists, this is the route to use. `--corpus` points at the directory `capture.py corpus` wrote and the probe *becomes* the capture: its notes, its velocities and its eight-second gate come from the manifest, and the oracle is the captured audio assembled onto that same timeline, one slot per recording.

```sh
autofit.py --spec tools/voicematch/specs/piano_corpus.json --program 0 \
    --corpus <capture dir> --corpus-timbre grand-227 \
    --notes 36,60,84 --velocities 56,120 \
    --optimizer cmaes --max-evals 200 --workers 4
```

It matters more than a convenience flag sounds, because until it existed this harness ran **two pipelines over the same instrument with different stimuli**. `profile.py compare` reported against the captured grid — fifteen notes, four velocities, held eight seconds — and had no search in it. `autofit.py` had the search and scored a stimulus of its own: three notes at one velocity, held two. A value fitted on the second cannot be read off the first, and neither table can say so.

Three properties, all of which the rest of the harness depends on:

- **Slot spacing is the capture's own length**, not a chosen gap, so a note's analysis window is precisely the audio recorded for it and never reaches into its neighbour.
- **The capture's preroll is dropped on assembly**, so a captured onset lands exactly where the model's does. Keeping it would place every reference onset a preroll late and read as an attack that arrives slow on every note of the grid.
- **The oracle is silence wherever the corpus has no recording**, so a grid cut down with `--notes` / `--velocities` stays on the same timeline as the full one — a fit and its hold-out are laid out by the same rule.

The grid is not free: sixty slots of ten seconds is ten minutes of audio per render, so the run prints what it is about to cost before it starts. Cut it with `--notes` / `--velocities`; a six-slot grid is one minute per render and enough to move the decay and level terms.

The capture's dryness is read from the config it names. A dry capture is not given a room; a wet one is measured and the model is placed in a matching space before any metric is taken, exactly as for a supplied WAV.

## Oracle from a WAV

The reference does not have to be fluidsynth. Export the probe, render it wherever the sound you are chasing actually lives, and hand the file back:

```sh
# 1. Write the probe SMF (plus probe.json describing its timeline).
voicematch.py export-probe --programs 0 --pattern sustain
#    -> out/p000_sustain/probe.mid

# 2. Render probe.mid at 48 kHz with the reference instrument — a VST in a DAW,
#    a plugin host, a hardware synth, or a player recorded against the same score.

# 3. Score, or fit, against it.
voicematch.py compare --programs 0 --oracle-wav /path/to/rendered.wav
autofit.py --spec specs/piano.json --program 0 --oracle-wav /path/to/rendered.wav
```

What the harness handles for you:

- **Bit depth and channel count** — PCM 8/16/24/32 and IEEE float 32/64, mono or stereo, including `WAVE_FORMAT_EXTENSIBLE`. A DAW bounce rarely comes out 16-bit.
- **Lead-in silence** — the WAV does not have to start on the first sample. The render's onset-strength function is correlated against an impulse train at the score's note onsets, and the measured offset is removed (reported as `lead-in removed: +NNN ms`). Pin it with `--oracle-offset` or switch it off with `--oracle-no-align`.
- **Length** — trimmed or zero-padded to the probe's timeline.

Sample rate is **not** silently converted: a rate converter's passband error lands in every harmonic the fit reads, so a mismatch is an error telling you to re-render at 48 kHz. `--oracle-resample` overrides that when re-rendering is not an option.

One thing the harness cannot fix, and which will quietly bias a fit:

- **A different note set.** Render the probe SMF as exported. Transposing it, or playing "roughly the same notes" by hand, breaks the per-note analysis windows.

Render the reference dry where you can. Where you cannot — see below.

## Ambience: references that come with a room

Some instruments are never heard dry. A church organ is voiced *for* its building; a sampled or VST cathedral organ has that space baked into the samples and no switch turns it off. Comparing a dry model against such a reference makes every metric lie in the same direction, and the effect is not subtle: on a 2 s hall, the organ's release reads **1220 ms short**, and a fitter handed that loss will spend every envelope knob chasing the building.

So the room is treated as a nuisance parameter (`room.py`). When the oracle is an external WAV, the harness measures the reverberation from the tails between its notes and convolves the *model* with a response fitted to reproduce that same measurement, before any metric is taken. On the same 2 s hall the release delta goes to **0 ms**, and what is left in the loss is timbre. It is on by default and `--room none` switches it off.

What it measures, and what it refuses to:

- **Reverberation time** per octave band, from the −5…−25 dB slope of a Schroeder backward integration. Recovered to within ~25 % on a synthetic round trip.
- **High-frequency ratio** (4 kHz RT60 over 500 Hz). Every real absorber damps highs faster than lows, so a ratio near or above 1 is the *instrument's* ring, not a room — a dry reference organ measures 0.98 — and the measurement is discarded.
- Decays under 0.35 s are reported as no room at all: at that length an instrument's own release is indistinguishable from a small studio, and inventing a room is far more damaging than missing one.

Only an external WAV is measured. The fluidsynth route renders with its effect units off, so anything measured there is the instrument.

**Use `--pattern room-probe` when the reference is wet.** A T20 fit needs the tail to fall 25 dB, which takes `RT60 × 25/60` seconds of silence — 1.0 s for a 2.4 s hall, 1.7 s for a 4 s cathedral. The `sustain` pattern leaves one second after a two-second note, so anything past a small hall is measured on a decay that gets cut off before it has fallen far enough, and what little tail there is arrives mixed with the instrument's own ring. `room-probe` inverts both: 0.25 s notes with 4 s gaps, so the excitation stops long before the room does and the tail is the room. Nothing in it is an analysis note, so it is a probe to measure the space with, not to fit timbre on.

`estimate_room` records the shortest silence it actually had (`Room.tail_window_s`), and `autofit.py` prints a warning naming both numbers when the probe was too short for the decay it measured — the RT60 is then biased low, since what fits in the window is the steepest early part of the decay.

### Writing the room back into libsonare

libsonare already gives each program a default ambience: the GS power-on CC91 of 40, weighted per program by `gm_fallback_sends` (a church organ is scaled 2.2×, a bass 0.4×). `room-match` measures a reference's space and searches libsonare's own controls for the settings that land closest:

```sh
voicematch.py room-match --programs 19 --oracle-wav /path/to/cathedral-organ.wav
```

```
oracle room: RT60 1.95s  tail level +13.3dB  HF ratio 0.72
closest match: CC91 10, reverb_decay 0.28 (gs_effects.kReverbDecayScale=0.4)
  reached RT60 2.07s  tail +15.0dB   residual 0.648
  -> gm_fallback_sends reverb_scale for program 19: 0.25
```

It searches rather than inverting a formula because the two controls are coupled and neither maps to RT60 linearly — measured on program 19, the tank decay gives 1.12 s at 0.28 and 6.45 s at 0.98, while the send moves the measured RT60 too because the instrument's own release sits inside the window. Around 26 renders, one per grid point.

The ambience weights are themselves fittable (`gm_fallback_map.kSendsChurchOrganRev` and friends), so a program whose reference is always a wet one can have its room fitted alongside its timbre.

`--model-sends R,C,D` controls what the model render is given (default `0,0,0`, fully dry; `gs` leaves libsonare's power-on ambience in place).

## Automatic fitting (`autofit.py`)

`autofit.py` closes the tuning loop mechanically: it sets a voice's calibration constants, re-renders the model, and minimises the model-vs-oracle mismatch. Pure Python + numpy; no scipy.

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

### Every instrument is already addressable

Ask the library what it has, rather than reading the source for knob names:

```sh
autofit.py --spec auto --program 40 --dump-knobs --program-only   # this program's voicing
autofit.py --spec auto --program 40 --dump-knobs                  # all ~11k knobs
```

A tuning build reports every key it consulted during a render, with its compiled-in default, so the list is produced by the code rather than by a parse that can go stale. Two kinds appear:

| key shape | what it is | scope |
|---|---|---|
| `violin.bowed_string.bow_force` | one **patch field** of the patch that voices a program | that program (and any that shares the patch) |
| `bowed_string_voice.kRosinDepth` | one **engine calibration constant** | every program on that engine |
| `fam0.piano.brightness` | a **family patch** field, for programs with no override of their own | the eight programs of that GM family |
| `d038.percussion.wire_buzz` | a **drum note's** patch field | that note |
| `gm_fallback_map.kSendsChurchOrganRev` | the program's default **ambience weighting** | that program group |

The patch prefix is the patch's own name, not the program number, because one patch often voices several programs — the dump's `# program NN is voiced by patch 'X'` header line resolves which is which.

`--spec auto` builds the knob list for a program from that catalogue: its patch fields plus its engine's constants. Good enough to start a fit on any of the 128 programs without writing a spec first.

### Where a knob's search range comes from

A patch field's range is **mostly not** a guess. `clamp_synth_patch()` bounds all but thirteen of them, and the dump reports those bounds (`--dump-knobs` prints them as the `min`/`max` columns), so `--spec auto` searches the interval the engine actually accepts:

| clamp bound | what `--spec auto` searches | why |
|---|---|---|
| `0 .. 1` (and any span ≤ 4) | the whole interval, linear | 0 and 1 are both meaningful settings; a window around the default would hide them |
| `1 .. 20000` ms | `default/8 .. default*8`, log, capped by the bound | four decades put the default in the first percent of a linear cube; the clamp still stops the window leaving the space |
| wide, default `0` | *not fitted* | the field is switched off and there is no magnitude to anchor a window on — picking one is the guess the bounds replace |
| none (an engine constant, or one of the thirteen fields the clamp leaves open) | `default/2 .. default*2`, or `0 .. 1` for a normalized-sounding name | the old heuristic, now the fallback rather than the rule |

The thirteen unbounded fields are the eight Karplus-Strong extensions (`ks.body_coupling`, `pluck_style`, `nail`, `pickup_pos`, `dispersion`, `tension_mod`, `octave_mix`, `keyoff_noise`), `bowed_string.stribeck` / `sympathetic` / `polarization`, `pipe_organ.keytrack` — each clamped by its own voice at `start()` rather than in the patch clamp, so the audio is safe and only the reported range is missing — and `percussion.strike_theta`, an angle that reaches a cosine and so only has to be finite.

Write a hand spec when a knob needs something else — a zero-default field you want switched on, or a range wider than eight times the default.

**A range that does not contain the answer is the most expensive failure this tool has**, because nothing about the result looks wrong: the search reports the best point it was allowed to visit, so the value pins to the bound, the loss goes down, the diff is small and the report is clean. `piano_voice.kTrebleDecayOct` was searched over `[0.5, 3.0]` while the value it wanted was 5.0, and 3.0 is what such a run reports. So every run ends by naming any knob sitting on an end of its range, **including one that started there** — a spec whose range no longer covers a constant's default has that default clamped in on load, and the knob then reads as unchanged for the rest of the run because by a start-to-best measure nothing happened. A pinned knob is not automatically wrong; a clamp bound is a real end of the space. What is never safe is reading it as an interior optimum.

### Knobs: runtime or source

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
- **Any source knob in the spec puts every evaluation back behind a rebuild.**

Both kinds can be mixed in one spec.

### Write-back

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

### Making a constant tunable

Replace the declaration:

```cpp
constexpr float kStrikeNoiseInject = 0.298027f;   // before
SONARE_TUNABLE(kStrikeNoiseInject, 0.298027f);    // after (+ #include "util/tunable.h")
```

In a normal build the macro expands to exactly the `constexpr float` it replaced — same storage, same codegen, no lookup. Only `-DBUILD_TUNING=ON` turns it into a runtime-overridable value, and `autofit.py` sets that flag itself when the spec needs it. A value written mid-expression (`ham_mu_ = 0.229431f;`) has to be lifted to a named constant first; that is a readability improvement anyway. Prefer file scope over function scope: a function-scope declaration is re-resolved on every call, which in a render loop means a table lookup per sample.

Two rules:

- **Never branch on `SONARE_TUNING`.** The two configurations must render identical audio for identical values, or a fitted value transfers nothing back to the shipped build. (Compiling a tuning-only override layer out of a normal build is not a branch on behaviour: with no override set the layer is the identity.)
- **Keys are scoped by file, values are not namespaced within one.** The scope comes from `__FILE__`, so `brass_voice.kBreathBase` and `flute_voice.kBreathBase` are separate knobs, but two declarations of one name in the same file would collide — `autofit.py` refuses to run when it finds that.

### Loss

Per analyzed note, from the same fields `report.json` carries:

- **harmonic profile** (`--w-harm`) — L1 distance of the h1-normalized `harmonics_db` over the first `--n-harm` (default 10) harmonics, the most directly actionable timbre signal;
- **intonation** (`--w-cents`) — absolute f0 cents difference from the oracle;
- **noise floor** (`--w-tnr`) — TNR shortfall, penalised **only when the model is noisier** than the oracle (a model cleaner than the sampled oracle, which carries natural vibrato/breath, is not penalised);
- **temporal envelope** (`--w-env`) — sustain slope, release and attack differences: the ring-down signature a purely spectral match is blind to;
- **skeleton** (`--w-init` / `--w-slope`) — per-harmonic onset ladder and decay slopes, which separate the *excitation* spectrum from the *loop* decay that the time-averaged harmonic term conflates.

Plus one whole-render term:

- **multi-scale STFT** (`--w-mss`) — magnitude distance at four FFT sizes (512/1024/2048/4096), log and linear. This is what sees everything the metric set does not model: inharmonicity, structure between the harmonics, attack detail, the tail past the sustain window. Phase is ignored, since two renders of the same note are never phase-aligned.

`centroid_hz` is deliberately **excluded** — it depends on the probe note set (register weighting) and has been an unreliable signal in this harness.

And four terms that exist because everything above is a *shape*, normalised past the very things a listener notices first:

- **aftersound** (`--w-tail`) — the per-harmonic decay slope 2–6 s in. It has frames to fit only when the probe holds a note that long, which is why it belongs with `--corpus`: on a piano the aftersound is most of the note, and a model whose C6 fell to nothing in 1.6 s of an 8 s hold used to score identically to one that held for eleven, because both were measured over the same first two seconds.
- **crest** (`--w-crest`) — peak minus held RMS, per note. A difference of two levels from one render, so any gain common to both cancels and it needs no calibration at all. It is the only term that can fail on a note whose envelope never falls after its attack: a model 3.9 dB over the reference at the peak and 8.7 dB over in held RMS is not loud, it is not decaying, and every normalised term says it is correct.
- **level balance** (`--w-level`) — how loud each note is relative to the others, with the grid's own median offset removed first. Absolute dBFS is not comparable between a model rendered here and a reference captured through somebody else's output stage, and a term that treated it as comparable would spend the fit's budget on an output gain. The removed offset is printed once per run, since a model uniformly 9 dB over its reference is worth knowing about even though no voicing knob should be the one to fix it.
- **attack tilt** (`--w-hf`) — the high-band balance in 20 ms slices over the first 120 ms, each band relative to that slice's own broadband level. Level-blind by construction, and it catches what a whole-timeline distance averages away: a 43 ms excess of +53 dB at 20–24 kHz on a note whose midband matches within half a decibel is a tick, and the ear finds it instantly.

**Every weight except `harm`, `cents` and `tnr` defaults to zero.** A run left on the defaults scores a time-averaged harmonic ladder, an intonation error and a noise floor and nothing else — no envelope, no decay, no level, no attack. Rather than carrying that in your head, put the weights in the spec, which then makes a fit reproducible from one file:

```json
{ "weights": { "harm": 1, "slope": 1, "tail": 2, "crest": 2, "level": 2, "hf": 1 },
  "knobs": [ { "tunable": "piano_voice.kTwoStageWidthOct", "min": 0.3, "max": 3.5 } ] }
```

An explicit `--w-*` on the command line still wins; the bare-array spec form is unchanged. `specs/piano_corpus.json` is the worked example.

**Each term is normalised to its value at the start point**, so the start scores exactly `1.0` and a reported `0.70` is 30 % better than the compiled-in values. Without that the weights would not mean what they say: the harmonic term is an L1 sum over ten harmonics in dB and runs to tens, the intonation term is a handful of cents, the multi-scale term is a fraction, so weighting them in their own units hands whichever is numerically largest an influence nobody chose. `--raw-loss` restores unit weighting.

### Optimiser

- `--optimizer coord` (default) — coordinate descent, golden-section per knob. Readable, and fine when a knob has an obvious optimum, but it stalls on knobs that trade against each other: a level and the taper that undoes it send it back and forth without either step being wrong on its own. Inherently serial — each probe is chosen from the previous one's result — so `--workers` does not help it.
- `--optimizer cmaes` — covariance-matrix adaptation. It learns that correlation and steps along it. Tune with `--population`, `--sigma0`, `--seed`; samples are clipped into each knob's range rather than penalised, so an optimum pinned to a bound is reported as such (widen the range, or accept that the model cannot go further in that direction). `--restarts N` restarts from a fresh random point with a doubled population when a run stalls, sharing `--max-evals` rather than multiplying it.

Use `cmaes` once the spec is runtime knobs only — that is the case where the evaluation budget is large enough for it to pay off.

**`--workers N`** renders a CMA-ES generation's whole population concurrently. The candidates are independent subprocesses, so the only thing serialising them was the loop that launched them; scoring stays on the main thread in submission order, so the trajectory and the log are byte-identical to a serial run at the same `--seed`. Measured on this machine, a 120-evaluation organ fit: **71 s at `--workers 1`, 30 s at `--workers 8`**, with identical losses. The speedup is well under 8× because a fixed ~12 s of build, catalogue dump and oracle resolution is not parallelised and the render itself is not single-threaded. A spec containing a source knob is forced back to one worker, since a rebuild rewrites the tree every render reads.

### Cutting the problem down

49 knobs for a violin, 21–114 across the 128 programs. CMA-ES learns a covariance whose cost grows with the square of the dimension, so a budget that would comfortably fit ten knobs does nothing at fifty. Three levers, all optional:

- **`--screen`** probes each knob at both ends of its range with everything else at its default, and fits only the ones that move the loss by at least `--screen-threshold` (default 0.002 — 0.2 % of the start). The dropped knobs are always listed with their measured effect; a silently narrowed search reads afterwards as a search that covered everything. It costs `2n+1` evaluations, so it pays for itself only when the budget is several times the knob count — the tool says so when it is not.
- **`--stages`** fits the excitation knobs against the onset evidence (`--w-init`), then the decay knobs against the decay evidence (`--w-slope` / `--w-env`), then everything under the weights given on the command line. A brighter excitation with a faster decay and a duller one with a slower decay produce nearly the same average spectrum, so asking one search to set both at once sends it wandering along that ridge; `skeleton_note` already separates the evidence, and this is what uses the separation. Classification is by field name, and the final stage takes every knob, so a misclassification costs efficiency and never reach.
- **`--validate-notes 48,60`** scores the result on notes the fit never saw and reports both. It is the only thing in the run that can say whether the values generalise — the fit's own objective is the number it minimised. Three probe notes are enough to pin a physical voice into a configuration that is right at those three and wrong a fifth above.

**Build isolation** — a dedicated build dir (`--build-dir`, default `build-autofit`) is configured with `-DBUILD_SHARED=ON`, plus `-DBUILD_TUNING=ON` when the spec has runtime knobs (a cache left at the other setting is reconfigured, since a library that ignores every override would fit a perfectly flat loss for no visible reason). Each model render runs in a fresh subprocess with `SONARE_LIB_PATH` pointed at that dir's dylib, so a rebuilding run never reads a dylib already mapped into the process, and a runtime-knob run gets its overrides into the environment before the library's static initialisers read them. `build-python-shared` is refused as a build dir.

**Safety** — the pristine text of every target file is snapshotted at startup and restored in a `finally` block, so an exception or Ctrl-C never leaves the tree perturbed. On a normal run the best values are then written back and a unified diff plus the loss trajectory are printed; `--dry-run` restores pristine, skips the write, and reports the diff it would have applied.

### What calibration cannot reach (`--diagnose`)

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

Probes whose render had nothing to measure are excluded and counted, since one end of any gain is silence and a silent render matches nothing. That case used to score as a perfect harmonic ladder: a render with no signal reports h1 at 0 dB by definition and every partial above it at the floor, the floor guard skips all of them, h1 matches h1, and the term sums to its best possible value. `loss_terms` now returns unscorable when not one partial above the fundamental survived on both sides anywhere in the probe.

## Oracle from a plugin on this machine (`--au`)

An AudioUnit instrument installed here can be the oracle directly, with no manual render step in between. [aubounce](https://github.com/libraz/aubounce) hosts it; `au_oracle.py` puts the result behind the same interface as `render_oracle_fluidsynth`, so `compare`, `autofit` and `room-match` all take `--au` without knowing anything changed.

```sh
voicematch.py compare --programs 0 \
    --au "<piano plugin triple>" \
    --au-preset "<close-mic preset>" \
    --au-dry
```

- `--au` takes a plugin name or a `type:subtype:manufacturer` triple; `aubounce list` prints both.
- `--au-preset` takes a `.vstpreset` path or a unique fragment of one, resolved under the macOS preset roots. An ambiguous fragment lists the candidates rather than picking one, because which piano was captured is the whole point.
- `--au-dry` switches off every effect section the plugin advertises. Worth preferring over a preset that merely sounds dry: it is the difference between measuring the instrument and measuring the instrument plus a room, and `room.py` then has nothing to correct.
- `--au-param "Name=value"` sets anything else, repeatable.
- Renders are cached under `out/au_cache/` keyed by the probe, the preset's digest and every host setting, so re-running a comparison costs nothing and a changed preset is a different recording rather than a stale hit.

The binary is found via `AUBOUNCE`, then `PATH`, then a sibling `../aubounce` checkout.

**Five ways a plugin produces a plausible file rather than an error**, none of which announces itself. Three are detected:

| what happens | what you get | the guard |
|---|---|---|
| rendered faster than real time | the note starts correctly and goes silent in the middle | aubounce reports `dropout_ms`; a non-zero value is refused |
| not given long enough to load | the right length, the right shape, a peak three orders of magnitude too small | a peak below the floor is refused |
| leaking on load | energy before the first note | reported, not refused — a plugin is allowed a tail |

Two cannot be, because nothing in the file distinguishes them from a good render, so they are prevented instead:

| what happens | what you get | how it is avoided |
|---|---|---|
| the plugin's first note is not its steady one | a probe whose softest hit — the one a drum fit is validated on — is louder and thinner than the same velocity struck again | aubounce's `--warmup` strikes one note and discards it before recording. On by default; `--au-no-warmup` opts out |
| the probe's program change is obeyed | the wrong instrument, at the right length, with a clean preroll and a healthy peak | the program change is stripped. `--au-gm` keeps it, for a plugin that really is a GM synth |

Measured on the piano sampler, and the numbers are why the defaults are what they are: rendered at full speed a 2 s C4 loses 1544 ms out of its middle; at `--au-settle-ms 1000` it renders a peak of 0.0011 where the real one is 0.158. Two seconds of settling is enough and the default is twice that. `capture.py calibrate` measures all of this for a given plugin rather than assuming it.

The program change is the one to know about before capturing a drum kit, because it is loud and it looks fine. A probe is written for a GM synth, where the program number selects the kit; a multitimbral rack reads it as an instruction to load a different program into that slot, and loads it asynchronously — so the first note of the probe beats the load and every later one does not. Measured on a a sampled GM kit, one snare struck at 64 / 100 / 127 with the program change left in: peaks 0.610, 0.171, 0.365 and a probe that says the snare gets quieter as you hit it harder. Stripped: 0.610, 0.927, 0.898, with RMS ascending. No guard fires on either.

## Where the audio lives

Four places, and which one a file belongs in follows from whether it can be committed and whether it can be regenerated.

| | what is there | committed |
|---|---|---|
| `tools/voicematch/capture/<id>.json` | the capture definition: plugin, timbres, grid, and the GM program the model answers it with | **yes** |
| `tools/voicematch/reference/<id>.json` | the measurements taken from that capture — the actual calibration target | **yes** |
| `.cache/voicematch/capture/<id>/` | the captured WAVs, one per (timbre, note, velocity), plus their manifest | no, and cannot be |
| `.cache/voicematch/audition/<name>/` | the listening sets rendered from a capture | no |

The captured audio **cannot** enter the repository: a sample library's licence covers what is rendered from it. It also should not, even where a licence would allow it — what a physical model has to reproduce is a handful of measured properties, not four hundred megabytes of one particular instrument. `reference/<id>.json` is those properties, and it is what a fit and a gate are held against.

`SONARE_VOICEMATCH_ROOT` moves that whole scratch root off the checkout, which is worth doing: a capture is expensive — several seconds per note, in real time, with retries — so one worth re-measuring is kept rather than re-rendered, and `profile.py --corpus <path>` reads a corpus from wherever it was kept.

Renders that are cheap to reproduce stay in `out/` and `out/au_cache/`, both gitignored: probe WAVs, comparison output, and the plugin render cache keyed on everything that defines a sound.

## Calibrating a voice, start to finish

The order matters, and each step exists because the one before it cannot answer the question the next one asks.

Steps 1 and 2 are the only ones that need the reference plugin, and they are the only ones already done for a captured instrument: `reference/<id>.json` is committed, so **a voice can be measured, fitted and diagnosed against the grand or the harpsichord from a plain clone**, on any platform, with nothing installed. Skip to step 4.

```sh
# 1. What the plugin needs from its host. Measured, not guessed: a sampler that
#    streams from disk drops notes at a settle time that looks generous.
#    macOS + the plugin + aubounce; see "Writing a capture definition".
capture.py calibrate --config capture/harpsichord.json

# 2. The grid, one note per process, resumable.
capture.py corpus  --config capture/harpsichord.json
capture.py verify  --config capture/harpsichord.json   # what a plausible file can still be wrong about

# 3. The reference: WAVs -> the numbers a voice is fitted to. Committed, so
#    this is the last step that needs the plugin, and the last one already done.
profile.py measure --config capture/harpsichord.json

# 4. The model over the same grid, as one more timbre of the corpus, so every
#    tool that reads a corpus reads the model with no special case.
profile.py render-grid --config capture/harpsichord.json

# 5. Where the voice stands, dimension by dimension.
profile.py compare --config capture/harpsichord.json --timbre baroque

# 6. Values. --spec auto derives the knobs from the library's own catalogue.
autofit.py --spec auto --program 6 --optimizer cmaes --workers 8 --validate-notes 41,65,84

# 7. What the fit could not reach, and whether anything could.
autofit.py --spec auto --program 6 --diagnose --workers 8

# 8. Record what is now worth holding, in the same change as the behaviour.
profile.py compare --config capture/harpsichord.json --timbre baroque \
    --write-gate reference/harpsichord_gate.json
```

Steps 6 and 7 alternate with physics work rather than repeating: a fit converges to the best point reachable from where it started, and a mechanism added to the model moves the anchor, so the next fit starts somewhere new.

**The capture definition names the instrument, and everything downstream reads it from there.** `program` is the GM program the model answers with, `takes` is the phrase set the audition page plays, and `dimensions` narrows what `compare` gates on. Splitting those across a config, a command line and a constant in the source is how a harpsichord ends up measured against program 0 without a word of complaint.

**`profile.py compare` renders the model dry and does not correct for the reference's room.** That correction exists, and it lives in `autofit.py --oracle-wav` (see [Ambience](#ambience-references-that-come-with-a-room)). So a comparison against a timbre recorded in a space reads the space: the model comes back short in decay and fast in damper release by an amount that is the room, not the voice. Compare against a slot known to be dry, and treat a wet one as a listening reference rather than a numeric one.

## Capturing a reference corpus (`capture.py`, `profile.py`)

The oracle route above compares one probe at a time. A corpus is the other half: the whole note-by-velocity grid of a real instrument, measured once, so a voice is fitted to properties rather than to three notes.

**This route needs macOS, the commercial plugin being captured, and `aubounce`, and a fresh clone has none of the three.** It is how the committed references in `reference/` were produced and it is not how they are consumed: `profile.py compare`, `autofit.py` and `--diagnose` all read `reference/<id>.json`, which is committed, so a voice can be scored, fitted and diagnosed against a captured instrument with no plugin present. What needs the plugin is re-capturing — measuring an instrument nobody has measured yet, or re-measuring one after a plugin update.

Without it, the reference comes from somewhere else and everything downstream is unchanged: `--sf2` plays a SoundFont through fluidsynth on any platform, and `--oracle-wav` takes a WAV of the probe rendered by anything at all — a VST in a DAW, a hardware synth, a recorded player. `export-probe` writes the score to render. The audition page has the same shape: `make_audition.py --model-only` needs no plugin and produces a set that plays rather than compares.

`capture/piano.json` and `capture/harpsichord.json` are the definitions: the plugin, its timbres, the notes and the velocities. The grand's holds a long gate and a short tail because on a piano note-off is the damper — the free decay a model has to match happens while the key is still down. The harpsichord's is shorter, because its jack drops the moment the key is released and what follows is a fast damping plus the jack's own noise.

### Writing a capture definition for a new plugin

Every field is discoverable, and none of them should be guessed. `aubounce` answers the first four questions and `capture.py calibrate` answers the fifth.

```sh
cargo install --git https://github.com/libraz/aubounce   # once

# 1. What is installed, and its component triple.
aubounce list

# 2. What is inside it. A rack keeps its contents in a chunk only it can parse;
#    --strings decompresses what is compressed and reports what is readable, so
#    it says what a rack contains without knowing the format. Unlabelled: it
#    does not say which slot holds what.
aubounce info "<rack plugin triple>" --settle-ms 4000 --strings harpsi

# 3. Which MIDI channel plays which slot. This is the one that cannot be
#    guessed and the one a multitimbral rack never publishes: --probe-slots
#    raises one solo control at a time and plays the channels that sound.
#    --probe-channels is the fallback when a plugin has no solo per slot.
aubounce info "<rack plugin triple>" --settle-ms 4000 --probe-slots

# 4. What it advertises as an effect section, which is what `dry` switches off.
aubounce info "<rack plugin triple>" --params reverb

# 5. What the host has to give it. Measured, and reported rather than asserted.
capture.py calibrate --config capture/<id>.json
```

| field | what it is |
|---|---|
| `id` | names `.cache/voicematch/capture/<id>/` and `reference/<id>.json` |
| `plugin` | the triple from `aubounce list` |
| `program` | the GM program the model answers this reference with |
| `takes` | the audition phrase set (`piano`, `harpsichord`) |
| `dimensions` | which `compare` columns the gate holds; empty means all of them |
| `timbres[]` | `id`, `label`, `preset`, and `channel` — the slot, from `--probe-slots` |
| `notes` / `velocities` | the grid. Velocity is an axis in its own right on anything plucked |
| `settle_ms` / `realtime` | from `calibrate`, at twice the measured minimum |
| `gate_ms` / `tail` | how long the key is held and how long is recorded after it lifts |
| `dry` | switch off every effect section the plugin advertises |
| `params` | anything else, as `Name=value` |

**A rack's slot names are not a description of what the slot does.** On the the harpsichord rack the slot called `Digi` decays over 36 seconds and the one called `Ambient` decays in 3. That is why that capture sets `dry: false` and measures each timbre's room instead: the switch cannot be trusted where the effects are per-slot sends rather than a section the plugin advertises. Measure before believing a name, and record which slots came out dry — that decision is what a later `compare` depends on, since it does not correct for a room.

**A `_`-prefixed key is a note to the next reader and is ignored by the loader.** Both shipped definitions use them to record why a value is what it is — which slots were left out and why, what the velocity axis is there to settle, why the gate is the length it is. That is the right place for it: the reasoning belongs next to the number, not in a commit message.

**The captured audio never enters the repository.** A sample library's licence covers what is rendered from it, so the WAVs land in a local untracked directory and it is the measurements that are committed, in `reference/`. They are also the right thing to commit: what a physical model has to reproduce is a handful of measured properties, not one particular piano's four hundred megabytes.

`profile.py` measures six things `metrics.py` does not, none of which its harmonic-series assumption can express. All six are measured for every instrument, and what varies between instruments is which of them is the headline rather than which are taken:

- **Inharmonicity** — a stiff string puts partial *n* at `n·f0·√(1+Bn²)`. Fitted, not assumed, by iterating the search window down as the estimate improves. On the captured C7, B is smallest around C2 (7.6e-5) and rises toward both ends — the shape a real piano has. The harpsichord capture reads 1.5e-6 to 5.7e-6, three orders of magnitude lower, which is the measurement saying its strings are essentially harmonic. Above roughly C6 there are too few partials left under the Nyquist frequency to fit two parameters, and those notes are reported as unfitted rather than as a number; the summary curve names the notes it stopped at, because a curve that quietly stops short reads afterwards as one that covered the keyboard.
- **Stretch tuning** — cents from equal temperament. The grand capture reads the Railsback curve, −14.5 c at A0 rising to −2.7 c at C4; the harpsichord reads inside ±1.6 c everywhere, which is the same measurement reporting a plain equal temperament. A model tuned to exact equal temperament beats against a reference that is not, and the beating is the first thing anyone hears.
- **Double decay** — the knee where the fast initial fall gives way to the aftersound, found by searching the breakpoint rather than assuming one. Where it falls is set by how fast the strings of a unison drift apart, so a model with one string cannot have it.
- **Damper release** — note-off as a damper landing on a moving string. A row that never fell 40 dB inside the capture's tail is shown and left out of the median, since the number recorded for it is the length of the window.
- **Tone-to-noise** — the mechanism against the string. A model with the partial stack right and no action noise reads cleaner than any recording, and every spectral metric scores that as an improvement. The partial window is the wider of ±2 % and three FFT bins, because a relative-only window collapses below one bin in the bass and reports a clean low note as a noise burst.
- **Velocity response** — the level range from the softest blow to the hardest, and whether it is monotonic. On a plucked instrument this is most of the identity: the harpsichord capture reads 5.9–6.6 dB on three of its four slots against a piano's thirty, and a model given the wrong one of those is wrong by 20 dB in a way no timbre metric reports, since every one of them normalises each note by its own fundamental.

### Holding the comparison to what it measured (`--gate`)

`profile.py compare` ends with a per-dimension summary, and it reports **two** numbers per dimension rather than one:

```
                                                  median  |median|  rows
  tuning vs the reference (cents)                    -0.15      0.91    12
  partial stack h2-h6 vs h1 (dB)                     +4.04     13.58    12
  brightness (% of the reference centroid)           -3.50     37.27    12
```

The second column exists because the first cannot fail on a defect that is symmetric across the keyboard. The brightness row once read +0.16 % while individual notes were between 26 and 660 % out — the bass dark by as much as the treble was bright — and the median of the signed errors said the voice was correct to a sixth of a percent.

```sh
profile.py compare --notes 36,60,84 --write-gate reference/piano_gate.json
profile.py compare --notes 36,60,84 --gate reference/piano_gate.json   # exit 1 on a regression
```

Both statistics are bounded, and the gate exits 1 when either is exceeded. What it exists to catch is not a bad voice but **a change that improves one dimension by breaking another**, which is the shape most of this work takes: widening the prompt-decay profile fixes the level on every note between F#2 and F#5 and brightens the same notes by 35 to 60 points of centroid. Both are real, and whoever makes that trade should decide it rather than discover it in a later listening test. Bounds are written at `--margin` (1.25× by default) with a floor under each, deliberately loose — a gate that fails on measurement noise gets switched off, and a switched-off gate catches nothing. Re-record only in the same change as the behaviour that justifies the new numbers.

`capture.py`'s own finding is worth keeping: the piano sampler fails to load its samples on roughly one render in three, and *more* settling time does not help — 4 s and 16 s both produced the real peak while 8 s in between produced silence. So the capture retries, and decides "too quiet" from the loudest velocity already recorded for that same note rather than from an absolute floor, since a real note at the top of the keyboard at velocity 24 is quiet enough that any fixed threshold either lets a failure through or rejects real notes.

## Listening to the result

```sh
make_audition.py                                    # render the phrases
python tools/audition/serve.py <audition-dir>       # the directory it wrote
```

`make_audition.py` renders the same piano phrases through libsonare and through each captured timbre, at one shared gain per take so the level difference between them survives, and writes the manifest [`tools/audition`](../audition/README.md) reads. The phrases are chosen for what a metric will not report: a chord that exposes inharmonicity as beating, an arpeggio, a note struck again while it is still ringing, and a passage under the sustain pedal — couplings between strings rather than properties of one, which the per-note analysis in `metrics.py` never sees.

## Files

- `voicematch.py` — CLI driver (`compare`, `export-probe`, `room-match`)
- `patterns.py` — note patterns + per-GM-program register table
- `render_model.py` / `render_oracle.py` — the model renderer, and the oracle (fluidsynth, an external WAV with score alignment, or a plugin)
- `au_oracle.py` — an AudioUnit instrument as the oracle, hosted by aubounce, with the guards a disk-streaming sampler needs and an on-disk render cache
- `capture.py` / `profile.py` / `capture/` / `reference/` — capturing a reference corpus from a plugin and reducing it to committed measurements; the audio itself stays untracked. `profile.py render-grid` puts the model into the same corpus as one more timbre, so nothing downstream needs a special case for it
- `make_audition.py` — the listening set for `tools/audition`
- `metrics.py` — per-note analysis and deltas
- `smf.py` — minimal type-0 SMF writer (single source of truth for both sides)
- `gm_names.py` — GM program labels
- `room.py` — ambience: measure a reference's space, put the model in it, translate it back into libsonare's sends
- `wavio.py` — stdlib WAV I/O: 16-bit PCM out, any common format in
- `assets/`, `out/` — gitignored (soundfont download, render artifacts)

The fitter is `autofit.py` plus the modules it drives, one per stage of a fit:

- `autofit.py` — the CLI and the loop: probe resolution, the oracle, the `Evaluator` the optimisers minimise, the held-out check, and the `_render_metrics` subprocess each evaluation runs in
- `catalogue.py` — what the library reports about its own knob space under `SONARE_TUNING_DUMP` (defaults, program→patch map, clamp bounds), plus the `SONARE_TUNABLE` declaration scan the write-back needs
- `corpus.py` — the captured single-note grid as a probe timeline and as the oracle for one: the bridge between the capture `profile.py` reports against and the search `autofit.py` runs
- `knobs.py` — what a fit may move and over what range: the spec forms, the clamp-derived search ranges, `--spec auto`, and the one rule for what counts as sitting on a bound
- `loss.py` — from a render to the number being minimised: `probe_rows`, `skeleton_note`, the harmonic and percussion term sets, the level terms, and the start-point normalisation
- `optimizers.py` — coordinate descent with a golden-section line search, and CMA-ES with IPOP restarts
- `staging.py` — cutting the problem down: knob screening and the excitation/decay/all staged fit
- `diagnose.py` — the same probe read per term instead of per loss: what the residual is made of, and which of it no knob reaches
- `writeback.py` — putting a fitted value back: literal splicing, the program table, the drum table
- `report.py` — the end-of-run report and the diff it applies
- `specs/` — knob spec JSONs; `example.json` shows all three knob forms, the rest are hand-tuned per-instrument sets. `--spec auto` needs none of them.
- `test_*.py` — tests (`python -m pytest tools/voicematch/`), one file per module and none of them rendering anything. `test_autofit.py` covers the range rules, loss normalisation, stage classification and write-back path translation; `test_diagnose.py` gives every verdict a case, including the two that look identical when only improvement is measured; `test_profile.py` covers the measurements that are not tied to one instrument

The knob machinery itself lives in the library, all of it behind the `BUILD_TUNING` CMake option: `src/util/tunable.h` (the `SONARE_TUNABLE` macro, the override table, and the `SONARE_TUNING_DUMP` catalogue) and `src/midi/synth/patch_tuning.h` (the per-program patch field layer).

The clamp bounds in the catalogue are measured rather than mirrored: `patch_tuning.cpp` fills every field with a value far outside any interval, runs `clamp_synth_patch`, and reads back what survived. It walks the same field list the override layer already has, so a field added there is bounded for free and no table can drift out of step with `clamp_synth_patch` itself. A field the clamp leaves open comes back at the probe value and is reported as unbounded rather than as a range of ±1e30.
