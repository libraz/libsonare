# voicematch — physical-model voice tuning harness

`tools/voicematch` renders libsonare's GM/GS fallback voices and compares them with an oracle. The same harness captures reference corpora, turns them into committed measurement profiles, searches tunable source values, checks held-out conditions, and prepares listening pages.

## Setup

Run commands from the repository root through the Python environment used by the bindings:

```sh
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/<script>.py --help
```

The command above is the invocation convention for the examples in these pages; each script's `--help` output is authoritative for its current options and defaults. `shape` is a package, so invoke it as `PYTHONPATH=tools/voicematch rye run --pyproject bindings/python/pyproject.toml python -m shape --help`.

Configure and build the native library using the [Python binding instructions](../../bindings/python/README.md). The default GM oracle requires fluidsynth and a SoundFont; pass `--sf2 <path>` to use an installed SF2/SF3 file. Capture-time plugin requirements are in [Reference](docs/reference.md#sources).

## Minimal quickstart

For direct model comparisons, build the shared library after synth changes and point `SONARE_LIB_PATH` at another tuning build when needed:

```sh
cmake --build build-python-shared --target sonare_shared -j8
rye run --pyproject bindings/python/pyproject.toml python tools/voicematch/voicematch.py compare --programs 40 --pattern sustain
```

The default library is `build-python-shared/lib/libsonare.dylib`. `autofit.py` creates an isolated tuning build under `build-autofit` unless `--build-dir` selects another directory. Captured audio, audition renders, datasets, and comparison output are untracked. `SONARE_VOICEMATCH_ROOT` moves capture, audition, and dataset storage; direct comparison output remains under `tools/voicematch/out/`.

## Task map

| task | page |
|---|---|
| choose an oracle, define a capture, store a corpus, map notes, or classify rig and room | [Reference](docs/reference.md) |
| fit a voice, choose knobs, search, validate, diagnose, or write values back | [Fitting](docs/fitting.md) |
| understand the measured terms, coverage, and percussion measurements | [Measurements](docs/measurements.md) |
| decide numerical gates, regression status, bank status, signoff, and listening | [Acceptance](docs/acceptance.md) |
| maintain modules, run checks, test connectivity, generate datasets, or use recovery tools | [Development](docs/development.md) |

The model side loads the library named by `SONARE_LIB_PATH`; oracle routes depend on the command. A capture's tracked definition owns the method and model address, while its local overlay owns commercial product identity. Profile-based comparisons use committed reference measurements. Corpus fitting and diagnosis need the captured WAVs, while the plugin is needed only to capture or render a live AudioUnit reference.

A fit must use a reference at the same instrument boundary; an amplifier or cabinet baked into a reference remains an acceptance target. A room can be measured and applied to the model, while a rig is nonlinear and has no equivalent correction. See [Reference](docs/reference.md) for the capture contract and [Fitting](docs/fitting.md) for the refusal and diagnostic rules.

Use `make voice-status` to inspect the bank roll-up and `make spec-check` to check tracked fit specifications when a tuning build is available. The complete module and verification map is in [Development](docs/development.md).
