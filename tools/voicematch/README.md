# voicematch — physical-model voice tuning harness

Renders the same MIDI through two synths and reports per-note timbre deltas, so physical-model voicing can be tuned against a reference ("oracle") instead of by ear alone.

- **Model side** — libsonare's GM fallback bank: `Project.import_smf` → `bounce_with_sf2_instrument` with **no SoundFont loaded**, which forces every program through `gm_fallback_map` → NativeSynth physical voices. This is exactly the code path under tuning. The dylib is loaded via `SONARE_LIB_PATH` (defaults to `build-python-shared/lib/libsonare.dylib`), so it always tests the working tree.
- **Oracle side** — fluidsynth over a GM SoundFont, a WAV you rendered yourself, a captured single-note corpus, or an AudioUnit plugin on this machine. All four go through one interface; see [docs/oracles.md](docs/oracles.md).

This file is the entry point and stays one: it holds the loop and the order of the steps, and nothing else. Everything that can grow — flags, fields, findings — lives under [`docs/`](docs/), one page per subject, and that is where an addition goes.

## Where to look

| you want to | go to |
|---|---|
| run something, or look up a flag | [docs/commands.md](docs/commands.md) |
| know what a pattern plays, or how a drum probe differs | [docs/probes.md](docs/probes.md) |
| know what a reported field means | [docs/metrics.md](docs/metrics.md) |
| pick or set up a reference | [docs/oracles.md](docs/oracles.md) |
| know what a `--w-*` term prices, or what the defaults are | [docs/loss.md](docs/loss.md) |
| run, read or trust a fit | [docs/fitting.md](docs/fitting.md) |
| capture a new instrument, read a reference profile, or record a gate | [docs/capture.md](docs/capture.md) |
| listen to a voice — any GM program, with a reference or without one | [docs/audition.md](docs/audition.md) |
| generate training pairs for an amortized inverse | [docs/dataset.md](docs/dataset.md) |
| find the module that does something, or run the tests | [docs/modules.md](docs/modules.md) |
| compare spectrograms rather than summaries | [shape/README.md](shape/README.md) |

## How everything is invoked

Everything runs through the bindings' rye environment, **from the repo root**:

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/<script>.py …
```

Two exceptions and one precondition:

- **`shape` is a package, not a script.** It runs as `PYTHONPATH=tools/voicematch python -m shape …`, still from the repo root.
- **`tools/audition/serve.py`** is a plain server and needs no environment.
- **Rebuild the dylib before comparing** when the synth code changed. `autofit.py` builds its own isolated tree and does not need this; `voicematch.py` and `profile.py` read whatever `SONARE_LIB_PATH` points at.

```sh
cmake --build build-python-shared --target sonare_shared -j8
```

The four environment variables that move any of that are in [docs/commands.md](docs/commands.md#environment).

## The tuning loop

```sh
# 1. Edit voice code (src/midi/synth/...), then rebuild the shared library.
cmake --build build-python-shared --target sonare_shared -j8

# 2. Compare.
rye run --pyproject bindings/python/pyproject.toml \
    python tools/voicematch/voicematch.py compare --programs 40

# 3. Read the deltas, adjust the voice, repeat.
```

Outputs land in `out/p<NNN>_<pattern>/`: `model.wav`, `oracle.wav`, `notes.mid`, `report.txt`, `report.json` (machine-readable, for driving the loop from an agent). Listen with `afplay out/p040_sustain/model.wav`.

## Calibrating a voice, start to finish

The order matters, and each step exists because the one before it cannot answer the question the next one asks.

Steps 1–3 are the only ones that need the reference plugin, and they are already done for a captured instrument: `reference/<id>.json` is committed, so **a voice can be measured, fitted and diagnosed against the grand, the harpsichord, the kit or the organ from a plain clone**, on any platform, with nothing installed. Skip to step 4.

```sh
# 1. What the plugin needs from its host. Measured, not guessed: a sampler that
#    streams from disk drops notes at a settle time that looks generous.
capture.py calibrate --config capture/harpsichord.json

# 2. The grid, one note per process, resumable.
capture.py corpus  --config capture/harpsichord.json
capture.py verify  --config capture/harpsichord.json

# 3. The reference: WAVs -> the numbers a voice is fitted to. Committed, so
#    this is the last step that needs the plugin.
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

## Tests

```sh
python -m pytest tools/voicematch/
```

One file per module, and none of them renders anything. What each covers is in [docs/modules.md](docs/modules.md#tests).
