# Changelog

## Unreleased

### Bindings & API

- Added `fill_na` / `fillNa` to YIN and pYIN pitch APIs across the C ABI,
  Python, Node, and WASM. The default keeps unvoiced frames as `NaN`; enabling
  the option returns `0` for unvoiced `f0` frames.
- Added time-varying timbre output to `analyze_timbre` / `analyzeTimbre`.
  Results now include per-window brightness, warmth, density, roughness, and
  complexity entries via `timbre_over_time` / `timbreOverTime`.
- Exposed additional librosa-compatible feature, decomposition, effect, and
  loudness APIs across the C ABI, Python, Node, and WASM: spectral contrast,
  polynomial spectral features, zero-crossing indices, pitch tuning, tuning
  estimation, NMF decomposition, nearest-neighbour filtering, interval remix,
  phase-vocoder time scaling, HPSS with residual, multichannel LUFS, and
  EBU R128 loudness range.

### Fixes

- Preserved mixer pan mode when serialising scenes after `sonare_strip_set_pan`.
- Removed a per-call allocation from latest goniometer reads.

### Tooling & internal

- Updated Node/WASM package tooling versions and adjusted parity normalisation
  so digit runs in names such as `ebur128` match C naming.

## v1.2.2 (2026-06-01)

### Breaking changes

- Replaced stdlib exceptions (`std::invalid_argument`, `std::logic_error`,
  etc.) with `SonareException` across the C API, RT, EQ, mixing, mastering, and
  WASM surfaces so all failures throw a single, catchable type.
- Unified the `AutomationCurve` enum across the engine and mixing modules; code
  referencing the previous per-module enums must use the shared definition.
- Aligned binding facade parameter names to the canonical C API and aligned the
  melody/section/acoustic analyzer defaults to the documented values, which
  changes keyword-argument names and default behaviour for existing callers.
- Unified the `bounceOffline` LUFS default between the C API and WASM bindings.

### DSP & analysis correctness

- Fixed EQ/saturation, stereo-image, gate, de-esser, maximizer, and formant DSP
  in the mastering and editing engines.
- Switched the `chroma_cqt` default norm to L-infinity and corrected the chroma
  `fmin`, chord decoding, and overlap growth in the streaming analyzer for
  librosa parity.
- Hardened numerical robustness in feature/core paths, replacing remaining raw
  constants with the centralised `util/constants.h` values.
- Added an FFT null guard and beat-tracker frame-bounds checks, a bus denormal
  guard, BS.1770 surround weighting, and denormal flushing in the voice changer.
- Added the missing `<cstdint>` include so `streaming_reverb` builds under GCC.

### Real-time safety

- Fixed RT thread-safety across the engine, graph, mixing, transport, and
  automation modules; capped insert vectors and documented the `AutomationLane`
  SPSC contract.

### Performance

- Replaced the O(N) LRU promotion with an O(1) splice in the mel/chroma filter
  caches and optimised additional hot paths while hardening API boundaries.

### Bindings & API

- Added imperative `Mixer` strip setters and planar-stereo voice processing,
  hand-written offline effects/dynamics bindings for Node and Python, offline
  dynamics TypeScript typings for WASM, and backfilled Python `.pyi` stubs for
  runtime-exposed analyzer functions.

### Tooling & internal

- Added a cross-binding parity checker (`tools/parity`) that detects default,
  constant/enum, and parameter-name drift between the C++ core and bindings, and
  a realtime voice-changer quality gate in CI.
- Split the monolithic `sonare_c.h` and `sonare_c_daw.cpp` into per-domain
  units, folded offline-analysis boilerplate into a `run_offline` helper, and
  commonised biquad state, `db_to_linear`, and pass/gain processors into `rt/`.
- Added thread-safety contracts to the RT/mixing/engine Doxygen headers.

## v1.2.1 (2026-05-27)

### Bindings & API

- Added a `StreamingRetune` WASM binding (prepare/reset/setConfig/config/
  grainSize/processMono) backed by `editing/voice_changer/streaming_retune.h`,
  with TypeScript types and Vitest coverage.

### CLI

- Added VQT, mel-to-audio/MFCC-to-audio (Griffin-Lim) reconstruction, meter,
  clipping, dynamic-range, stereo, and phase analysis commands.
- Added normalize, gain, fade, biquad filter, and resample processing commands.
- Added tone, chirp, and clicks synthesis generators.

### CI

- Dropped `windows-latest` from the native build matrix; MSVC source-portability
  fixes are retained so building from source on Windows still works.

## v1.2.0 (2026-05-26)

### Mixing engine

- Added the mixing engine surface: channel strips, pan modes, width controls,
  sends, FX buses, goniometer/true-peak metering, JSON scene presets, and
  offline stereo rendering.
- Added channel-strip input trim, insert gain scale/output gain/pan controls,
  external sidechain parameters, bus insert hosting, graph PDC, and scene-loaded
  persistent mixer APIs.
- Added hold and s-curve automation shapes plus per-target insert/send lanes.
- Added automation lanes, scene/preset API, and an AudioWorklet bridge.
- Added a native mixing benchmark target and expanded CI coverage for macOS and
  Windows native builds.
- Added mixing QA coverage for golden hashes, no-allocation process checks,
  graph routing/PDC integration, meter/goniometer snapshots, and CLI/binding
  smoke tests.

### Mastering engine

- Added a monitor bus output with automation telemetry diagnostics and
  sample-accurate, bind-feedback automation routing.
- Made the dynamics processors real-time-safe via channel pre-allocation, with a
  centralised channel preallocation limit.
- Resolved loudness targets per streaming platform and honoured platform
  normalisation.
- Registered ducking and loudnessOptimize processors and added a de-esser
  bandpass Q with stereo preservation.
- Added assistant/profile/streaming-preview JSON output and a configurable
  speech mono-maker amount.

### Analysis & features

- Added a cosine-similarity mode to the tempogram.
- Derived streaming-retune grain size from the sample rate.
- Improved DSP correctness for iSTFT windowing, chroma folding, K-weighting,
  spectral/VQT/iirt/melody/CQT features, and percentile interpolation (now
  matching NumPy's linear interpolation).

### Bindings & API

- Exposed mixing presets and rendering through C, Python, Node, WASM, and CLI
  APIs.
- Exposed mastering assistant/profile/preview, ducking, streaming chord/pattern
  progression, stream window/output-format config, and inverse Mel/MFCC
  reconstruction across the C, Node, and WASM bindings.

### Fixes

- Preserved per-channel mastering state on channel-count change and tightened
  config validation.
- Made engine counters and smoothing atomic and excluded shared strips.
- Fixed exact cumulative sample counting and bounded chroma history in the
  streaming analyzer.
- Dropped the spurious sidechain reset in the Node streaming equalizer.

### Internal

- Centralised numeric constants in `util/constants.h` and routed IIR,
  crossover, and mastering filters through shared biquad/loudness helpers.
- Fixed the stale `SONARE_VERSION_*` macros in `sonare.h` so the runtime
  `version()` reports the correct value.
