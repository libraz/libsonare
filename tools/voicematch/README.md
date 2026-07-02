# voicematch — physical-model voice tuning harness

Renders the same MIDI through two synths and reports per-note timbre deltas, so physical-model voicing can be tuned against a reference ("oracle") instead of by ear alone.

- **Model side** — libsonare's GM fallback bank: `Project.import_smf` → `bounce_with_sf2_instrument` with **no SoundFont loaded**, which forces every program through `gm_fallback_map` → NativeSynth physical voices. This is exactly the code path under tuning. The dylib is loaded via `SONARE_LIB_PATH` (defaults to `build-python-shared/lib/libsonare.dylib`), so it always tests the working tree.
- **Oracle side** — fluidsynth fast-render with a GM SoundFont (`assets/MuseScore_General.sf3`, downloaded from the OSUOSL MuseScore mirror; override with `--sf2` or `VOICEMATCH_SF2`). Rendered **dry** (`-R 0 -C 0`) — reverb tails would contaminate release and noise metrics.

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
```

Patterns (`patterns.py`):

| pattern | what it probes | analyzed per-note |
|---|---|---|
| `sustain` | steady-state timbre at low/mid/high register (per-program ranges) | yes |
| `velocity` | dynamics curve at one pitch (vel 40/70/100/127) | yes |
| `staccato` | attack transients and release | no (too short for spectra) |
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

## Automatic fitting (`autofit.py`)

`autofit.py` closes the tuning loop mechanically: it perturbs one or more numeric calibration literals in the C++ voice sources, rebuilds the shared library, re-renders the model, and minimises the model-vs-oracle timbre mismatch with a pure-Python coordinate descent (golden-section per knob; no scipy).

```sh
rye run --pyproject bindings/python/pyproject.toml \
    python tools/voicematch/autofit.py \
        --spec tools/voicematch/specs/example.json \
        --program 56 --pattern sustain --notes 48,60,72 --max-evals 30
```

The knobs are described by a spec JSON — an array of entries, each locating one literal:

```json
[
  {
    "file": "src/midi/synth/brass_voice.cpp",
    "pattern": "kLipCouple = ([0-9.]+)f",
    "min": 2.0, "max": 8.0, "scale": "linear"
  }
]
```

- `pattern` is a Python regex with **exactly one capturing group** selecting the numeric literal (the surrounding text, including any `f` suffix, is untouched). It must match the file exactly once — zero or multiple matches abort before anything is written.
- `scale` is `linear` or `log` (log optimises in log-space; needs `min > 0`).

**Loss** (mean over the analyzed notes, from the same fields `report.json` carries):

- harmonic profile — L1 distance of the h1-normalized `harmonics_db` over the first `--n-harm` (default 10) harmonics, the most directly actionable timbre signal;
- intonation — absolute f0 cents difference from the oracle;
- noise floor — TNR shortfall, penalised **only when the model is noisier** than the oracle (a model cleaner than the sampled oracle, which carries natural vibrato/breath, is not penalised).

`centroid_hz` is deliberately **excluded** — it depends on the probe note set (register weighting) and has been an unreliable signal in this harness. Term weights are tunable via `--w-harm` / `--w-cents` / `--w-tnr`.

**Build isolation** — a dedicated build dir (`--build-dir`, default `build-autofit`) is configured once with `-DBUILD_SHARED=ON` and rebuilt (`--target sonare_shared`) per evaluation. Each model render runs in a fresh subprocess with `SONARE_LIB_PATH` pointed at that dir's dylib, so every evaluation loads the just-built code (a dylib already mapped into the process would otherwise stay stale across rebuilds). `build-python-shared` is refused as a build dir.

**Safety** — the pristine text of every target file is snapshotted at startup and restored in a `finally` block, so an exception or Ctrl-C never leaves the tree perturbed. On a normal run the best values are then written back and a unified diff plus the loss trajectory are printed; `--dry-run` restores pristine, skips the write, and reports the diff it would have applied.

## Route B: plugin-hosted oracle (optional, not wired)

`probe_grand.py` is a standalone go/no-go probe for hosting real VST3 instruments (Steinberg The Grand 3 / HALion 7 on this machine) headlessly via DawDreamer as a higher-quality oracle. It needs `pip install dawdreamer` in some environment and a real-time warm-up sleep for disk-streaming instruments. If route B is adopted, implement it behind the same interface as `render_oracle_fluidsynth` (SMF bytes in, `(frames, 2)` float32 out).

## Files

- `voicematch.py` — CLI driver (`compare`)
- `autofit.py` — automatic calibration-constant fitter (perturb → rebuild → render → minimise)
- `specs/` — knob spec JSONs for `autofit.py` (`example.json` is a runnable single-knob template)
- `patterns.py` — note patterns + per-GM-program register table
- `render_model.py` / `render_oracle.py` — the two renderers
- `metrics.py` — per-note analysis and deltas
- `smf.py` — minimal type-0 SMF writer (single source of truth for both sides)
- `gm_names.py` — GM program labels
- `wavio.py` — stdlib 16-bit WAV I/O
- `assets/`, `out/` — gitignored (soundfont download, render artifacts)
