# Changelog

## v1.5.4 (2026-07-22)

This is a follow-up release to v1.5.3, adding a musical-beat playhead and configurable undo history to the realtime and project surfaces, a physically motivated air-absorption term for large-hall reverberation, and a round of realtime-safety, voice-changer and CLI/binding correctness fixes.

### New surfaces

- The engine transport snapshot now reports the musical `beat` (one-based) and `beat_fraction` (in `[0, 1)`) alongside the existing bar index, so hosts can render a bar:beat:tick playhead. The C `SonareTransportState` grows the two fields (appended after the time signature to preserve existing offsets) and the Node, Python and WASM readers plus their type declarations surface them.
- The project edit history exposes a configurable undo depth and an explicit history reset across every surface: `sonare_project_set_max_undo_depth` / `sonare_project_clear_history` on the C ABI, mirrored as `setMaxUndoDepth` / `clearHistory` (Node, WASM) and `set_max_undo_depth` / `clear_history` (Python). Shrinking the depth evicts the oldest entries immediately (clamped to at least one), letting callers trade undo history for resident memory or reset it between sessions.

### Acoustic model

- Reverberation time gains an optional atmospheric-absorption term: an ISO 9613-1 pure-tone air-absorption coefficient feeds the `4mV` denominator of the Sabine/Eyring `shoebox_reverb_time`, shortening the high bands of large halls the most to match the physical air roll-off. The parameter is opt-in and defaults off, so the geometry-only reverberation tail is byte-identical.
- Early reflections are coloured per octave band and the polyhedral image-source search is bounded, so non-shoebox rooms render more accurately without unbounded reflection enumeration.

### Bug fixes

- Realtime audio output is hardened against non-finite state and torn reads, and hot-path scratch buffers are reused to remove a realtime allocation on the render path.
- The voice changer applies a flat configuration POD through `setConfig` on Node and WASM, accepts a narrower interleaved channel count on WASM, folds `retune.mix` into its reported realtime latency, and matches the C-ABI error contract on the WASM path.
- Section analysis runs on the shared analysis-rate signal, and a handful of DSP / analysis parity and edge-case bugs are corrected in the core.
- The realtime engine seeds its fallback tempo map, and the embedded-scene version mismatch is classified as an explicit error.
- Arrangement editing restores split-MIDI order exactly and skips redundant store clones on undo; the composition assistant budgets its iterations by consumed count rather than slot count.
- Direct-call bindings require an explicit sample rate and validate WASM inputs, and the Python CLI corrects its output handling and surfaces project diagnostics.
- Streaming computes the per-frame smoothed chord once per frame.

This release is a cross-surface input-validation and realtime-safety hardening pass over the offline, streaming, CLI and macOS-host paths, rounded out by a request-object call form for the one-shot JS facades and a handful of additive analysis, engine and mastering surfaces.

### New surfaces

- The top-level one-shot analysis, effects, mastering, metering, feature and mixer/voice-changer functions accept a request object as their canonical call form, so each input is named and optional settings can grow without disturbing argument order. Positional signatures remain as compatibility overloads and normalize through the same path, keeping defaults, validation, errors, results and progress behaviour identical between both forms. Mirrored on Node and WASM with matching field names and defaults (the WASM entry point also re-exports the request-object types); the embind and N-API calls underneath stay positional, and Python keeps its idiomatic keyword arguments.
- Every mastering result now reports the chain output true peak (dBTP, at the chain's configured oversample factor), the output loudness range (LRA) and per-stage gain reductions, surfaced as a `StageGainReduction` type on Node, Python and WASM so callers can confirm a preset ceiling was met without a second oversampled scan.
- The realtime engine gained `sonare_engine_set_tempo_segments` / `sonare_engine_set_time_signature_segments` (Node `setTempoSegments` / `setTimeSignatureSegments`, Python `set_tempo_segments` / `set_time_signature_segments`, and the WASM equivalents), letting callers install a piecewise tempo / time-signature map instead of a single value; an empty list clears the map back to the single value.
- Streaming frame results expose `feature_flags` and `n_chroma` so consumers can tell which arrays are physically present; disabled features emit empty arrays with zero strides instead of implied full widths, across the C ABI, Node, Python and WASM.

### Hardening and bug fixes

- Public audio input is validated and bounded uniformly across the C ABI, Node and WASM direct-call paths — finite, non-empty samples within the supported sample-rate and size limits — so an invalid call fails the same way on every surface instead of copying bad data into the core. The mastering, metering, room and voice-changer configs, the realtime tempo / marker / parameter input, and CLI arguments and imports are validated on the same footing.
- Realtime-thread safety is tightened across the synth, engine and acoustic paths, and the macOS device and plugin-host backends are hardened against RT-thread and lifecycle hazards, including per-slot SysEx cursor resets and non-finite AU-output scrubbing.
- Offline resource use is bounded against resource-exhausting and degenerate inputs; RIR length is capped while early reflections past the tail are preserved; every GM program-override patch is bounded; and MIDI 2.0 pitch bend is center-scaled correctly on up-conversion.
- Silent-failure paths in serialization, MIDI, mastering and decode are replaced with explicit errors, and audio and project files are written atomically.
- The CLI propagates invalid global option values into its exit-code mapping, plumbs global `fmin` / `fmax` and warns on ignored flags, keeps its JSON output valid, and writes artifacts atomically; the Python CLI corrects option inheritance and neutral voice-changer defaults.
- Streaming flushes the final held chord and reports zero mel bands when mel is disabled; the WASM `masterAudio` path flattens nested overrides and wires progress callbacks while the Node mastering facade also accepts the legacy flat override spellings; and the bare saw, square and triangle synth presets are restored.
- Cross-surface validation parity is tightened further: the Node and WASM acoustic facades now reject out-of-range or non-finite per-band absorption / scattering coefficients instead of silently clamping them (matching the C ABI), the WASM realtime tempo / time-signature / marker setters bound their list length, and the scale quantizer rejects non-finite MIDI input.
- Reported mastering metrics are corrected: the stereo chain loudness range (LRA) is measured with BS.1770 channel summing rather than a phase-cancelling mono downmix, and the reported true-peak oversample is coerced to a supported factor so a disabled-loudness chain no longer fails on an odd configured value.
- The room impulse response treats `max_seconds` as an upper bound rather than an exact length, so a naturally short response is no longer zero-padded out to the cap; the clamp diagnostic now also fires when the cap truncates the early reflections.
- Resource use on hostile input is bounded: project deserialization deduplicates markers in linear time, guards its base64 size estimate against unsigned underflow, and caps decode-path diagnostics with a suppression summary, the undo/redo history is bounded to a maximum depth, and files are written through a per-writer temporary path so concurrent writers to one destination stay valid.
- Realtime-engine tempo handling is corrected: clearing a piecewise tempo / time-signature map with an empty list reverts to the last single value set via `set_tempo` / `set_time_signature` (as documented) rather than a hardcoded default, and the C-ABI marker snapshot is published before its backing string storage is replaced.
- CLI behaviour is aligned across the native and Python surfaces: a repeated `--set` applies every assignment, `--flag=false` disables a boolean flag, an output destination given to a pure-analysis command is rejected rather than silently discarded, audio-rendering effect commands require an output file on both surfaces, `estimate-room` accepts both band-count flag spellings, the `version --json` `cli_version` tracks the build, a missing `project` subcommand exits with the usage code, and an oversized project import is rejected before allocation.
- The macOS CoreMIDI host backend reassembles multi-packet SysEx off the realtime callback (handing completed payloads to the control thread) instead of mutating a shared store from the callback, the Audio Unit output-scrub path is shared between the instrument and effect roles and reports dropped instrument events, and the CoreMIDI host build is fixed.

### Behavioural changes

- The Node and WASM acoustic room APIs now reject a per-band absorption or scattering coefficient outside `[0, 1]` (or non-finite) with an invalid-parameter error instead of clamping it, matching the C ABI and every other surface. Callers that relied on out-of-range values being silently clamped must pass in-range coefficients.
- Pure-analysis CLI commands reject an `-o` / `--output` destination (they print to stdout), and audio-rendering effect commands now require one on both the native and Python CLIs. Scripts that passed `-o` to an analysis command, or omitted it from an effect command on the Python CLI, will now receive a parameter error.

## v1.5.2 (2026-07-16)

This release adds a spectral-reconstruction path and a handful of additive analysis, project and streaming surfaces, and continues the v1.5.1 hardening pass across the mastering, mixing, MIDI-import and realtime-thread paths.

### New surfaces

- `sonare_griffinlim_cqt` / `sonare_griffinlim_vqt` reconstruct a time-domain signal from a constant-Q or variable-Q magnitude spectrogram via Griffin-Lim, callable on the Node, Python, WASM and C-ABI surfaces.
- The chord analyzer now reports an explicit no-chord (N.C.) interval whenever the frame correlation falls below the detection threshold, surfaced on every binding instead of silently dropping the segment.
- Compound clip edits go through the C-ABI project surface as a single undo transaction, so a multi-clip operation is undone or redone in one step across Node, Python and WASM.
- The mastering processor catalog reports each insert's decay-tail length alongside its latency, wired into the typed Node, Python and WASM surfaces.
- The Node realtime voice changer gained an explicit `destroy()` so its native resources can be released deterministically rather than on GC.
- The WASM entry point re-exports the `ExternalMidiEvent` type.
- Insert-automation scheduling failures are now classified through the mixing C ABI instead of returning a single opaque error.

### Hardening and bug fixes

- Streaming analysis bounds its chord-progression history and enforces contiguous frame offsets, rejecting out-of-order or gapped input.
- Lane sidechain rebinding, live mixer parameters and realtime insert channel state are made safe against concurrent audio-thread processing, and realtime seqlock snapshots are stored in lock-free atomic words.
- The mastering path validates named-processor configs before applying them and rejects non-finite loudness-optimize targets; the mixing path reports the longest audible tail.
- Offline pitch-shift expansion is bounded by the shared resource limits.
- MIDI import enforces SoundFont resource limits and rejects malformed records, overflowing SMF ticks and mismatched export track counts.
- The core rejects non-finite and oversized time / pitch / VQT / chord inputs through new overflow-safe size, addition and projection helpers; project serialization rejects out-of-range enum and integer fields; and the engine bounds the public PPQ and guards timeline and MIDI-FX overflow.

## v1.5.1 (2026-07-14)

This is a stabilization release for the v1.5.0 instrument and engine work: it hardens input validation across every surface, tightens realtime-thread safety, and fixes a set of mastering, warp and MIDI-import edge cases. A few small additive surfaces round out cross-binding parity.

### Input validation and resource bounds

- Every C-ABI entry point now clears the thread-local error before it runs and rejects non-finite, out-of-range or oversized numeric and resource inputs instead of proceeding on bad data, with the DSP, serialize, mastering and metering paths validated through shared finite/range and clip-page bounds helpers.
- The JS facades (Node and WASM) were aligned with the Python surface on argument validation, object-key handling and index bounds, so an invalid call fails the same way on every binding rather than reaching the core.
- Paged-clip provider dimensions are bounded across the C ABI and WASM, power-of-two / slice / indexing math guards against numeric overflow, room sizes that would overflow are rejected while early-reflection energy is preserved, the early-reflection IR length is capped, the final dither type enum is range-checked, and clip fades are clamped to the clip length.
- Project deserialization and the serializer validate their numeric and resource inputs, and the Project facade exposes its clip count.

### Realtime-safety hardening

- Live graph and MIDI configuration changes are now adopted through RT-safe immutable snapshots, and realtime instrument rebind and mixing toggles are hardened against audio-thread races.
- Control-thread mixer parameter resolution no longer races the audio-lane state, live SysEx payload slots are published as a seqlock, and a live GS insertion-effect SysEx is realized only after its command has enqueued.
- Offline render of a never-prepared engine now fails closed instead of touching unreserved telemetry state.

### Mastering, warp and DSP fixes

- The true-peak limiter preallocates its per-channel state, widens its oversample counts and scales its release to the oversampled rate; the mono-compatibility check band-projects the side signal in its log-band comparison; and the MultibandImager enumerates a descriptor for every band.
- The multichannel phase-vocoder re-locks its bins so the stretched tail matches the mono result, the stereo-delay ping-pong coefficient is smoothed, the onset frame offset is corrected at large hop sizes, and the pitch-tuning histogram bin count uses `ceil`.
- The voice changer reports its realtime latency as the wet-mix-weighted delay, the stem bounce keeps its tails and rejects unsupported channel counts, and the brass low-register tuning is corrected for the in-loop DC blocker.

### MIDI import resilience

- The SMF parser resynchronizes after a variable-length quantity overruns a non-final track, and a single corrupt SMF track no longer fails the whole import.

### Cross-surface consistency and new exposure

- `voiceChangeRealtime` (WASM) is positional like the Node and Python surfaces, `timeStretch` / `pitchShift` argument order (Node) matches the C ABI, `crossfadeMs` 0 (Node) is treated as the default, and the WASM surface threads the `tempogramRatio` factors through and exposes the aggregate `abiVersion`.
- The streaming analyzer bounds its unread output with drop-oldest backpressure via a new `maxUnreadFrames` limit, the mixer `latencySamples` value is wired to every binding, the streaming mastering chain stage names are exposed through the C ABI, and Python gains `RealtimeEngine.set_midi_fx` to match the other surfaces.
- The Python CLI uses the anti-aliased resampler, reports a C bass note correctly, and maps RIR geometry and the `synthesize-rir` legacy override to the right exit codes.

## v1.5.0 (2026-07-06)

### Physically-modeled instrument voices

The built-in synthesizer gained a family of physical-modeling engines that replace the previous subtractive/FM sketches for many General MIDI programs. Each is exposed through `SonareSynthEngineMode` and wired across the Node, Python, WASM and C-ABI surfaces in lockstep:

- A sustained digital-waveguide flue **pipe organ** (`SONARE_SYNTH_ENGINE_PIPE_ORGAN`) with multi-rank stop registration under a single key, lingual reed pipes, a self-oscillating cubic jet, a shared wind chest (tremulant / wind sag) and mouth-radiation brightening; GM Church Organ voices on it.
- A **bowed-string** engine (`SONARE_SYNTH_ENGINE_BOWED_STRING`) modeling Helmholtz stick-slip friction, with violin / viola / cello / contrabass presets, live bow control (CC11 expression, CC2 breath, CC74 brightness), a fourteen-mode measured violin body resonator, and off-by-default elasto-plastic bow friction, sympathetic resonance and a second vibration plane.
- A **reed woodwind** engine (`SONARE_SYNTH_ENGINE_REED`) with cylindrical/conical bore selection, clarinet / saxophone / oboe / english-horn / bassoon presets, and off-by-default cone-growth and tonehole-scattering registers (a cylinder overblows to its twelfth, a cone to its octave).
- A lip-reed **brass** engine (`SONARE_SYNTH_ENGINE_BRASS`) with a fixed-formant `SONARE_SYNTH_BODY_BRASS_BELL` radiation body, eight brass presets, live breath/brightness control, and off-by-default cuivré, mute, half-valve and two-mode lip gates.
- An air-jet **flute** engine (`SONARE_SYNTH_ENGINE_FLUTE`) with concert-flute through piccolo / recorder / shakuhachi / ocarina presets and live breath/brightness control.

The synthesizer also gained three further engines — **plucked-string** (`SONARE_SYNTH_ENGINE_PLUCKED_STRING`, 13) for the buzzing-bridge harp/koto/sitar family, **vocal** (`SONARE_SYNTH_ENGINE_VOCAL`, 14) for a source-filter choir/voice with selectable vowels through a new `SONARE_SYNTH_BODY_VOCAL` body, and **free-reed** (`SONARE_SYNTH_ENGINE_FREE_REED`, 15) for the accordion/harmonica/bandoneon/reed-organ family. Each has named presets, is routed as a GM fallback voice (GM 20-23 free-reed, 52-54 vocal, 104/106/107 plucked) and is exposed on all four surfaces. These ship with an initial voicing that will be refined in a later release.

- The **piano** voice gained per-note stiff-string dispersion derived from its inharmonicity coefficient, register-graded unison string counts, a Railsback stretch-tuning curve, a shared instrument-wide modal soundboard, pedal-gated sympathetic resonance, velocity-dependent hammer dynamics, and a full three-pedal set — continuous half-pedal sustain (CC64), sostenuto (CC66) and una corda (CC67).
- The **percussion** voice gained strike-point weighting, shell resonance, snare-wire rattle, nonlinear cymbal shimmer and stochastic-particle (shaker/scraper) excitation, and every GM/GS drum key 27-87 now resolves to a distinct membrane / wood / metal / whistle archetype with kit-variation selection and mute-group choking.
- Many GM programs now route to these physical cores instead of the FM/subtractive fallback — the plucked-string, guitar and bass families (Karplus-Strong with bridge coupling, steel dispersion, a physical pluck and a shared sympathetic-string bank), bowed strings, brass, reed woodwinds, flutes, and chromatic/pitched percussion — with per-program ambience sends and per-voice pitch drift and stereo spread. The SF2-less GS fallback path shares the same body, wind-chest and pedal components so those patches sound consistent there.

### GS insertion effects and live SysEx

- General MIDI / GS insertion effects are now realised as real insert chains. The type-to-insert map covers single, composite and multi-effect types — graphic and shelving EQ, enhancer, the chorus family (hexa / space-D / 3D), the delays, plate and gate reverb, overdrive/distortion, rotary, pitch shifter, lo-fi and the guitar/keyboard multi-effect chains — expanding composite types into their manual signal-order stages with parameter translation from the effect data. Types with no faithful stock insert stay bypassed.
- Live MIDI SysEx delivery landed: `sonare_engine_push_midi_sysex` (`pushMidiSysex` / `push_midi_sysex`) queues a variable-length SysEx frame to a bound instrument while playing, wired across Node, Python and WASM. A pushed GS insertion-effect SysEx installs and swaps its insert chain wait-free on the audio thread, so insertion effects are audible live as well as through the offline bounce.

### Amp-sim and modulation inserts

- The mastering amp-sim insert gained selectable amp voicings (classic-crunch / fender-clean / modern-hi-gain plus tweed / Vox-chime / rectifier), guitar 4x12 and bass 8x10 cabinet models, a push-pull power-amp stage with supply sag and output-transformer saturation, and a global negative-feedback path — each off by default, bit-identical when disabled, and exposed as automatable insert parameters.
- Added modulation insert effects — wah, auto-wah, rotary, ring modulator and pitch shifter — which report realtime-safe parameter updates and back the corresponding GS insertion-effect types.
- The mastering processor catalog now reports a per-insert `latencySamples`, probed at a representative 48 kHz / 512-sample configuration (offline and non-insertable processors report 0).

### Constant-Q chroma and MFCC liftering

- Added the constant-Q chromagram `sonare_chroma_cqt` (mirroring `sonare_chroma_cens`) so `chroma_cqt` is callable on the Node, Python, WASM and C-ABI surfaces.
- `sonare_mfcc_ex` gained a trailing cepstral `lifter` coefficient (default 0, the library default), reachable on all four surfaces; the inverse path assumes an unliftered input.

### Surround metering and bus mixing

- `SonareMixMeterSnapshot` now carries per-plane peak / RMS / true-peak arrays plus a channel count for up to eight surround planes, so 5.1 / 7.1 center, LFE and surround meters reach the host; indices 0/1 still mirror the existing stereo fields. Fanned out through the Node, Python and WASM marshalers and type stubs.
- A bus can shape its summed output with an input trim, stereo width and per-channel polarity invert, and `sonare_engine_set_bus_strip_insert_bypassed` bypasses a bus-strip insert. Both are wired across every surface, and the three bus-shaping fields round-trip through project and scene JSON.

### Insert-parameter and external-MIDI automation

- Track, master and bus strip inserts can be automated in realtime: `sonare_engine_resolve_track_insert_automation_id` / `_resolve_master_insert_automation_id` / `_resolve_bus_insert_automation_id` resolve a strip/insert/parameter triple to a reserved automation id drivable through the existing automation lanes, `sonare_engine_set_bus_strip_insert_param_by_name` sets a bus insert parameter by name, and `sonare_engine_set_param_smoothing_ms` tunes the engine-wide glide time (previously fixed at 20 ms). Exposed on Node, Python and WASM.
- An external-MIDI output queue routes a track to external gear: `sonare_engine_set_midi_destination_external`, `_set_external_midi_clock_enabled`, `_external_midi_dropped_count` and `_drain_external_midi` — draining `SonareExternalMidiEvent` records with shared MIDI-2-to-MIDI-1 lowering — are available on every surface, with a full destination table reporting overflow instead of silently rerouting to the internal rack.

### Scale-aware pitch correction

- Added `sonare_pitch_correct_timevarying` and a `SonarePitchCorrectionConfig` POD that generalize the fixed-MIDI corrector: a caller-supplied F0 contour can snap to a musical scale with tunable retune strength, correction clamp, glide and vibrato threshold. Wired across Node and WASM (a `PitchCorrectOptions` bag) and Python (keyword args). The Python CLI also gained pitch-shift, time-stretch, normalize, trim-silence and resample subcommands.

### Bounded-memory clip streaming

- Added `ClipPageStreamer` and the one-call `attachOpfsClipStream`, a sliding-window manager that keeps OPFS-paged clips fed within a bounded window around the playback frontier (prefetch ahead, evict behind), so a long multitrack arrangement never holds its full PCM in WASM memory. The AudioWorklet now runs the single full-featured embind engine.

### Native host backends (macOS)

- The experimental macOS host backends gained CoreAudio xrun telemetry (`xrun_count()`), per-render Audio Unit output channel renegotiation with cached AU instances for parameter enumeration, and CoreMIDI SysEx output that expands a resolved SysEx payload into SysEx7 UMP packets at flush. These stay macOS-only, source-build opt-in and add no C-ABI surface.
- The unused multichannel audio-loading path (`load_audio_multichannel` / `AudioLoadResultMC`), added in v1.4.0 but never wired to a caller or a published surface, was removed.

### Bug fixes

- librosa feature parity: `chroma_cens` uses librosa's symmetric-Hann smoothing window and zero-padded edges; the STFT chroma filterbank is a direct port of the librosa chroma filter; `chroma_cqt` centers its CQT-bin-to-pitch-class fold at coarse resolutions; `spectral_flatness` reports the maximally-flat value on a silent frame; spectral-contrast quantile rounding matches the librosa float64 result; mel `fmax` is clamped to Nyquist instead of erroring; and the `peak_pick` local-max/average windows follow librosa's exclusive slice bounds.
- Metering: silent peak/RMS report the finite dB floor instead of `-inf`, so the level fields stay JSON-safe.
- Realtime engine: clips dropped by a live mute or delete release their hung notes; the capture punch state is read without spinning on the audio thread; MIDI-learn assembles 14-bit / RPN / NRPN controllers under a movement gate; live track-pan automation honors the strip's pan law; external-MIDI records carry the monotonic device render frame and drain uniformly across surfaces without loss; insert-automation slot-table overflow is surfaced on telemetry; track strips update in place to avoid fader/pan clicks; the stereo-width and bus input-trim smoothers settle for a deterministic offline pre-roll; and cached filterbanks are pinned behind shared handles to prevent a concurrent-eviction use-after-free.
- Mastering & effects: the reverb and stereo-delay processors report their decay tails so an offline bounce no longer truncates them; reverb parameter updates set before `prepare()` are retained; convolution-reverb decay is clamped to its ceiling at construction; loudness-optimize reports zero (already-aligned) latency; the saturation and spectral inserts preallocate per-channel state so a live insert never allocates on the audio thread; and the core and WASM mastering chains reject empty, non-finite or non-positive-rate input.
- Mixing: a centered mono strip stays at unity under every pan law.
- MIDI & SMF: lossy (non-power-of-two) time-signature denominators are flagged on SMF export; a non-zero first tempo/time-signature segment survives an SMF2 round-trip; and UMP word counts derive from the message type.
- Arrangement & serialization: track-kind changes that would orphan a track's clips and non-finite, negative or non-monotonic warp-map anchors are rejected in the core; bus trim / width / polarity persist across project save/load; and pitch-correction MIDI range is validated on every surface.
- Synth: the church-trumpet preset builds its reed stop on the pipe-organ waveguide again after GM Reed Organ moved to the free-reed core.
- Bindings & CLI: the WASM SysEx push distinguishes `InvalidParameter` and `OutOfMemory` rejection classes; the Python package type stub re-exports its documented public names; the Python mix command resamples each input to the mixer rate; and the WASM module builds before the JS bundle so a plain build is never a step behind.

## v1.4.1 (2026-06-28)

### Looping, mastering & long-form analysis

- `sonare_project_set_clip_loop` gained a `loop_crossfade_ppq` argument: an equal-power crossfade at the loop seam that blends the loop tail with the pre-roll source material (clamped to the available clip offset and half the loop, disabled under warp). It is serialized only when non-zero, so existing projects round-trip unchanged. Added across the Node, Python, WASM and C-ABI surfaces in lockstep.
- `SonareMasteringConfig` gained `release_ms` (0 keeps the 50 ms library default) and `apply_gain_at_input_rate`; zero-initialized callers keep their previous behaviour. Propagated through the mastering helpers and all four surfaces.
- `BoundaryDetector` now accepts long-form input that exceeds the self-similarity int-index cap (~46340 frames) by mean-pooling features to at most 8192 frames and re-normalizing, instead of throwing `InvalidParameter`. Boundary times stay accurate; the `frame` field indexes the pooled grid for long inputs, so callers should map positions via the `time` field.

### Bug fixes

- Mono live monitoring now matches the mono bounce downmix: a panned clip A/B'd between the live monitor and the bounce agrees in level and balance, and a centered clip stays at unity.
- WASM embind vector and object returns are re-rooted into the calling realm's `Array`/`Object`, so results from `*Names()`, preset and section/key-candidate calls survive `structuredClone` / `postMessage` to a Worker.
- Hardened input validation and integer-overflow guards across surfaces: `MasteringChain` and `StreamAnalyzer` reject empty / out-of-range / non-finite input in the core so every binding inherits the checks; the WASM realtime engine and voice changer now validate `prepare()` / `setTimeSignature()` / `setLoop()` and block-size arguments that the WASM build otherwise bypassed; self-similarity, Viterbi, segment and window builders gained the int-overflow guards already used elsewhere; repitch-warped comp parts with a large source offset play instead of being silenced; and MIDI 2.0 note velocity is humanized in the full 16-bit domain.

## v1.4.0 (2026-06-25)

### macOS host backends (experimental)

- Added experimental native macOS audio/MIDI host backends so a project can drive real hardware without an external host: a CoreAudio output backend, a CoreMIDI input/output backend, and an Audio Unit (AU) instrument host. They are macOS-only, built behind off-by-default `BUILD_COREAUDIO` / `BUILD_COREMIDI` / `BUILD_AU_HOST` options, add no C-ABI surface, and ship in no published package (npm / PyPI / WASM) — a source-build opt-in that may still change. The unwired `BUILD_VST3_HOST` placeholder was removed. When lowering MIDI 2.0 program changes to MIDI 1.0 for the host, the preceding bank-select pair is preserved, and AU MIDI events are sorted by render frame before dispatch.

### Surround & multichannel mixing

- Added surround channel layouts (mono through 7.1), a layout-aware downmix and a surround panner to the mixer, with per-plane meters and surround group-bus rendering in the realtime engine. The track strip gains dual-pan, pan-law and pan-mode controls plus a per-channel delay, exposed via `sonare_engine_set_track_strip_pan` / `set_track_strip_dual_pan` / `set_track_strip_pan_law` / `set_track_strip_pan_mode` / `set_track_strip_channel_delay_samples`. Scene JSON now persists the surround layout, pan and VCA offset so a surround mix round-trips. Wired on Node, Python and WASM.
- The core gained `load_audio_multichannel`, preserving the file's native channel layout instead of folding to mono/stereo; host-only multichannel decoders are guarded on WASM.

### Realtime scope & meter telemetry

- Added a realtime scope-telemetry tap (`sonare_engine_configure_scope_telemetry` / `sonare_engine_drain_scope_telemetry`) and a wide meter-telemetry drain (`sonare_engine_drain_meter_telemetry` / `sonare_engine_drain_meter_telemetry_wide`) so hosts can read per-band scope levels and wide multichannel metering off the engine. Scope band levels are block-size independent, and the meter drain reports a defined floor instead of full-scale/NaN. Surfaced across Node, Python and WASM, with scalar L/R meter telemetry fields on Node.

### Realtime parameter automation

- Effects, mastering and mixer processors now publish JSON-key parameter descriptors that enumerate the parameters a host can automate in realtime, including realtime-automatable insert parameter info (`sonare_mastering_insert_param_info`, `sonare_mastering_processor_catalog`) and insert/pan automation. Reserved mixer parameters are driven directly from automation lanes. Insert and master-strip insert parameters can be set by name (`set_track_strip_insert_param_by_name` / `set_master_strip_insert_param_by_name`). Exposed on every binding.

### Region-based spectral editing

- Added region-based spectral editing (`sonare_spectral_edit`): apply gain/attenuation to a time–frequency region of a signal. Wired with consistent behaviour across the Python, WASM and C-ABI surfaces.

### Track editing & group routing

- Added track-level edit commands — `sonare_project_set_track_gain` / `set_track_mute` / `set_track_solo` / `set_track_pan` / `set_track_midi_destination` (plus `sonare_project_remove_warp_map`) — with non-finite / negative gain and pan rejected at the C-ABI boundary. The track mixer gained group-bus routing and per-lane sidechain keys, exposed on the WASM `SonareEngine` facade as `setTrackOutputBus` / `setLaneSidechain` and on the other bindings. Instrument racks are mixed into their shared buses once per block.

### MIDI & synth

- The built-in synth gained MPE pitch-bend and per-note pressure, honours Reset All Controllers, and the MIDI-FX JSON config parses arpeggiator keys. Live control changes (14-bit / RPN / NRPN) are decoded at full resolution and MIDI 2.0 note velocity is shaped in the full 16-bit domain.

### Analysis

- `chroma_stft` now matches librosa's L-infinity per-frame normalization.

### ABI guards

- Added an ABI version mirror consistency check (`make check-abi-version`) and a ctypes struct-layout guard (`make abi-layout` / `abi-layout-check`) with make targets, so an ABI mirror desync or a ctypes layout drift fails as a red test instead of a runtime segfault. The layout guard also asserts ctypes mirror field types, not just byte layout.

### Deterministic offline bounce

- Offline renders settle (snap) all smoothed gain and effect parameters before rendering (`settle_parameters`), so a bounce is deterministic and independent of the live smoother state at render time.

### Bug fixes

- Engine: warped mid-clip comp parts no longer double-offset the source read; `time_to_frames` saturates instead of casting an out-of-range float to int; lane remap is skipped on an unchanged config in hot mixer commands; block-final automation values are preserved past the per-block event cap; engine markers are staged atomically to avoid a use-after-free on rejection; and the compiled graph topology is invalidated when sidechain ports change.
- MIDI: `MidiFxChain::process` sorts its fixed-capacity output buffer in place (binary insertion sort) instead of via `std::stable_sort`, which requested a temporary heap buffer — restoring zero heap allocation on the audio thread while keeping the same render-frame / off-before-on event ordering.
- Mastering: `dynamics` `set_parameter` is RT-safe via an in-place working config (with a noexcept in-place `release_ms` setter); standalone `loudnessOptimize` honours `releaseMs` and `applyGainAtInputRate`; and mastering name-getter return pointers are stabilized across repeated calls and gated on a write-once flag.
- Mixing: VCA group offset accumulates with an atomic read-modify-write; send timing defaults to post-fader across all surfaces.
- Analysis & util: `OnsetAnalyzer` detects flat-topped onset peaks; DTW/RQA and NNLS guard integer index overflow; the acoustic image-source reflection order is clamped.
- Audio I/O: WAV write rounds float samples to the nearest PCM integer.
- Validation hardening: the Node addon throws on an invalid compressor detector instead of falling back; WASM rejects a voice-changer channel count that differs from the prepared layout, rejects non-positive melody/sections params, aligns detailed-analysis config validation with the C ABI, validates offline audio input through a shared core helper, and validates engine bounce/freeze/lane inputs against the C-ABI oracle; `StreamAnalyzer` rejects malformed config geometry on every surface; and undoing `RemoveMarker` restores all marker fields.

### CI

- Bumped the GitHub Actions workflows to the Node 24 runtime.

## v1.3.3 (2026-06-12)

### SMF meta events & structured markers

- The SMF core now preserves the standard text-class meta events it previously dropped: text (0x01), lyric (0x05), cue point (0x07) and key signature (0x59) import and export round-trip alongside the existing marker (0x06). Each is tagged with a `SmfMarkerKind` (Marker / Text / Lyric / CuePoint / KeySignature); key signatures carry the structured fifths/minor pair plus a human-readable tonic name (e.g. "E minor"). Text and lyric events are collected into the flat, timeline-global marker list — their musical-time position is preserved, but per-track / per-note alignment is not.
- Project markers gained `kind` + key-signature fields end-to-end: the document model, JSON serialization, the SMF import path and a new SMF export of project markers (previously project markers were never written back to SMF). New C ABI: the `SonareMarkerKind` enum, a `SonareProjectMarker` struct, `sonare_project_set_marker_ex` (set a marker with its kind / key signature) and `sonare_project_marker_by_index` (read markers structurally without JSON). `SonareEngineMarker` carries the same fields. Wired on Node, Python and WASM with a `MarkerKind` enum, `ProjectMarker` type, and `setMarkerEx` / `markerByIndex` facades.

### Per-track lane mixer

- Added a realtime-safe per-track lane mixer (`TrackMixerRuntime`) owned by the realtime engine: tracks route through configurable aux sends into numbered buses, and plugin delay compensation is recomputed whenever the lane snapshot is published. New C-ABI surface — `sonare_engine_set_track_lanes` / `set_track_buses`, per-track / master / bus channel-strip JSON (`set_track_strip_json`, `set_master_strip_json`, `set_bus_strip_json`), EQ-band updates (`set_track_strip_eq_band_json`, `set_master_strip_eq_band_json`), insert bypass (`set_track_strip_insert_bypassed`, `set_master_strip_insert_bypassed`), queueable lane solo/mute (`set_solo_mute`), and the `SonareEngineTrackLane` / `SonareEngineTrackSend` / `SonareEngineBus` structs — wired on Node, Python and WASM.
- Added a realtime MIDI clip schedule API (`sonare_engine_set_midi_clips` with `SonareEngineMidiEvent` / `MidiClipSchedule`) and `sonare_engine_sample_at_ppq` for sample-accurate PPQ lookup, exposed on every binding.
- The clip player renders individual lanes in isolation (`process_track_at` / `process_excluding_tracks_at`), and EQ-band JSON parsing is now shared between the mastering EQ and the new strip helpers.

### Engine warp & realtime hardening

- Tempo-sync warp baking is now phase-coherent across channels (`bake_tempo_sync_warp_channels`, peak-locked phase vocoder), used by both the edit compiler and the C engine instead of per-channel baking.
- `RtPublisher` keeps a coalesced pending slot so `publish()` never drops a snapshot when the hand-off ring is full.
- The metronome and punch-capture are gated on the transport actually rolling.
- Realtime-thread hardening: the MIDI input destination id is atomic and snapshotted per block, input-monitor state is consolidated into a single seqlock cell, the C clip-page provider uses raw atomic pointers plus a retired-pages list to prevent use-after-free on supply/clear, and `captured_frames` uses release/acquire ordering.
- Clip editing propagates comp segments and take offsets through split and trim, blocks loop mode when comp segments split a clip, adds a `RestoreClip` undo step, and validates comp segments before scheduling.

### WASM realtime engine

- Added realtime-engine AudioWorklet facade coverage for track lanes, strip and bus scene sync, MIDI clips and live MIDI, instruments, capture read-back, marker loops, transport state, clip delta sync, clip loop/fade/warp typing, and tempo/time-signature segment sync.
- Added `SonareEngine.setMarkers`, a replace-all marker facade: where `addMarker` could only append, `setMarkers` replaces the whole marker set in one call — entries keep explicit positive unique ids or are assigned fresh ones (the id counter advances past explicit ids), the resolved list is returned for host-side id mapping, and the set is delivered to both the offline mirror and the realtime worklet through the existing `syncMarkers` path.
- The WASM build now compiles core objects with the atomics and bulk-memory features required by the `sonare-rt` shared-memory target, so `bindings/wasm` can build both embind and realtime worklet artifacts together.

### Bug fixes

- Clips scheduled on a stopped engine no longer emit a sustained buzz: the clip bus is gated on the transport rolling, matching the sequenced-MIDI gate, and `render_offline` now rolls the transport for the render duration and restores the prior state so offline clip / MIDI rendering works without a manual play command.
- Acoustic IR clarity (`clarity_db`) and definition (`definition_d50`) are scoped to the Lundeby truncation index instead of the full energy vector.
- SMF2 tempo conversion adds overflow guards and diagnoses timed events before the DCTPQ header; the phase vocoder guards `hop_length <= n_fft/2` and fixes its final-frame output count; and waveform peak bucket counts fix an off-by-one.
- Binding hardening: the Node addon adds `RequiredUint32Property` / `RequiredDoubleProperty` helpers that throw `TypeError` on missing or wrong-typed fields and validates `audio_channels` before deriving frame counts; Python tightens `warp_mode` parsing (rejecting booleans / ints / unknown strings) and guards page-provider `close()`; WASM extends `wrapModuleErrors` to wrap native embind objects returned from top-level factory functions.

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
