# Changelog

## v1.3.2 (2026-06-07)

### Error handling

- All four binding surfaces now throw a structured `SonareError` carrying a numeric `code` and `codeName` that mirror the C ABI `SonareError` enum, replacing bare string-message errors. The Node and WASM packages export `ErrorCode`, the `SonareError` class, and an `isSonareError` guard; the Node and WASM addons route every C-ABI failure through coded-error helpers; Python's previously missing `INVALID_STATE` code was added to close the enum.
- WASM no longer leaks the raw emscripten pointer number that a C++ throw surfaces under classic exception handling: a module Proxy intercepts it and rethrows a `SonareError` reconstructed via `sonareExceptionInfo`.
- The Python CLI distinguishes failure classes through C-ABI-aligned exit codes (usage 2, invalid-parameter 3, file-not-found 4, invalid-format 5, decode-failed 6, out-of-memory 7, not-supported 8, invalid-state 9, generic 10) instead of folding every failure to exit 1; `SONARE_LEGACY_EXIT=1` restores the old all-failures-are-1 contract.

### Inserts & scene validation

- New `masteringInsertParamNames(name)` (Node/WASM) and `mastering_insert_param_names(name)` (Python) enumerate the parameter keys a mastering insert actually reads, for tooling and pre-validation.
- Loading a mixer scene now surfaces insert params that no config builder consumes as non-fatal warnings, readable via `Mixer.sceneWarnings()` / `Mixer.scene_warnings()`. A dedicated `sonare_last_warning_message()` C-ABI channel carries them without polluting the error channel.
- Mixer scene JSON rejects a non-string insert `slot` or send `timing` with an `InvalidParameter` error instead of silently ignoring it.

### Bug fixes

- Synth and built-in-instrument bounce reattunes to each sequential note's pitch instead of freezing every note at the first note's pitch (MIDI dispatch previously stopped after the first render block).
- The `vocalReverbSend` mixing preset's EQ insert uses the `band{N}.*` key schema that `eq.parametric` actually reads, so its high-pass and presence bands take effect.
- The Python CLI `mixing-preset` default is now `vocalReverbSend` (was `basic`).

## v1.3.1 (2026-06-07)

### Engine & clip streaming

- Tempo-sync warp baking now goes through a single shared implementation (`engine::bake_tempo_sync_warp_channel`) with segment-join crossfade smoothing, so realtime bake (C ABI / WASM) and the offline edit compiler produce identical stretched audio instead of three diverging copies.
- Clip page misses are deduplicated per block: a missing page now raises one `kClipPageUnderrun` telemetry event per audio block instead of one per sample.
- Page providers (C ABI, WASM, Node) reject supplied pages whose frame count does not exactly match the configured page size; the OPFS provider retries partial reads until a full page is available and reports supply failures instead of leaving the request hanging.
- The Node binding reuses destroyed clip-page-provider slots instead of growing the handle table.
- The invalid `repitch` + loop + warp-anchors clip combination is rejected at `set_clips` time on every surface.

### Metering & mastering

- True-peak metering covers up to 8 channels (previously 2), and the max true-peak is computed over all channels, so rear/LFE channels on surround buses are no longer silently excluded.
- LUFS mono-energy scaling now triggers whenever exactly one channel is active, fixing mono-gated buses.
- Spectral-repair transfer gain is clamped to ±4× to avoid unbounded output where the mono mix crosses zero.
- Waveform peak buckets filter out non-finite samples; the platform SIMD paths were replaced with a single portable implementation.

### Acoustics

- Late-tail synthesis skips octave bands whose centre frequency exceeds Nyquist when computing the longest RT60, preventing above-Nyquist bands from inflating tail length at low sample rates.
- WASM `synthesizeRir` / `roomMorph` with `crossfadeMs: 0` now keeps the default acoustic crossfade instead of disabling it.

### Serialization & validation

- `project_from_json` validates that the sample rate is finite and within 8 kHz–384 kHz, returning a diagnostic error instead of silently accepting invalid values.
- Mixer scene JSON always serialises `vcaOffsetDb` (previously omitted when zero).
- The Python binding verifies `sonare_abi_version()` at load time and raises `RuntimeError` on mismatch.

### Bug fixes

- Graph nodes size their sidechain channel storage in `prepare()`, removing a potential out-of-bounds access at high port counts.
- The Python CLI bounce WAV writer supports arbitrary channel counts, fixing surround (>2 ch) bounce output.
- The Python engine retains references to active `ClipPageProvider` objects so they are not garbage-collected while clips still use them.
- WASM Web MIDI: `requestMIDIAccess` is invoked with the correct `this` binding, running status is cleared on system-common messages, and incomplete channel-voice messages are dropped.
- WASM live audio: extra `getUserMedia` constraints from the options object are forwarded to the native call, and `stopTracksOnClose` reliably defaults to true.
- The Node binding reads `startPpq` for offline track freeze as a double, preserving sub-millisecond precision, and exports the `EngineCaptureSource` type alias.

### CI

- The publish workflow caps C++ build parallelism and tolerates PyPI re-uploads of already-published artifacts.

## v1.3.0 (2026-06-06)

### New features

- Added paged-clip audio streaming for arrangements too large for memory: a `ClipPageProvider` C handle (create / supply / clear / destroy) backed by atomic page slots feeds the realtime engine lock-free, and the engine reports page misses through a wait-free request queue (`popClipPageRequest`). The WASM binding ships an OPFS-backed provider (`OpfsClipPageProvider`, inline worker) for browser DAWs. Exposed on every binding.
- Added clip warp modes to the engine clip schedule and the edit model — `off` / `repitch` / `tempoSync` with warp anchors; tempo-sync segments stretch through a new chunked, stateful `StreamingPhaseVocoder` (push / process / finalize API).
- Added takes and comp lanes to audio clips (`takes`, `active_take_id`, `comp_segments`) plus loop-recording take capture (`add_loop_recording_takes`); the edit compiler renders comp segments across takes and all of it round-trips through project JSON.
- Added capture-source selection (output bus or live input), record-offset compensation and input monitoring to the realtime engine; the capture status reports both.
- Added display-oriented waveform peak metering: `waveform_peaks` and `waveform_peak_pyramid` produce per-channel min/max buckets for clip drawing at any zoom level.
- Added browser glue to the WASM binding: `bindMicrophoneInput` (getUserMedia → AudioWorklet) and a Web MIDI → engine bridge with port management, CC binding and connection lifecycle.
- Wired live MIDI into the realtime engine on every binding: bind built-in / SF2 / NativeSynth instruments to destinations, queue live keyboard input (note-on / note-off / CC), swap per-destination MIDI FX without hanging notes, bind MIDI CCs to engine parameters, and recover from stuck notes with MIDI panic.
- Completed the headless-DAW edit surface on every binding: clip remove / gain / fade / loop / re-source / duplicate, track remove / rename / route / kind, and automation-lane add / edit / remove — plus overlap policy, tempo segments, time signatures, markers, warp maps, mixer scene JSON, destructive MIDI-FX bake, entity counts, key/chord annotation write-back and opaque assist sidecars.
- Added a built-in polyphonic synth instrument (sine / saw / square / triangle + ADSR, CC64 sustain, channel-mode CCs) so MIDI-only projects bounce to audible output via `bounce_with_builtin_instruments`; an omitted bounce length is auto-derived from the compiled timeline plus the release tail. Offline bounce now renders each track through its channel strip, sends and buses via the scene mixer instead of summing raw clips.
- Added `validate_midi_notes` (flags hanging / unmatched notes in a clip before bouncing) and a non-fatal compile warning when a project bounces MIDI clips with no instrument bound.
- Python `Project.bounce_with_instruments` hosts caller-supplied external instruments during bounce (the `ExternalInstrument` protocol: a `render(channels, num_frames)` callback plus optional prepare / on_event hooks and `latency_samples`).
- The one-shot `analyze()` now returns the complete result — chords, sections, timbre, dynamics, rhythm, melody, form and per-beat strength — on C, Python and Node, matching WASM; melody analysis exposes the pYIN tracker and frame centering on every binding, and Python gains `analyze_with_progress`.
- Added `chord_functional_analysis`: detect chords and label each with a Roman numeral relative to a supplied key, on every binding.
- Completed the Mel round-trip at custom ranges: `mel_spectrogram` / `mfcc` gain explicit `fmin` / `fmax` / `htk` arguments, and the inverse transforms (`mel_to_stft`, `mel_to_audio`, `mfcc_to_audio`) gain matching HTK variants on every binding.
- Added time-varying pitch correction (`pitch_correct_to_midi_timevarying`): follows a caller-supplied per-frame F0 contour (with optional voicing) toward a MIDI target, so vibrato and drift are tracked rather than flattened. Exposed on every binding.
- Extended the room-acoustics module: per-octave-band wall absorption, named material presets (concrete / wood / curtain / carpet / glass) and per-band scattering on RIR synthesis and room morph; the morph path exposes the late-reverb model selector and mixing-time / crossfade tail controls.
- Mixing: added strip send removal (later send indices shift down, and removing a bus drops the sends that targeted it), a non-fatal compile warning when an explicit submix/aux bus has no path to the master, and VCA group gain (`set_vca_group_gain_db` applies only the delta so direct trims survive; per-strip VCA offsets round-trip through scenes).
- Mastering: the streaming chain accepts a precomputed loudness static gain (loudness-enabled configs construct for realtime use); two-input match processors take independent source/reference lengths; integrated LUFS measurement supports surround layouts up to 8 channels with BS.1770 weights; the oversampler and true-peak stages accept factors 1 and 16 (the live meter too); `LoudnessOptimize` reports its latency.
- Metering: display-decimated vectorscope and phase-scope variants and a single-frame spectrum reader for UI consumption.
- Effects: the convolution reverb synthesizes a decaying-noise IR from its parameters when no IR is loaded; multiband imager / dynamic-EQ expose per-band parameters and custom crossover counts; the reverbs and modulation/delay FX are reachable from the one-shot named-processor path; insert names are enumerable; `decompose` gains an NNDSVD warm-start initialiser.
- Streaming: the quantized u8/i16 read paths accept custom quantization ranges (`QuantizeConfig` / `StreamQuantizeConfig`) so loud or quiet streams no longer saturate against the defaults.
- MIDI 2.0 / GM2: the SF2 player decodes MIDI 2.0 banked Program Change and resolves GM2 Bank Select LSB to the variation bank; NativeSynth honours RPN 0 pitch-bend range via Data Entry, with `reset_controllers` restoring the default.
- Added `sonare_synth_enum_names` to the C ABI as the single source of synth enum name tables; Node / WASM / Python (`synth_enum_tables()`) read from it.
- C ABI: `sonare_abi_version` plus versioned analysis/feature PODs, and length-checked inverse-transform variants.
- Python CLI: `project-bounce` / `project-synth-bounce`, `mixing-presets` / `mixing-preset` subcommands, `--fmin` / `--fmax` / `--htk` on `mel`, stereo WAV output for multi-channel bounces, and `mastering-pair-analyze` resamples the reference to the source rate.

- Exposed the patch-driven NativeSynth on every binding surface:
  - New versioned `SonareSynthPatch` C struct: the base is a named catalog
    preset (or the default subtractive patch) and every non-zero field
    overrides the wrapper sections all engines share (oscillator / filter
    model / envelopes / LFOs / glide / body / stereo spread / mod matrix /
    bus). The engine-mode field selects any of the seven synthesis engines.
  - Named preset catalog (`sonare_synth_preset_names` /
    `sonare_synth_preset_patch`): sine, saw-lead, square-lead, sub-bass,
    warm-pad, e-piano, bell, brass, pluck, electric-guitar, harp, marimba,
    glass, organ, drum-kit and acoustic-piano — data-only patches over the
    voiced GM fallback bank. The `drum-kit` preset plays the full GM drum map
    (note-on resolves the struck key's kit piece).
  - Offline bounce (`sonare_project_bounce_with_synth_instruments`) and a
    realtime engine entry (`sonare_engine_set_synth_instrument`) alongside
    the existing built-in/SF2 instruments — live MIDI input plays NativeSynth
    patches.
  - Python (`SynthPatch` / `synth_preset_names()` /
    `Project.bounce_with_synth_instrument` /
    `RealtimeEngine.set_synth_instrument`), Node and WASM
    (`SynthPatch` / `synthPresetNames()` /
    `project.bounceWithSynthInstrument(s)` / `engine.setSynthInstrument`)
    facades accept a preset-name string (a `"va:"` routing prefix is
    accepted) or a patch object with shared enum names.

- Added the NativeSynth realism-polish layer:
  - Body/formant resonance on every voice (the cheap end of commuted
    synthesis): unit-peak-normalized low-Q bandpass mode banks voiced as a
    guitar body, a violin body or the note-tracked wood tube under a
    marimba/xylophone bar, mixed over the dry voice. The GM acoustic
    guitars, harp and wooden mallets now carry their bodies (solid-body
    electrics intentionally do not).
  - Seeded per-voice stereo spread: a deterministic pan scatter per voice
    (0 keeps every voice centre-panned bit-exactly); the GM string, choir,
    organ and pad families spread into a section image.
  - Mix-bus glue: an optional gain-neutral tanh bus drive plus an
    always-on (config-defeatable) DC blocker that keeps the physical-model
    voices' small DC components off the output bus.

- Added a `effects.modulation.ensemble` insert — the Solina-style BBD
  string-machine ensemble: three delay taps per channel swept by a slow and
  a fast 3-phase LFO bank simultaneously, with the BBD bucket-bandwidth
  lowpass on the wet path and inverted right-channel LFO polarity spreading
  a mono source into stereo. Exposed through the insert factory and the
  automatable set_parameter surface on every binding.

- Added an extended-waveguide acoustic-piano mode to the NativeSynth voice —
  the no-SF2 data-free grand sketch. The four piano-defining elements are
  all present: stiff-string dispersion via an allpass cascade in each
  waveguide loop (partials stretch sharp, the inharmonicity growing up the
  keyboard, with the exact loop phase delay compensated so f0 tuning stays
  accurate), a nonlinear felt hammer (Hertz-contact velocity scaling of
  contact time and force plus a felt-stiffness lowpass — hard strikes are
  shorter and brighter), 2-3 coupled micro-detuned unison strings with the
  characteristic two-stage prompt-sound/aftersound decay, and a fixed
  soundboard resonator bank that also radiates the immediate hammer knock.
  The GM acoustic-piano programs play through it.

- Added modal, additive and percussion synthesis modes to the NativeSynth
  voice, completing the mallet / organ / drum coverage of the data-free GM
  floor:
  - Modal resonator bank with physical mode-ratio data (uniform-bar
    glockenspiel 1:2.756:5.404:8.933, deep-arch marimba/vibraphone 1:4:10),
    mallet-hardness velocity weighting, per-mode decay scaling, decay
    stretching and note-off damping; the chromatic-percussion mallets
    (glockenspiel, vibraphone, marimba, xylophone) now ring as modal bars.
  - Additive drawbar organ: the nine Hammond drawbar pitches with stepped
    stop levels, seeded free-running partial phases and the key-click
    contact transient; the GM organ family plays a drawbar registration.
  - Membrane percussion: Rayleigh circular-membrane modes
    (1:1.59:2.14:2.30:2.65) with a descending strike-pitch envelope layered
    under seeded filtered noise; the GM drum kit (kick, snare shell + wires,
    toms, hats, cymbals with inharmonic ring modes) is rebuilt on it, still
    one-shot and bit-deterministic.

- Added a Karplus-Strong plucked-string mode to the NativeSynth voice (the
  guitar / harp / banjo family): a fractional-delay waveguide loop with
  phase-exact tuning compensation, plus the Jaffe-Smith realism extensions —
  decay stretching (low strings ring longer), a pick-position comb on the
  excitation, a velocity-driven dynamic-level lowpass (hard pluck = bright)
  and note-off loop damping (finger/palm mute). The GM fallback bank now
  plays the guitar family (nylon / steel / jazz / clean / muted / overdriven
  / distortion), the orchestral harp and the plucked ethnic family through
  KS patches.

- Added a `saturation.ampSim` guitar amp insert to the mastering insert
  factory (drive -> tone stack -> cab-EQ): an oversampled 12AX7 triode drive
  stage behind one [0,1] drive knob with a drive-scaled pre-emphasis shelf,
  bass/mid/treble tone controls, and a fixed data-free cab voicing (low cut,
  body bump, presence peak, steep 4.8 kHz roll-off) that can be bypassed for
  a DI tone. Reachable from every binding through the existing
  mastering-insert names surface, with drive/tone/presence/level automatable
  via `set_parameter`.

- Added an FM synthesis mode to the NativeSynth voice (the e-piano / bell /
  brass / clav family): a 2-4 operator phase-modulation stack with a small
  algorithm table, exponential operator envelopes, a feedback operator,
  velocity-to-index (brightness) scaling and key-rate scaling (higher notes
  decay faster). The GM fallback bank now plays electric pianos,
  clavi/harpsichord, the chromatic-percussion bells and the brass family
  through FM patches.

- Added a modulation matrix, a second LFO and glide/portamento to the
  NativeSynth voice: up to 8 free-form routings from envelopes / LFOs /
  velocity / key tracking / mod wheel / seeded per-voice random to pitch,
  filter cutoff, amplitude and stereo pan, on top of the hardwired patch
  modulations; portamento glides each new note from the channel's previous
  note through a one-pole pitch ramp. All modulation stays deterministic.

- Added selectable virtual-analog filter models to the NativeSynth voice — the
  core of each classic synth "character": TPT state-variable (SEM family),
  4-pole transistor ladder (ZDF, saturating loop, self-oscillates), diode
  ladder (VCS3 / TB-303 family, coupled-stage ZDF, self-oscillates) and Korg35
  Sallen-Key lowpass (MS-10 / early MS-20, self-oscillates) — plus a
  gain-compensated pre-filter drive stage per patch. All models stay stable
  and zipper-free under per-sample cutoff/resonance modulation and
  self-oscillation is deterministic; the GM fallback bank routes bass, brass
  and synth-lead families through the transistor ladder.

- Added a NativeSynth virtual-analog engine and made it the data-free floor of
  the SoundFont player — MIDI never renders silent for lack of data:
  - Antialiased PolyBLEP oscillators (sine / saw / square / triangle plus a
    seeded deterministic noise source), unison stacking up to 7 oscillators
    with seeded detune and per-voice pitch drift, a TPT state-variable filter
    (low/band/highpass) with cutoff envelope, velocity-to-brightness and
    keyboard tracking, and exponential DAHDSR amplitude/filter envelopes.
  - A patch-driven `NativeSynth` MidiInstrument (16 channels, sustain /
    channel-mode CCs, CC1 vibrato, CC7/11 gain, CC10 pan, pitch bend) built on
    the shared voice pool; rendering is deterministic (seeded per-voice
    variation, no RNG).
  - A GM fallback bank covering all 128 programs by family plus the GM drum
    map (one-shot kick / snare / hats / toms / cymbals / percussion), used by
    the SF2 player whenever a program is not covered by the loaded SoundFont —
    or no SoundFont is loaded at all. `bounce_with_sf2_instruments` and the
    realtime engine's `set_sf2_instrument` therefore no longer require a prior
    SoundFont load; the manifest keeps reporting the honest per-program
    backend (`sf2` vs `synth`).

- Added a GS-compatible SoundFont 2 instrument so MIDI arrangements render with
  real sampled sounds (the SF2 file is host-supplied data; nothing is baked into
  the binaries):
  - SF2 parsing and a 16-part multitimbral player: preset/instrument zone
    layering with generator/modulator semantics (volume + modulation DAHDSR
    envelopes, vibrato/mod LFOs, low-pass filter with velocity tracking,
    exclusive classes, loop modes), the SF2 default modulator set (velocity /
    CC7 / CC11 square-law gain, CC1 vibrato, CC91/93 sends), pitch bend with
    RPN 0 bend range, and deterministic voice stealing.
  - GS architecture on top: variation-bank fallback to the capital tone,
    bank-128 drum kits on channel 10, NRPN part edits (TVF cutoff/resonance,
    TVA envelope, vibrato) and per-note drum NRPNs, GS Reset / GM System On /
    "use for rhythm part" SysEx (recognised both from hosts and from SysEx
    events inside an arrangement), and reverb / chorus / delay send-return
    effects with a per-part drive insert.
  - New C ABI: `sonare_project_load_soundfont` (+ clear / preset count),
    `sonare_project_soundfont_manifest` (reports per-program source backend:
    SF2 or synthesizer fallback), `sonare_project_bounce_with_sf2_instruments`,
    and the realtime-engine pair `sonare_engine_load_soundfont` /
    `sonare_engine_set_sf2_instrument` so live MIDI input plays through the
    SoundFont. Exposed across the Python, Node, and WASM bindings.

- Added a headless DAW / arrangement runtime, exposed through a new project C ABI
  and across the Python, Node, WASM, and CLI bindings:
  - Author projects with audio and MIDI tracks and clips. Clip edits (add /
    split / trim / move), tempo, and routing changes all route through an
    undoable `EditHistory`, so `undo` / `redo` cover every mutation. Musical
    positions are PPQ (quarter notes).
  - Sequence MIDI 1.0 and MIDI 2.0 channel-voice events, set per-clip program /
    bank and a MIDI-FX chain, and route a track's MIDI to a host-instrument
    destination id.
  - Import / export Standard MIDI Files, plus a MIDI 2.0 Clip File (`SMF2CLIP`)
    format that preserves 16-bit velocity, 32-bit CC, per-note controllers, and
    bank-valid Program Change without loss.
  - `auto_tempo` detects and installs a project tempo from audio; `snap_to_grid`
    quantizes a PPQ coordinate to the project grid.
  - `compile` produces a renderable timeline with structured diagnostics, and
    `bounce` renders the project offline to interleaved float audio. Both are
    deterministic; project JSON serialization is byte-stable within one build.
  - New `sonare project` CLI subcommands: `abi`, `new`, `validate`, `compile`,
    `bounce`, `export-smf`, `import-smf`, `export-midi2`, `import-midi2`.
- Wired a flag-gated MIDI sequencer into the realtime engine and added
  audio / MIDI / plugin host integration seams for embedding hosts.

### Concurrency & real-time safety

- Tempo-map publishing moved to a seqlock-backed publisher: the audio thread adopts snapshots through a non-spinning read path at block start (no audio-callback stalls on preemption), the control thread reads its own current copy, and `transport_state_control()` re-derives PPQ from the latest snapshot so callers never observe a stale tempo map.
- Transport position / play-state / tempo-map fields are atomic for safe cross-thread reads; `SeqlockCell::store()` gained a release fence before the sequence bump.
- `CaptureSink` arm / punch / segment state is published through a seqlock, and its audio callback uses the non-spinning snapshot path.
- Engine MIDI sink/source pointers and the captured-frames counter are atomics with acquire/release ordering.
- Mastering processor/preset name accessors use thread-local string buffers so concurrent callers no longer race on a shared buffer.
- `ParamSmoother` targets are atomic so control-thread sidechain resizes are safe against the audio thread.
- `AutomationEngine` can pre-register parameter metadata so the realtime-safe flag is checked before any processor is reached.

### DSP & analysis correctness

- Mastering integrated LUFS is measured via BS.1770 channel summing instead of a mono mix, which under-counted M/S-heavy content by ~3 LU.
- RT60 estimation applies Lundeby noise-floor truncation to the Schroeder decay curve before fitting (falling back to T20 when T30 is unavailable), and C50 / C80 / D50 are anchored at the detected direct sound instead of sample 0.
- The plate reverb maps `decaySec` to an approximate RT60 like the FDN and velvet engines, so the same value yields a comparable tail length across all three.
- The stereo imager derives its allpass coefficients analytically from the sample rate, making the decorrelation timbre rate-independent.
- Melody vibrato is estimated over continuous voiced runs, eliminating spurious zero crossings at unvoiced boundaries.
- `bit_depth` / `dither` clamp the quantized code into the representable range (full-scale +1.0 no longer overflows) and sanitize NaN/±Inf samples before quantization.
- The image-source broadband fallback collapses per-band reflection by RMS (energy-correct) instead of the arithmetic mean.
- The FDN and velvet reverbs apply DC blocking per sample inside the loop, and the stereo delay smooths feedback / dry-wet / delay-time changes so automation jumps no longer click.
- The offline realtime voice change compensates the chain's processing latency (retune grain + limiter lookahead), so the result aligns with the input instead of carrying a silent head and truncated tail.
- The asymmetric waveshaper no longer silently bypasses ADAA1 anti-aliasing.

### Bug fixes

- Python FFI: heap-string out-pointers were declared `c_void_p` instead of `c_char_p` across the mastering / mixing / effects signature tables, corrupting returned strings on some platforms.
- `TruePeakLimiter` re-prepare with a different lookahead or oversample factor no longer keeps stale delay-line lengths; scalar-only config changes (ceiling / release) skip re-prepare to avoid mid-stream artifacts.
- `ParallelComp`'s output stage uses a release-smoothed per-channel envelope instead of a hard clip, and its limiter state is initialized on prepare/reset.
- `Tape` pre-allocates state in `prepare()` and rejects excess channels instead of silently growing.
- Transport loop wrapping uses modulo arithmetic, so a single `advance()` can no longer overshoot past the loop region.
- `StreamingEqualizer.match` on Node and WASM defaults to the construction sample rate instead of a hardcoded 48 kHz.
- `RoomReverb` suppresses its default IR synthesis after an explicit RIR is loaded.
- WASM progress callbacks guard against a null stage string; the Node offline-graph bounce read result fields after freeing them; the C-ABI stage-array copy leaked already-copied strings on allocation failure.
- `estimate_room` clamps the octave-band count with a diagnostic instead of silently truncating; `rms_energy` handles zero-length input; the engine no longer leaks parameter strings on re-add.
- The mastering chain passes its configured true-peak oversample factor into the final true-peak measurement; the one-shot named-processor path rejects out-of-range repair modes instead of passing audio through unchanged.
- Audio files open via wide-char paths on Windows (UTF-8 → wchar_t).

### Behavior & default changes

- `Audio.from_buffer` / `fromBuffer` default sample rate corrected from 22050 to 48000 on Python, Node and WASM, and Node `analyzeMelody` defaults `frameSize` to 256, matching the documented defaults.
- `StreamConfig.compute_magnitude` defaults to off everywhere (no streaming read path surfaces the magnitude buffer; the flat C ABI already rejected it).
- Dynamic-range percentile defaults use a negative sentinel on every surface, so 0 is a real 0th-percentile request.
- Mixer scene insert JSON keys are camelCase (`processor`, `params`, `sidechainKey`); the legacy snake_case names are still accepted on read.

### Bindings & API consistency

- Python raises `SonareError` (a `RuntimeError` subclass with a `.code` attribute) instead of plain `RuntimeError`, re-exported from the top-level package.
- Cross-binding alignment: unknown built-in-synth waveform names throw on WASM (and `"sawtooth"` aliases `"saw"` everywhere); an explicitly empty instrument array renders silence on every surface; `setPan` keeps the current pan mode when none is given; `stripMeter` accepts a tap argument on Node and WASM; `set_program` defaults the bank to "no Bank Select" everywhere; `add_clip` passes gain 0 through verbatim and rejects negative/non-finite gain; WASM one-shot `mixStereo` routes through the real mixer graph and gains the missing metering/decompose helpers.
- Node accepts string names for fade curves, loop modes and automation curves alongside ordinals (`'equalPower'` / `'equal-power'` / `'equal_power'` aliases included); `TransportState` adds `playing` as the canonical field (`isPlaying` deprecated).
- The built-in-synth patch type resolves as `BuiltinSynthConfig` on every surface (the old per-binding names remain).
- Inverse-transform entry points and the streaming mastering chain reject non-finite input with an explicit error on every surface; the C ABI clears its last-error message on entry and documents the contract.
- `StreamAnalyzer` exposes `delete()` as the canonical release method on WASM (`dispose()` stays as an alias); Node mastering pair functions accept independent source/reference lengths; the Node `decompose` facade exposes the `init` initialiser.
- The Node and WASM hand-written UMP MIDI-1.0 packers are pinned to golden vectors matching the native packer.

## v1.2.3 (2026-06-02)

### New features

- Added a geometric room-acoustics module (built with `BUILD_ACOUSTIC_SIM`):
  - `synthesize_rir` synthesizes a mono room impulse response from shoebox
    geometry, combining image-source early reflections with a deterministic,
    seeded late tail. Invalid geometry is reported via a diagnostics flag rather
    than an error.
  - `estimate_room` performs blind equivalent-room estimation from a recording
    or impulse response, returning volume, representative dimensions,
    direct-to-reverberant ratio, per-octave-band absorption/RT60, and an honest
    confidence score.
  - `room_morph` applies an offline room-character morph toward a target room
    (a creative effect, not dereverberation).
  - Streaming `RoomReverb` and `RoomMorphProcessor` engines are reachable
    through the generic insert API by name (`effects.reverb.room`,
    `effects.acoustic.roomMorph`).
- Exposed the new module across the C ABI, Python, Node, and WASM bindings.

### Concurrency & real-time safety

- `ClipPlayer::clip_count()` now reads a published atomic instead of calling the
  audio-thread-only `RtPublisher::acquire()`, removing a data race when a host
  polls clip count (via the C ABI / WASM) during playback.

### DSP & analysis correctness

- Room impulse-response synthesis now measures the early-reflection level over a
  window that excludes the direct sound, so the late tail is no longer
  over-scaled in small rooms; per-band late-tail noise is energy-normalized so
  the tail's spectral balance is set by the materials, not the filter bandwidth.
- `estimate_tuning` now thresholds piptrack peaks against a single global median
  (matching librosa); `pitch_tuning` returns the librosa bin left edge.
- `onset_strength` defaults to `detrend=false` and `tempogram` normalizes each
  column by its max (L-infinity), both matching librosa defaults. The internal
  beat/tempo/music analyzers opt into detrend explicitly, preserving behavior.
- Mel `delta` uses Savitzky-Golay `mode='interp'` at the frame edges; chord
  per-frame confidence is computed against the smoothed chroma used for the
  decision; BPM peak picking covers the full tempo range and no longer throws on
  a single-frame onset envelope; 6/8 syncopation no longer counts the secondary
  strong beat.

### Bug fixes

- `declip` now honors `lpc_blend`, blending the LPC estimate with the
  interpolation fallback instead of ignoring the parameter.
- Stereo dither / output-chain now uses a decorrelated per-channel seed instead
  of identical noise on both channels.
- Multiband processors built through the named/insert API now accept a custom
  number of crossover cutoffs instead of throwing.
- Time-stretch / pitch-shift honor `n_fft` / `hop_length` on the default
  spectral backend.
- Streaming analyzer construction clamps `magnitude_downsample` / `hop_length`
  to safe values, preventing a divide-by-zero from direct Node/WASM use.
- `mfcc_to_mel` can invert MFCC liftering when the lifter is supplied.

### Bindings & API consistency

- RIR synthesis exposes `late_model` (Sabine/Eyring), `mixing_time_ms`, and
  `crossfade_ms` across the C ABI and all bindings; the room estimator forwards
  its full acoustic config. Node and WASM acoustic entry points now validate
  sample rate and input like the C ABI / Python.
- The CLI gained `--max-seconds` (synthesize-rir, room-morph) and
  `--n-octave-bands` (estimate-room).
- The absolute-threshold trim is renamed `trim_absolute` to disambiguate it from
  the librosa-compatible relative-to-peak `trim`.

## v1.2.2 (2026-06-02)

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
- TD-PSOLA now preserves duration: the output-epoch-driven synthesis loop maps
  each grain to the nearest analysis pitch mark, so a constant pitch shift no
  longer time-compresses sustained voiced regions.
- Fixed mono fold-down for FDN reverb, velvet reverb, chorus, and flanger, which
  previously wrote two wet signals to the same aliased output buffer.
- The true-peak meter uses the history-preserving (RT-safe) upsample path,
  fixing block-size-dependent inter-sample peak misses.
- HPSS soft masking applies the margin before the power (`margin^power`) to match
  the reference, and `hybrid_cqt` rescales the pseudo-CQT half to the full-CQT
  amplitude convention, removing the magnitude step at the split bin.
- VQT (`gamma>0`) builds the analytic sinusoid with the same `+sin` convention as
  CQT/reference, so its complex phase is no longer conjugated.
- Restored the `a==b => hash(a)==hash(b)` invariant for the chroma/CQT/VQT kernel
  caches (strict float equality with quantized keys), ending silent cache misses
  and rare wrong hits.
- Corrected the KeyAnalyzer profile normalization no-op, slash-chord bass
  detection, `iirt` frame-count off-by-one, and the metronome click step
  discontinuity (now fades in and decays to zero).
- GraphicEq clamps band centers below Nyquist so high bands no longer throw at
  low sample rates; stereo width uses the standard M/S law so widening no longer
  attenuates the center/mono component.
- `ChordChange` records the completed chord's own held confidence; streaming
  `compute_onset` now coerces `compute_mel` so BPM is no longer silently zero;
  short-term LUFS uses the spec 100 ms hop.
- Mastering tape/exciter color stages engage only when they would actually color
  the signal (explicit `enabled` wins; otherwise drive/saturation/amount above
  zero), instead of running at zero strength whenever merely mentioned.
- Hardened degenerate inputs: DynamicsAnalyzer floors the loudness window/hop to
  >=1 sample, the phase-vocoder helper rejects `n_bins<2`/zero hop/zero rate,
  `BoundaryList::clear()` resets the overflow flag, and the C-API
  `spectral_flatness`/`zero_crossing_rate`/`onset_strength` zero their
  out-parameters on the error path.
- `detect_key` now stable-sorts key candidates so silent/tonally-empty input
  deterministically yields the documented C-major fallback on every platform
  instead of a libstdc++/libc++-dependent winner.

### Real-time safety

- Fixed RT thread-safety across the engine, graph, mixing, transport, and
  automation modules; capped insert vectors and documented the `AutomationLane`
  SPSC contract.
- Tape oversampling and AdaptiveRelease no longer allocate on the audio thread
  (preallocated scratch; in-place release update), and
  `RealtimeEngine::bind_mixing_strip` is no longer `noexcept` since it allocates
  on the control thread.
- `monitor_runtime` size is now atomic with acquire/release ordering;
  `send_automation` returns `OUT_OF_MEMORY`/`INVALID_PARAMETER` consistently and
  `validate_stereo_pair` validates both channels.

### Performance

- Replaced the O(N) LRU promotion with an O(1) splice in the mel/chroma filter
  caches and optimised additional hot paths while hardening API boundaries.
- Streaming onset and full-chroma histories use a sliding-window deque (O(1)
  trim, bounded memory on long sessions), the graph plugin-delay-compensation
  pass is O(V+E), the DCT reuses its cached matrix, and `spectrum` `to_db` uses
  the single-allocation overload.

### Bindings & API

- Added imperative `Mixer` strip setters and planar-stereo voice processing,
  hand-written offline effects/dynamics bindings for Node and Python, offline
  dynamics TypeScript typings for WASM, and backfilled Python `.pyi` stubs for
  runtime-exposed analyzer functions.
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
- Surfaced voice-character preset accessors (`voice_character_preset_id`,
  `realtime_voice_changer_preset_config`) across Python, Node, and WASM, with a
  consistent `preset` parameter name.
- Wired previously ignored mastering chain parameters through the named-processor
  and JSON paths (`repair.declip` `lpcBlend`, `multiband.*` per-band params,
  compressor detector/sidechain-HPF/PDR), and round-tripped the realtime
  voice-changer ISP limiter enable flag and dBTP ceiling through JSON presets.
- Hardened binding inputs: WASM `remix` reads interval boundaries as exact
  integer sample indices (no float truncation of large indices), Node
  `scaleQuantizeMidi`/`scaleCorrectionSemitones` reject a `modeMask` outside
  `[0, 4095]`, and Node time-stretch requires an explicit numeric `sampleRate`.
- Preserved mixer pan mode when serialising scenes after `sonare_strip_set_pan`
  and removed a per-call allocation from latest goniometer reads.

### Tooling & internal

- Added a cross-binding parity checker (`tools/parity`) that detects default,
  constant/enum, and parameter-name drift between the C++ core and bindings, and
  a realtime voice-changer quality gate in CI.
- Split the monolithic `sonare_c.h` and `sonare_c_daw.cpp` into per-domain
  units, folded offline-analysis boilerplate into a `run_offline` helper, and
  commonised biquad state, `db_to_linear`, and pass/gain processors into `rt/`.
- Added thread-safety contracts to the RT/mixing/engine Doxygen headers.
- Extracted the four mel/chroma/CQT/VQT cache copies into a single
  `util/lru_cache.h` template, and centralised every mastering processor's
  parameter list into shared X-macro field tables driving both the chain JSON
  serializer and parser (one definition site per parameter).
- Deduplicated next-power-of-two callers and the `copy_audio_result` / C-API stub
  helpers, and adjusted parity normalisation so digit runs in names such as
  `ebur128` match C naming.
- Generate the gitignored K-weighting reference fixture before `ctest` in CI and
  hardened the Compressor concurrency test against runner scheduling jitter.

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
