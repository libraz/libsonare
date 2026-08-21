# Changelog

## Unreleased

### Mixing assistant

**A new optional subsystem, and an addition rather than a change.** Nothing that existed before behaves differently because of it, and `-DBUILD_MIXING_ASSISTANT=OFF` removes the whole thing while leaving the mixer, the routing model and every existing scene exactly as they were. It is a separate target with a one-way dependency: it reads the mixing and mastering types and nothing in the runtime reaches back into it.

Given a set of tracks it measures each one, measures what happens between them, and returns a mixer scene — input trims, faders, pans and widths, corrective EQ, dynamics, effect buses and sends — together with a written reason for every decision it made. `suggestMixScene` on Node and WASM, `suggest_mix_scene` on Python, `sonare_mixing_assistant_suggest_scene_json` on the C ABI.

**It suggests; it does not apply.** No audio is processed and none is emitted. Handing the returned scene to the mixer is the caller's own explicit step, and there is deliberately no convenience entry point that collapses the two halves into one call — a mix has no single right answer, so nothing is applied on the user's behalf.

Every decision is rule-based. There is no trained model, no statistical classifier and no learned parameter anywhere in it; source classification is a single-layer decision table over measured features. The numbers the rules start from are studio convention, and where a peer-reviewed survey of professional practice has measured the same quantity, the convention has been checked against it rather than left as folklore: the reverb return level, the reverb pre-delay, the lead vocal's position, the width of a wide pan, and the frequency ordering of the compression ratios all rest on that survey.

`enableHighPass` is off by default, which is the one place the assistant declines to do something a mixing habit would suggest. The same survey found the blanket per-track high-pass seldom used in studio mixing and unsupported by subjective testing. Switched on, the filter is proposed from the track's measured energy below its register rather than from its class label, so a part written low keeps what it plays.

### Python

`libsonare.SonareValueError` is new and is now what Python-side buffer and argument validation raises. It subclasses both `SonareError` and `ValueError`, so existing `except ValueError` and `except SonareError` handlers keep working unchanged; code that inspects `.code` gets `ErrorCode.INVALID_PARAMETER`. The message text is unchanged — the numeric prefix `SonareError` carries is deliberately not prepended. CLI usage errors still raise plain `ValueError`, because those report a command-line mistake rather than an API argument, and the CLI exit code for both is 3.

`mel_spectrogram`, `mfcc`, `stft`, `chroma`, `chroma_cens`, `chroma_cqt` and `bass_chroma` now validate their buffer length before the call instead of relying on the native side, so an undersized buffer is reported against the function the caller invoked rather than against an internal helper or a C symbol. `zero_crossings` and `pitch_tuning` reject empty and non-finite input, which they previously accepted and answered with a fabricated result.

**The exception type and message text change for around seventy buffer-taking entry points.** Every public function whose leading argument is a sample buffer now preflights it, so the analysis, mastering, repair, spectral-feature, constant-Q and segmentation entry points raise `SonareValueError` naming the function and the offending argument — `master_audio_stereo: right must not be empty`, `spectral_centroid: samples contains NaN or Inf at index 0` — where they previously let the call reach the C ABI and surfaced the bare `SonareError: [4] Invalid parameter`. `SonareValueError` subclasses both `SonareError` and `ValueError` and carries `ErrorCode.INVALID_PARAMETER`, so a caller catching either base class or branching on `.code` is unaffected; a caller matching on the message text is not. `cross_similarity` reports its own `x` / `x_rows` parameter names rather than the internal `data` / `rows` spelling, and the scalar and stereo meters name the facade function instead of the `sonare_`-prefixed C symbol.

Five entry points reject input they previously accepted. `trim_silence`, `split_silence` and `fix_frames` raise on an empty buffer instead of returning an empty result, and `tempogram_ratio` rejects a `factors` entry that is not finite and positive — a NaN there reached an undefined float-to-int conversion in the core and an infinity silently degenerated to the DC lag. `mix_stereo` rejects a scene it cannot mix: strips that are all empty previously answered with an empty result and per-strip meters reading the `-120` dB floor, which is what a real mix of silence looks like, and a strip carrying NaN metered as that same floor because a NaN loses every comparison the peak meter makes. Both now raise, naming the strip index and channel. A zero-frame block stays a valid no-op at the C ABI, where it means "process this block" rather than "mix these strips". Element-wise conversions keep their empty-in / empty-out contract and are unchanged: `power_to_db`, `amplitude_to_db`, `db_to_power`, `db_to_amplitude`, `preemphasis`, `deemphasis`, `vector_normalize`, `frame_signal`, `pad_center`, `fix_length`, and the tempogram family. `f0_hz` is likewise never scanned for non-finite values, because pYIN marks an unvoiced frame with NaN and that track representation must pass straight into pitch correction.

**The feature ABI moved from 4 to 5.** `SonareNoteSegmenterConfig` gained a trailing `voiced_threshold` field, so the Python binding refuses a shared library built from a different tree; rebuild the library alongside the binding. Existing C callers are otherwise unaffected at both the source and the call level: the new field is read only when `struct_version` is 2, so a struct filled as before keeps its previous behaviour. The engine, project, voice-changer and acoustic ABIs are unchanged.

The realtime engine gains two things it could describe but not do — continuous automation of a hosted instrument's parameters, and pitch-preserving warp playback — and the clip streamer now asks for pages before it needs them instead of after it has already read silence. Two long-standing contract bugs are fixed on the analysis side. Two behaviour changes reach existing callers and are called out below: the strength of time-varying pitch correction, and the rate at which a track's fader, pan and gate smoothers advance.

Two test-side defects are fixed alongside: the mel filterbank cache tests compared against an address the cache had already freed, which an allocator may hand back for the rebuilt entry, so the eviction test failed intermittently and the LRU-promotion test could pass on a cache that had dropped the entry; both now hold the returned handle. The golden-hash comparisons no longer abort at the first drifted row, so a fixture that has fallen behind a whole processing stage reads as such instead of as one stale preset.

The built-in mastering preset golden hashes are refreshed for the six presets that enable the air band. Those hashes still described the output from before the air band's dry and harmonic paths were time-aligned and before the offline loudness stage was measured from its own input; both of those shipped in earlier releases without the fixture being regenerated. No mastering behaviour changes in this release.

### New surfaces

- Hosted instrument parameters are continuously automatable. `sonare_engine_resolve_instrument_automation_id` returns a reserved parameter id for a named instrument parameter, in the same shape as the existing track, master and bus insert resolvers, so a host can drive a synth's cutoff or vibrato depth from an automation lane at audio-block precision instead of stepping it from the control thread. The resolved lane is smoothed on the audio thread, so live and offline rendering agree. Builds without the arrangement subsystem report `instrumentParamAutomation: false` in the capability JSON, which is how a host detects that the resolver will not answer.
- Warped clips can follow their warp map without transposing. `'time-stretch'` joins `'off'`, `'repitch'` and `'tempo-sync'` as a clip warp mode on both the engine and the project surfaces. It reads the same anchor map as `'repitch'` but synthesizes the output by overlap-adding source segments at a fixed rate, so changing the map moves the timing and leaves the pitch where it was — and it takes effect from the next block, with no control-thread bake to redo. The stretcher's voices are preallocated; a clip that cannot get one falls back to `'repitch'` behaviour rather than allocating on the audio thread.
- `sonare_decompose_stems` separates a signal into listenable components. `sonare_decompose` returns the factors of a magnitude spectrogram, which carry no phase, so reconstructing from them needs a phase estimator and the result does not survive as a stem. This instead builds a per-component soft mask from the same factorisation and applies it to the original complex spectrogram, so every component keeps the source's phase and the components sum back to the input. Node and WASM take a request object (`decomposeStems`), Python takes keyword arguments (`decompose_stems`).
- `sonare_remix_aligned_intervals` resolves the cut points `sonare_remix` would use without cutting. Zero-crossing snapping is a per-signal decision, so calling `remix` channel by channel snaps each channel to a different frame and drifts a stereo take apart. Resolving one cut set from one channel and applying it to every channel is the fix, and this is the entry point for it.

### Fixes

- **Behaviour change, WASM.** Errors raised inside the mixing and project C-ABI entry points now arrive as the code those entry points document, instead of as the raw C++ exception. The exception flag was on the module's link line but not its compile line, and emscripten drops landing pads while compiling, so every `catch` in the module had been deleted — including the guards that turn a throw into an error code. A `Mixer.fromSceneJson` on a scene naming an unknown insert reported `InvalidParameter` with the inner message and now reports `InvalidState` with the facade's wrapped message; malformed scene JSON reported an unknown-error code and now reports `InvalidState`. Callers matching on the previous codes or messages need updating; callers that only branch on success are unaffected. The other three surfaces always behaved this way.
- **Behaviour change, Node.** `SonareError` is a runtime class rather than a type-only interface, matching the WASM package. Importing the name by value no longer yields `undefined`, and `instanceof` works — it is brand-based, so an error raised by the addon (which does not construct the class) still narrows, as does one that lost its prototype crossing a worker boundary. `isSonareError` is unchanged and still the documented guard.
- **Behaviour change.** `pitchCorrectToMidiTimevarying` and `pitchCorrectTimevarying` no longer scale the correction amount by `voicedProb`. pYIN's voiced probability is a per-frame observation mass that depends on how many periods fit in the analysis window, so it falls with the fundamental — using it as a correction weight silently under-corrected low registers. `voicedProb` now only derives voicing when the explicit `voiced` flags are absent, which is what it always documented. Callers that were passing pYIN output straight through will see stronger correction in the low register; callers that pass explicit `voiced` flags are unaffected in every register.
- `noteSegments` takes a configurable `voicedThreshold` (default 0.5, the previous fixed value), so a caller working with low-register material can lower it instead of losing every segment. The pYIN documentation across all four surfaces now states what `voicedProb` measures and why it tracks the fundamental.
- `remix` with `alignZeros` no longer returns an empty result for material with no zero-crossing to snap to. A signal with no sign change at all — silence, a DC offset, any constant — is left unsnapped, and a slice that had content but collapses to empty after snapping keeps its unsnapped boundaries. Ordinary signals snap to the same frames as before.
- The realtime clip streamer requests pages ahead of the playhead instead of only reporting a miss after the read. A host servicing page requests could not get a page resident in time, so every page boundary rendered a block of silence; the look-ahead window is configurable and defaults to half a second. The read-then-miss reporting is unchanged and still runs, so a host that ignores the look-ahead behaves exactly as before.
- `chroma_dtw_align` no longer rejects every call. It assigned its `binsPerOctave` to the chroma CQT grid without moving the bin count with it, so the default resolution stretched the grid to twenty-one octaves and its top bin ran past Nyquist. The bin count is now derived from the octave span, matching what the chroma entry points already did. Seven octaves above C1 top out under 4 kHz, so any sample rate from 8 kHz up carries the whole grid.
- **Behaviour change.** A group bus's non-linear inserts run once on the summed bus signal. The engine opened the lane and bus stages twice per block — once for clip audio and once for hosted instruments — so a saturating or compressing bus insert was applied separately to each contributor rather than to their sum. As a side effect of merging the two passes, the lane fader, pan and gate smoothers now advance once per block instead of twice, so solo and mute ramps take their documented time rather than half of it.

## v1.7.2 (2026-08-18)

This release opens three paths a host could describe but not take: routing the cue bus to its own AudioWorklet output, running the streaming mastering chain inside the worklet realm, and carrying a selection across a destructive MIDI-FX bake. The ABI is unchanged and no existing call behaves differently.

### New surfaces

- The WASM zero-copy realtime path can separate the cue bus from the program output. `prepareMonitorChannels`, `getMonitorChannelBuffer` and `processPreparedWithMonitor` are the monitor-tap counterparts of `prepareChannels` / `getChannelBuffer` / `processPrepared`, and the AudioWorklet node takes a `cueOutput` option that gives the processor a second output fed by the PFL/AFL tap. Per-track monitor modes have been settable since v1.7.0, but the worklet render path only ever called the plain `process`, which folds the cue into the program mix — so a host could select PFL and still have nowhere for the cue to go. The copy-in `processWithMonitor` and the C entry point it mirrors were already available and are unchanged; a node built without `cueOutput` keeps one output and the folded mix, sample for sample.
- `StreamingMasteringChain` is exported from the AudioWorklet entry, so a live mastering preview no longer has to round-trip audio to the main thread. Its class documentation now states the contract that reaching the render thread makes load-bearing: `prepare` allocates and belongs in a message handler, an enabled loudness stage still requires the offline-measured `loudnessStaticGainDb`, flush output leads by the reported latency, and the chain is a host-side stage outside the engine's own delay compensation.
- `sonare_project_bake_midi_fx_ex` reports where each baked event came from: one index per transformed event naming the input it derives from, in the same canonical order the bake commits. Chord and arpeggiator fan-out lands several outputs on one source index, which is what lets a host tell the surviving event from the newly generated ones and keep a selection or an editorial annotation across the bake. `sonare_project_preview_midi_fx_count` runs the same deterministic transform without mutating the project, so the buffer can be sized exactly. Node and WASM fold both into a request form of `bakeMidiFx` plus `previewMidiFxCount`; Python takes a `with_source_index` keyword and `preview_midi_fx_count`. The positional and keyword forms without provenance are byte-identical to before, and only a caller that asks for the map pays for the narrower drain that produces it.

### Documentation

- The insert-automation resolvers say which mastering processors they cover. The `eq.*`, `dynamics.*`, `saturation.*`, `spectral.*`, `stereo.*`, `maximizer.*` and `multiband.*` processors are all available as strip inserts, so resolving one through `sonare_engine_resolve_track_insert_automation_id` and its master and bus counterparts already drives it at audio-block precision. The stages with no automation id are the whole-signal ones — `repair.*`, `loudness` and the match stages — which buffer the entire signal and do not run on the realtime path at all.

## v1.7.1 (2026-08-15)

**The feature ABI moved from 3 to 4.** `SonareSynthPatch` gained a trailing `present_fields` word, so the Python binding refuses a shared library built from a different tree; rebuild the library alongside the binding. Existing C callers are otherwise unaffected, at both the source and the call level: the new field is read only when `struct_version` is 2 or higher, so a struct filled as before keeps its previous behaviour. On Python, the `SynthPatch` numeric fields default to `None` instead of `0.0`, so reading a field that was never set returns `None` rather than zero.

This release restores the offline formant path, which lost the whole LPC prediction gain, adds stereo variants of the mastering analysis entry points, lets a synth patch override a field with an explicit zero, and corrects two JavaScript declarations that rejected input the runtime accepts.

### New surfaces

- Mastering analysis reads both channels through `sonare_mastering_audio_profile_stereo`, `sonare_mastering_assistant_suggest_stereo`, `sonare_mastering_streaming_preview_stereo` and `sonare_metering_crest_factor_db_stereo`, which take the planar left and right pair the rest of the stereo mastering surface uses. The mono entry points required a `0.5*(L+R)` downmix, which reads 6.02 dB below the BS.1770 channel sum on decorrelated material and cancels entirely on an anti-phase pair; the streaming preview derived both its normalization gain and its ceiling-risk flag from that loudness, so the error reached the verdict a host shows its user. Only the profile's loudness block is measured from the channels — the spectral, dynamics and tempo fields describe shape and timing rather than absolute level, so they stay on the downmix and remain comparable with the mono entry point field by field. Mirrored as keyword arguments on Python and as request objects on Node and WASM.
- `SonareSynthPatch` distinguishes a field set to zero from an omitted one. Every numeric field previously read zero as "keep the base", so a host could not ask a preset for no stereo spread, no bus drive or a zero sustain. `present_fields` under `struct_version` 2 names the fields the caller set on purpose: a set bit overrides even when the value is zero, a clear bit keeps the version-1 behaviour, and the modulation-matrix bit lets an empty routing table clear the base matrix rather than keep it, which the routing count alone could not express. The preset read direction reports every bit set, so a preset field that happens to be zero survives a round-trip instead of decaying into "keep base". Node and WASM set a bit for each key the descriptor actually carries, and Python's dataclass fields default to `None` so a supplied zero is a real override.

### Bug fixes

- Offline `voiceChange` with a non-unity `formantFactor` no longer collapses in level. `FormantWarp` recoloured the LPC residual with the absolute all-pole envelope, but the residual already carries the frame's excitation, so every frame was scaled a second time by its own residual RMS. Because that factor is derived from the signal, the transfer was quadratic in input level rather than a fixed offset, and a unity factor hid it by returning the input untouched. The realtime formant path was never affected.
- `pitchCorrectToMidiTimevarying` and `pitchCorrectTimevarying` accept the voicing a caller actually has. Both declared `voiced` as an `Int32Array` while `PitchResult.voicedFlag` is a `boolean[]`, so the natural call did not type-check on either JavaScript surface. The parameter is now a `VoicedFlags` union over the boolean, plain numeric and typed array forms, reduced to 1 and 0 inside each facade — the Node addon reads only an `Int32Array` and silently voices every frame otherwise.
- `MasteringChainConfig` accepts the dot-notation override spelling. The flattener passes a caller-supplied dotted key straight to the core, which is the form the C ABI carries parameters in and the form the Python binding documents as accepted, but the type modelled only the nested spelling. Both forms now type-check; the nested form is still checked field by field, and an unknown key without a dot is still rejected.

### Documentation

- The synth preset table's engine attributions match the engines that actually render. The rows grouped under "FM" and "Karplus-Strong" are aliases of GM fallback programs, so `gm_fallback_map` picks the engine, and `bell`, `brass` and `pluck` resolve to the modal, lip-reed and plucked-string engines. The v1.7.0 note describing the `harp` preset key as renamed to `harp-plucked` is restated as the duplicate-key removal it was: the table carried two entries named `harp`, lookup returns the first match, and `harp` resolved to the GM fallback orchestral harp both before and after the rename.

## v1.7.0 (2026-08-14)

**This release contains source-incompatible changes on the JavaScript and Python surfaces, and both command-line tools changed exit codes.** Read Behavioural changes before upgrading; the items that change meaning without raising an error are:

- Both CLIs exit 2 for a usage error where the invalid-parameter code was reported before, and a cancelled run exits 11. A script branching on the old codes will misread the outcome.
- Python raises `SonareError` where native parameter validation previously surfaced as `ValueError`. Python-side preflight of empty, NaN or Inf buffers and bad shapes still raises `ValueError`.
- WASM `voiceCharacterPresetId()` returns `VoicePresetId | null` instead of a string, and an unknown ordinal returns null rather than throwing.

This release completes the option coverage of the analysis and effects entry points on every surface, adds typed automation targets, per-track PFL/AFL monitoring, bounded undo/redo memory and owning audio-source metadata, rebuilds the native CLI on a single option registry with a machine-readable contract, and adds a cross-surface conformance harness. It also corrects the predominant local pulse, subsegmentation, chord inversion and pitch-tracking defects, bypass latency continuity in the mixer and the routing graph, the macOS host backends, and a set of binding-level resource and validation defects.

### New surfaces

- Configuration-taking variants of the remaining one-shot effect and analysis entry points reached the C ABI: `sonare_hpss_ex` (soft or hard mask with an optional residual), `sonare_time_stretch_ex`, `sonare_pitch_shift_ex`, `sonare_trim_ex`, `sonare_normalize_rms`, `sonare_nnls_chroma_ex2`, `sonare_analyze_impulse_response_ex` and `sonare_audio_file_channel_count`. The existing entry points forward with their previous defaults, so current calls are unaffected. Node and WASM take the new settings as additional request fields (`nFft`, `hopLength`, `hardMask`, `frameLength`, `minDecayDb`, a peak or RMS `mode`), Python as keyword arguments, and both CLIs as flags.
- Automation lanes carry a typed target: `SonareAutomationTargetKind` distinguishes an opaque parameter id from the track fader and pan, `SonareAutomationLaneDescEx` describes it, and `sonare_project_add_automation_lane_ex` / `sonare_project_edit_automation_lane_ex` install it. Typed lanes resolve to the engine's reserved parameter namespace at install time and are applied by the offline bounce through the track mixer. Project JSON moves to schema version 2 only when a typed lane is present, so a document without one keeps its existing bytes. Mirrored as `targetKind` on Node and WASM and as a typed target argument on Python.
- Per-track-lane cue monitoring is available as `SonareEngineTrackMonitorMode` and `sonare_engine_set_track_monitor_mode`: PFL taps after the lane strip, AFL after the fader, gate and pan, including the surround path. It is a queueable realtime command, mirrored as `setTrackMonitorMode` on Node and WASM, `set_track_monitor_mode` on Python, and reachable from the WASM AudioWorklet.
- Audio sources carry owning metadata — a content hash and an external stem role — through `SonareProjectAudioSourceMetadata` with set, get and free entry points, committed as one undoable edit and surfaced as `contentHash` / `externalStemRole` on the project source descriptors.
- Undo/redo memory is bounded: every edit command reports its retained bytes, the history enforces a combined cap over the undo and redo stacks, and `sonare_project_set_max_history_bytes` exposes it. A new rollback seam restores PCM sidecars when an apply or commit step fails part-way.
- Engine telemetry error ordinals are published as `SonareEngineTelemetryError`, so a host can name a telemetry error instead of matching integers. The telemetry struct layout is unchanged.
- The mastering chain document gained a structured multiband compressor with an arbitrary band count, written and parsed as configuration schema version 2 with strict field validation.
- `Project.create` and descriptor-form assist sidecars are available on the JavaScript surfaces, and synth instrument bindings accept `useGmPrograms` so an offline bounce follows incoming GM bank and program changes.
- The native CLI publishes its own command and option inventory through a hidden `--dump-cli-contract`, and the Python CLI derives the same inventory from its live parser, so the published option contract cannot drift from the parser that runs.

### Analysis

- `plp` reconstructs the predominant local pulse from the masked Fourier tempogram's phase through a COLA-normalized inverse STFT instead of stamping a magnitude-only cosine at each frame centre. The previous reconstruction summed mutually misaligned cosines into a near-flat curve unrelated to the onsets; pulse peaks now land on the onsets. `fourier_tempogram` and `plp` share one complex-STFT front-end.
- `subsegment` splits a parent span with contiguity-constrained Ward clustering, so a span yields exactly `min(n_segments, len)` temporally contiguous runs. Unconstrained clustering previously emitted recurring labels after an interruption, producing more boundaries than requested and non-contiguous segments.
- Chord inversion detection reads the bass chromagram in the harmonic chromagram's frame space, and a host-supplied bass source with a different time base has its segment indices remapped through absolute time, so the reported bass pitch class and inversion no longer depend on the bass hop length.
- `piptrack` can report a peak at the topmost FFT bin, which was unreachable once `fmax` reached Nyquist. The top bin counts as a local maximum whenever it exceeds its predecessor, and the peak is reported at the bin centre with the raw magnitude.
- One interpolated-percentile kernel backs the dynamics analyzer, dynamic-range and LUFS metering and the acoustic percentile, replacing four hand-synchronized copies. It follows numpy's default linear rule, accumulates in double and returns an exact-rank element directly, so an infinite neighbour at weight zero can no longer turn a result into NaN.
- One shared CQT-bin to pitch-class fold backs both the `chroma_cqt` mean wrap and the CQT summed pitch-class accumulation, with a single definition of the bins-per-octave centering shift and the `fmin` rotation. `chroma_class_of_frequency` subdivides the twelve pitch classes correctly for resolutions other than twelve.
- CQT and VQT inversion share one Gaussian spectral projection, so `griffinlim_vqt` inverts with the VQT's own bandwidths when gamma is non-zero while the `VqtResult` overload stays pinned to gamma zero.
- `bin_to_hz` rejects a non-positive sample rate or FFT size instead of dividing by zero, and the reassigned spectrogram rejects a non-positive FFT size or hop length. The guards live in the core, so the WASM path that calls the reassigned entry point directly is covered.

### Mastering and mixing

- A bypassed insert keeps its latency compensated. Bus processors and channel strips gained a second per-insert alignment bank that substitutes for a bypassed insert's latency across every plane, kept primed while the other bank is active, so toggling bypass switches delay lines instead of opening a hole of silence.
- A bypassed routing-graph node keeps its per-port latency compensated through a delay sized from the node's reported integer and fractional latency, dropped entirely when no port is latent.
- Multiband and EQ processors accept a caller-declared channel bound at prepare, so an offline mono or stereo caller sizes per-channel scratch to its real channel count instead of reserving the realtime ceiling, and crossover scratch already sized for more channels is reused rather than reallocated on the audio thread.
- The air band delays its dry shelf path by the harmonic oversampler's round trip so the two paths stay time-aligned, reports that round trip as latency rather than tail, and derives its detector envelope from the sample rate.
- The loudness stage measures its own stage input rather than the chain's input report, and a tape or exciter parameter override no longer implicitly enables that stage.
- Acoustic room inserts carry material preset, per-band absorption and scattering, Eyring preference, mixing time and crossfade options into the streaming insert, and the processor catalog corrects the realtime cost tier of the tube saturator and the true-peak maximizer.
- Shoebox room validation reports every bad wall coefficient instead of aborting at the first one, `sonare_estimate_room` pads the shorter of the absorption and RT60 estimates with NaN instead of truncating the band count, and room impulse-response synthesis is bounded by a shared working-set budget with a single late-tail resolver, distinguishing a clamp caused by the requested length from one caused by the resource budget.

### Realtime engine, project and MIDI

- Meter telemetry merges across a host block that automation splits into several process calls: peak and true-peak are element-wise maxima over every sub-block and RMS is recomputed from a running energy accumulator, instead of reporting only the last fragment.
- The master scope record is captured per sub-block before the metronome rather than once at the end of the block, so the metronome is excluded from the master scope.
- Program delay compensation is reconfigured fail-closed: every replacement delay bank is built before any is installed, a zero delay reclaims its storage, and the reported prepared scratch includes the compensation storage.
- The engine rejects an offline render, bounce or freeze asking for more channels than prepare reserved instead of writing past the reserved scratch, and reports a channel-bound violation as its own telemetry error rather than the block-size one.
- The built-in synth renders every GM program: the fallback engine is resolved per program at note-on, every engine's per-voice delay slab is allocated up front, and the reported release tail is sized from the GM fallback tables whenever GM mode or the drum kit is reachable. The native synth and the SoundFont player share one bank-resolution rule and one drum-kit table.
- Sostenuto captures only on the pedal-down edge and a reused voice slot clears its stale capture; SoundFont exclusive-class choking is scoped to voices predating the current note-on, so a multi-layer strike no longer chokes itself.
- Project load rejects a present-but-wrong-typed scalar field instead of substituting the default, enforces the edit-API invariants on the assembled model, writes marker key fields whenever they carry a value, and walks an embedded scene in place instead of re-serializing it under a separate budget.
- Project MIDI import is gated on a persistence round-trip preflight and a project-specific event budget, and SMF parsing skips a zero time-signature numerator and pads a non-MTrk chunk only with an MTrk lookahead.
- External stem import no longer copies the whole audio content store; ids are transferred with a collision preflight.
- Non-WAV and non-MP3 input decodes with its source channel layout through FFmpeg for both interleaved loads and channel-count probes, with overflow-checked sample accumulation.

### Voice changer

- The config hand-off moved to a seqlock cell with a monotonic version counter, so setting a config allocates nothing, locks nothing and throws nothing, and is safe to call from the audio thread itself — which the WASM AudioWorklet path needs, since its port handler and process block share one thread. A torn read is reported as a failure and the audio thread advances its applied version only on a consistent read, so a burst of writes cannot permanently drop its final update.
- Control updates run on an absolute 32-sample cadence shared by the limiter, retune and formant stages, with cached decibel and coefficient derivations and a per-grain retune ratio.
- A malformed macros section fails the load with an invalid-parameter error instead of falling back to defaults, and the preset validator preserves the caller's id, name, description and category instead of the config-only placeholders that broke pack lookups.

### WebAssembly

- The worklet capture protocol carries a discriminated response type with transferable `Float32Array` channels in place of number arrays, matches replies to their request operation, and rejects a malformed reply instead of dropping it silently.
- Mixer and capture buffers cross the JavaScript boundary through typed-array bulk copies rather than per-sample access.
- Pan-law spellings are accepted case-insensitively with underscores read as hyphens, exported as the `PanLawName` and `PanLawInput` types.
- Web MIDI hotplug fires the inputs-changed callback once per port change, and a MIDI 2.0 note-on stays a note-on regardless of its downsampled 7-bit velocity, since note-off has its own status nibble. MIDI ring polling stops once the last listener unsubscribes.
- The size gate is split: the baseline file records the measured build while a separate budget file holds the enforced ceiling, so the gate fails on real growth rather than on every byte of drift.
- The offline worker smoke harness drives Chrome through the DevTools protocol, waiting for the page result and always stopping the browser.

### Node

- The streaming mastering chain, stream analyzer and streaming equalizer gained an idempotent `destroy()` and `Symbol.dispose`, so the native handle is released deterministically, including through `using`.
- `analyze`, `analyzeAsync` and `mixStereo` throw a `SonareError` carrying the C-ABI error code on every listed failure path, a throwing progress or cancel callback surfaces promptly instead of aborting on a second throw, and `mixStereo` releases the native mixer on every exit path.
- The asynchronous mastering entry points reject their returned Promise with a `TypeError` for missing, wrong-typed or length-mismatched arguments instead of throwing synchronously.
- `RealtimeVoiceChanger` destroys its native handle when `prepare()` throws during construction, where the handle was previously leaked.
- The request form of `vqtToAudio` shares the positional form's defaults, so an omitted `gamma` resolves to the automatic-VQT sentinel as on every other surface, and `pitchPyin` rejects NaN and Inf samples like `pitchYin`.
- Addon entry points read optional JavaScript fields through the shared property readers, so an explicitly undefined optional field is treated as omitted rather than coerced.
- Note-segment tuning fields are taken flat on the request, the nested configuration object is deprecated, and supplying both is rejected. Capture buffers accept a channel-count and capacity form beside the deprecated caller-owned plane array. Public types that were reachable only through internal modules are re-exported.

### Command-line tools

- The native CLI is driven by one immutable registry of command leaves and typed option specs, replacing the separate arity table, per-command schemas and accept lists. Aliases and declared defaults resolve through it, repeatable options accumulate, and numeric values and required options are validated during argument validation.
- Newly forwarded options: chords `--smoothing-window` and `--no-beat-sync`, mel `--htk`, key `--candidates`, mastering `--true-peak-oversample` into the assistant chain, and an always-applied analyze `--chroma-highpass`.
- The Python CLI covers the remaining options of its analysis, effects and mastering commands, splits stdout-only commands from artifact-producing ones so `--output` on an analysis command is a usage error, and requires a subcommand.
- Both CLIs report the same JSON: chroma mean energy as an array, minimum and maximum on spectral statistics, beat intervals on rhythm, canonical lowercase section types, sample rate and latency on the mastering payloads, a per-diagnostic message on project compile, and snake_case doctor keys.
- Project bounce and MIDI render default the sample rate to the project's own stored rate for both the render and the WAV header; an explicit rate is accepted only when it matches.
- `voice-change` pads its input by the chain latency, processes fixed blocks and drops the pre-roll, so output sample k corresponds to input sample k, and preset resolution routes through the strict validator so a mistyped section or macro key fails loudly.
- Ambiguous combinations that previously only warned are rejected: mastering preset with configuration or assistant, EQ shortcuts alongside explicit parameters, competing voice-changer preset selectors, HPSS output modes and the trim-silence thresholds.
- Both CLIs treat `--preset-pack` and `--preset` as one selector naming a file and an entry inside it, so a pack without an entry is reported as the missing `--preset` rather than as a rule that reads as if no selector had been given.

### Platform and host backends

- The CoreAudio configuration handed to the callback's open carries the device's nominal sample rate and reported input and output latency, so a callback seeding delay compensation is no longer left with zero or the wrong clock domain.
- CoreMIDI manual injection produces into its own event ring and SysEx reassembler, separate from the live callback's, so an on-screen keyboard keeps working while a device is connected and each ring keeps a single writer. Drains merge both by render frame, injected SysEx keeps the caller's timestamp, the group is masked before indexing reassembly state, and close clears both rings. Output flush reuses one event-list storage block instead of zero-initializing roughly 68 KB per call.
- The Audio Unit effect's input render callback clamps to the current block's frame count rather than the prepared maximum, fixing an out-of-bounds read on a variable-block-size host, and the MusicDevice instrument exposes its dropped-event counter while keeping allocation failure inside its noexcept boundaries.

### Verification

- A cross-surface CLI contract checker compares both CLIs against a manifest fixture that is independent of either implementation, covering the command inventory, option and alias parity, positional and exit-code contracts, closed payload schemas and native-versus-Python payload equality within a declared tolerance.
- A GM-program project bounce acceptance check renders the oracle through the C ABI and requires the Python, Node and WASM project facades to match it samplewise in both GM-program modes.
- One shared pan-law name fixture is checked by every binding, so accepted spellings, normalizations and rejected forms cannot diverge.

### Bug fixes

- Every analysis wrapper zeroes its result struct and owned out-pointers ahead of each validating early return, so a `sonare_free_*_result` after a rejected call can no longer free an uninitialised pointer.
- The mastering preset-name cache is guarded and uses a write-once flag instead of an emptiness test, so an empty preset set cannot invalidate a previously returned pointer, and the capability catalog fails cleanly when the processor catalog returns null.
- `sonare_engine_bind_midi_cc_binding` validates its arguments against the C surface in every build, and the wildcard channel is published as `SONARE_MIDI_CC_ANY_CHANNEL`.
- Optional subsystems compile out cleanly across the MIDI transport SysEx emission, the mixing-lane parameter constants and the SoundFont insert-factory wiring, and the capability catalog reports empty preset lists for absent subsystems.
- Numeric edges are checked rather than wrapped: matrix view indexing, hertz-to-bin, samples-to-frames and note-to-hertz conversion, the decibel converters' finite arguments, and bounce buffer sizing.
- Marker ids are pre-allocated and an imported clip whose events end at tick zero keeps a usable length; id-returning edit calls read the committed model rather than the command object.
- A security policy states where to report a vulnerability privately, the supported-version window, and what is and is not in scope across the audio, MIDI, SoundFont and project-file decoders and every binding.

### Performance

- The pYIN Viterbi transition table is precomputed as log weights and the voicing switch logarithms are hoisted out of the frame loop, so the logarithm runs once per transition pair instead of once per inner-loop iteration.
- Beat-local low-frequency energy is computed once per analysis and shared by the beat and chord downbeat-refinement passes, which each re-filtered the whole signal before.
- The benchmark fixture generator emits ground truth derived from the same constants that drive the synthesis and prints the fixture digest, an accuracy pass scores a build against it under the standard tempo, beat, chord and key conventions, the harness builds for WebAssembly, and both harnesses record thread count and load average and warn on a contended machine.

### Behavioural changes

- Both CLIs report a parse or schema failure as a usage error exiting 2 instead of the invalid-parameter code, a cancelled run exits 11, and `project validate --strict` exits 9 after the canonical artifact and diagnostics have been written.
- Python raises `SonareError` with a numeric code for native return-code failures including native parameter validation, where some of those previously surfaced as `ValueError`; Python-side preflight of empty, NaN or Inf buffers and bad shapes still raises `ValueError`, and a malformed project document surfaces as an invalid-format error at the CLI boundary.
- The built-in synth preset catalog no longer registers two patches under the key `harp`: the physical-model plucked voice is registered as `harp-plucked`, so every catalog key resolves to exactly one patch. Lookup returned the first match, so `harp` resolved to the GM-fallback orchestral harp before and still does; an existing configuration naming it is unaffected.
- WASM `voiceCharacterPresetId()` returns `VoicePresetId | null`, and an unknown ordinal returns null instead of throwing. The realtime voice-changer preset type is a `dsp`-or-`macros` union with id, name and category required, and a document supplying both sections is rejected.
- The WASM worklet capture read payload is a transferred `Float32Array` array rather than nested number arrays, and requesting captured audio throws when channels are missing instead of returning an empty result.
- Node's asynchronous mastering request no longer accepts a `cancel` callback, and the nested note-segment configuration object is deprecated in favour of flat request fields.
- Multiband and EQ processors reject a block or channel count above their prepared bound instead of growing scratch, and the linear-phase EQ recreates state at exactly the prepared capacity, so it can shrink.
- `RoomReverb` construction rejects invalid geometry with an invalid-parameter error instead of degrading to a dry passthrough, and acoustic room inserts reject it too.
- The air band's detector envelope is derived from the sample rate rather than fixed, so its output differs away from 48 kHz; mastering output also shifts where the dry shelf path is now latency-aligned and where the loudness stage measures its own input.
- An automation lane with a target parameter id of zero is rejected, an opaque lane colliding with the engine's reserved namespace is an error rather than accepted, and a track holds at most one lane per target kind.
- A channel strip bound by several tracks processes only the sum of their signal, with each track's gain, pan and mute folded into its own clips, reported as a shared-channel-strip diagnostic; two tracks automating the same target no longer collide silently in the live engine, and the loss is reported as an automation-lane-conflict diagnostic.
- A non-zero clip warp ref id naming no registered warp map is rejected on every surface.
- Project JSON with a present-but-wrong-typed scalar field is rejected as invalid format; an absent field still falls back. The default project-import string budget doubled to 64 MiB, project MIDI import is capped at 250 000 events, and a file that fails the persistence round-trip preflight is rejected even though it parses.
- The serializer emits configuration schema version 2 for a mastering chain carrying a structured multiband compressor and project schema version 2 for a typed automation lane, keeping version 1 otherwise.
- `sonare_estimate_room` pads the shorter of the absorption and RT60 estimates with NaN instead of truncating the band count, so consumers must treat NaN entries as not converged.
- An offline render, bounce or freeze requesting more channels than the engine prepared for is an error, and the graph node and connection counts report not-supported on a graph-disabled build instead of zero.
- WASM rejects an out-of-range key profile, key mode and stream-analyzer window ordinal at construction instead of falling back to a default, requires every field of the flat realtime voice-changer configuration, and maps `panMode: 'pan'` to the pan law it names rather than aliasing to balance. Node's MIDI helpers raise a single `TypeError` on a missing or wrong-typed required field.
- `plp`, `subsegment`, `piptrack`, chord inversion and the VQT inversion path produce different output where the fixes above apply, and the loudness range, dynamic range and acoustic percentile metrics now share one double-precision definition.
- A bypassed insert or graph node stays latency-compensated, so bypass toggling and steady-state alignment differ from the previous behaviour.
- CLI `voice-change` output is latency-compensated so sample k maps to input sample k, and a preset with a mistyped section or macro key is rejected rather than rendered.
- The Python CLI's `voice-change --preset-pack` requires `--preset`, matching the native CLI. It previously fell back to the pack's first entry, so which preset a pack-only invocation rendered depended on the file's ordering; that invocation is now rejected with the invalid-parameter code.

## v1.6.0 (2026-08-03)

**This release contains source-incompatible changes.** Two of them change behaviour without raising an error, so existing code keeps running with a different meaning — read Behavioural changes before upgrading:

- Project automation lanes are identified by their target parameter id instead of a positional index. The add, edit and remove calls keep the same arity and argument types on every surface, so an existing index-based call still runs and edits a different lane.
- `Audio#getData()` on Node and `Audio.data` on WASM return a copy. Code that wrote into the returned `Float32Array` to edit the audio in place no longer has any effect.

This release adds a machine-readable capability catalog, cooperative cancellation for long-running offline calls, a dedicated WebAssembly analysis bundle and Worker entry point, project-level stem import and flat model read-back, GM program following in the built-in synth, and before/after mastering reports. It also corrects oversampled mastering continuity across block boundaries, the voice changer's latency and limiting, and a set of analysis, mixing and MIDI defects.

### New surfaces

- A machine-readable capability catalog describes every processor, its parameters with bounds and defaults, and the built-in preset lists. It is published as canonical JSON through the C ABI and mirrored as `capabilityCatalog` (Node, WASM) and `capability_catalog` (Python), validated against `schemas/capability-catalog.schema.json`, and attached to the release. Unknown parameter bounds are reported as explicit nulls rather than invented ranges. A companion build-diagnostics report is exposed as `capabilities` on every surface and as a `doctor` command on both CLIs.
- Long-running offline analysis and mastering calls accept cooperative cancellation at their existing progress boundaries. The C ABI adds `SonareCancelCallback` and cancellable entry points; the facades take a `cancel` callback (`cancel?: () => boolean` on Node and WASM, `cancel=` on Python) and report `SONARE_ERROR_CANCELLED` / error code 8. A cancelled call leaves its outputs unallocated.
- The mastering chain reports before and after loudness, peak, range, gain-reduction and 32-band energy summaries, mirrored across the C ABI, ctypes, Node, Python, WASM and both CLI report files.
- `StreamingMasteringChain::flush()` emits the chain latency plus finite processor tails after the final input block, exposed as `sonare_streaming_mastering_chain_flush_mono` / `_stereo` and as `flushMono` / `flushStereo` on Node, Python and WASM.
- Monophonic note segmentation from F0 tracks is available as `sonare_note_segments` with a versioned `SonareNoteSegmenterConfig`, mirrored on all three bindings. `F0Track` gained an explicit `frame_rate_hz` cadence for host-supplied tracks.
- Ranked tempo and meter hypotheses are retained on the analysis result across the C ABI, Node, Python and WASM.
- The synthesis, segmentation and pitch families reached the C ABI: tone, chirp and click generation, Griffin-Lim, mel delta, piptrack, the reassigned spectrogram, spectral bandwidth with a configurable Minkowski exponent, spectral flux, onset backtracking, cross-similarity, recurrence matrices, recurrence/lag conversion, subsegmentation, agglomerative clustering and path enhancement. Onset detection takes an explicit config struct covering FFT size, peak-picking window, delta, wait and backtracking. All of it is mirrored as request objects on JS and keyword arguments on Python.
- Projects can import host-separated PCM stems through `sonare_project_import_external_stems`, turning already-separated stems into one audio track and clip each. The import is all-or-nothing and performs no resampling, retiming or gain compensation. An optional per-source `external_stem_role` round-trips through the serializer.
- Projects expose read-only `SonareProjectTrack` / `SonareProjectClip` / `SonareProjectSource` descriptors with by-index readers, a full UTF-8 marker name accessor, and `sonare_project_set_source_audio` so a host can rebind decoded PCM to a loaded project before bounce.
- The realtime engine gained `sonare_engine_prepare_with_channels`, so prepare reserves capture, instrument, PDC and monitor planes for the host's real channel count instead of always 64, plus `flush_control_commands` for control-only hosts.
- The mixer compensates untouched planes for a latent stereo-pair-only insert, and `ChannelStripConfig::enable_metering` lets strips whose snapshots are never read drop their meters entirely.
- Voice changer presets accept a `macros` shorthand covering pitch, formant, brightness, space, intensity, noise control and sibilance. Macros are an input-only convenience expanded into the ordinary DSP config by the shared parser on the control thread; an explicit `dsp` section always wins, and macros never appear in normalized output.

### Analysis

- Onset-envelope centering was extracted into a shared helper and applied in the music analyzer, whose beats now match the direct beat detector.
- `piptrack` matches librosa's local-maximum edge behaviour, `spectral_contrast` sanitizes non-finite magnitudes before sorting, CQT and VQT kernels whose top centre frequency reaches Nyquist are rejected, and a negative `top_n` is clamped in the key analyzer.
- `get_window_cached()` returns a shared handle, so bounded-cache eviction cannot dangle a window still in use.
- Multichannel PCM can be loaded without downmix through `load_audio_interleaved()`. WAV writes past the RIFF 32-bit size limit are rejected, and the synthesis generators, gain/RMS normalization and fades validate non-finite or oversized arguments.

### Mastering and mixing

- Oversampled processing is continuous across block boundaries. A shared Kaiser polyphase FIR design replaces three separate copies, the oversampler carries per-channel FIR history between blocks and reports its round-trip latency, and the true-peak limiter, tape, tube and air band moved onto that stateful path. The air band smooths its harmonic normalization per sample, making its output block-size independent.
- Velvet reverb is split into a direct early partition and an FFT-partitioned tail with bounded reverb time and tap count.
- Declick detection and interpolation share one Burg LPC model, the declip solver runs on a bounded local context, denoise input shorter than `n_fft` is rejected, and short dereverb input is zero-padded instead of silently switching algorithm.
- Surround buses exclude the LFE plane from linked compressor, gate, limiter and sidechain detectors while still applying linked gain to every output plane.
- The offline true-peak limiter stages report minimum gain reduction, so a zero-tail drain block no longer zeroes the reported program gain reduction. Stereo loudness is measured once and reused for both the requested and ceiling-clamped gain.
- The processor catalog carries a coarse `realtimeCost` tier, required to be non-null exactly for realtime-insertable ids.
- Out-of-range denoise mode and noise-estimator values are rejected rather than cast into the enum, and a chain with no enabled stages reports progress completion.

### Realtime engine, project and MIDI

- Automation lanes are identified by their target parameter id rather than a positional index that silently retargeted after an insert or removal; a track holds at most one lane per target.
- Clip and track removal is an undoable transaction that also drops the sources they orphan together with the decoded PCM those sources own. Clip split and trim are atomic on failure, warped clips are rejected, and warp maps require at least two strictly increasing anchors.
- Meter and scope records are staged at most once per target per host block, track-mixer sources accumulate into cleared lanes, and routing-graph latency folds into reported PDC and refreshes on graph swap.
- A malformed control command or sync message is contained as telemetry or a `syncError` message so it cannot escape `process()` and stop the audio thread. Node engine capture buffers are copied into addon-owned storage instead of retaining pointers into detachable JS ArrayBuffers.
- The built-in synth follows GM programs: melodic channels resolve their voice from the tracked bank and program change, and channel 10 routes through the GM drum-kit map. The configured patch remains the fallback, and fixed-patch behaviour is unchanged when the mode is off. SF2 player config version 2 adds `prefer_model_for_modeled_families`, routing covered melodic programs to the dedicated physical model while keeping drums SoundFont-first.
- One shared destination voice pool can render into per-source-track lanes instead of duplicating voices, used by the channel-strip project bounce.
- SMF import skips non-MTrk chunks instead of failing, drops set-tempo values outside the public BPM range, and marks a truncated file that still carries valid content so import installs the recovered prefix.
- Project scene JSON is encoded and decoded by one canonical schema walker regardless of the mixing build flag, with one stable key order and one set of accepted key spellings.

### WebAssembly

- A dedicated analysis-only module is published as the `@libraz/libsonare/analysis` entry, built without mastering, mixing, realtime or project bindings, with a size budget enforced in CI.
- One-shot analysis and mastering calls can run in a dedicated Worker through `OfflineWorkerClient`, published as the `@libraz/libsonare/worker` entry. `Float32Array` inputs are transferred by default with an explicit copy option, and tasks are cancellable.
- The AudioWorklet reports clip-page misses through a lock-free SPSC request ring instead of embind calls or postMessage traffic from `process()`, with `attachOpfsClipStream` wiring the OPFS path end to end.
- Telemetry, meter, scope and external MIDI drain through scalar scratch accessors, 64-bit ring fields are stored as word pairs instead of BigInt, and a worklet-to-main external MIDI ring was added.
- `pushMidiUmp` accepts a single-word MIDI 1.0 channel-voice UMP, dispatched immediately to restore program, pitch bend and pressure state on transport seek.
- Both voice-changer preset JSON Schemas ship in the published package.

### Command-line tools

- The native executable is published as `sonare-cli` in FFmpeg-free Linux and macOS release archives with SHA-256 checksums, so it can coexist with the Python `sonare` command.
- Every command has its own help listing the options it accepts, and the global DSP options are accepted only by the commands that consume them. ANSI colour is configured once at startup, so `NO_COLOR` and any redirected stream disable it.
- `mix` is renamed `mix-strip` with the old name kept as an alias, and it now loads and writes true stereo so `--width` is no longer a no-op. The fourth-order filter path runs through filtfilt only under `--zero-phase`.
- Project bounce writes the requested channel count, and `project validate --strict` and `project synth-presets` were added. The bare `--synth` flag follows GM programs with channel-10 drums.
- The two CLIs emit the same JSON for the same command, with snake_case keys throughout.
- Room-impulse-response errors and warnings are published through the C ABI as stable diagnostic codes, surfaced by the Python result object and the native CLI.

### Bug fixes

- Every C-ABI getter that builds a `thread_local` string, and the EQ and scene-JSON factories, return null with a diagnostic on allocation failure instead of escaping the ABI boundary; the two audio-thread process entries stay free of `thread_local` diagnostics. Quick-analysis output arrays are staged in temporary owners so a later allocation failure cannot leak the arrays already built.
- One shared error-code table backs the C ABI, Node and WASM instead of per-binding copies, including the previously missing cancellation mapping.
- The voice changer moves every live control onto per-sample smoothers, so adopting a config snapshot no longer steps values at a block boundary. The retune and whole-chain dry paths align to the overlap-add latency, so reported latency is fixed instead of scaling with the wet and retune mixes. The inter-sample-peak limiter gained a delayed detector with attack-lead gain compensation and no longer hard-clips base-rate samples, which recreated the peaks it was meant to prevent. Only the formant frequency displacement scales with the formant amount, so body, brightness and nasal still apply at amount zero.
- Preset ids, complete schema documents and the flat camelCase POD route through one shared parser used by the C ABI, Node and WASM, rejecting partial documents that previously fell back to unrelated defaults.
- The CoreAudio render callback zeroes the active output scratch before each block; a callback that only called the engine replayed the previous block's samples. The CoreMIDI SysEx staging ring uses release/acquire single-producer single-consumer cursors instead of a mutex, so the MIDI callback performs only bounded copies and can never block behind the control thread.
- Alignment delay is bounded by a named maximum applied to both the integer and Q8 setters, and out-of-range requests are rejected at the C-API channel-delay setter instead of silently clamped.
- The wah and auto-wah sweep is clamped below the SVF stability limit, and `frequency_to_w0` no longer inverts its clamp at low sample rates.
- Engine SysEx payloads are bounded at 512 bytes on both the C ABI and the WASM path.

### Behavioural changes

- Project automation lanes are addressed by target parameter id: `sonare_project_add_automation_lane` reports the id through `out_target_param_id`, and the edit and remove calls take `target_param_id` where they previously took `lane_index`. Node, WASM and Python changed with them. The argument count and type are unchanged, so an existing index-based call still runs and operates on a different lane; changing a lane's identity now requires remove then add.
- `Audio#getData()` on Node and `Audio.data` on WASM return a copy, so the internal snapshot the facade methods read cannot be mutated through the returned array. In-place edits to the returned `Float32Array` no longer affect later calls, and each call allocates.
- Node and WASM reject inputs they previously coerced: wrong-typed repair and dynamics options, an unknown track kind, capture source or pitch-correction mode, a negative spectrum setting, enum spellings and ordinals that are not declared, and override values that are neither number nor boolean. Instance methods reject after `destroy()`.
- Voice-changer preset documents must be complete; a partial document is rejected instead of falling back to unrelated defaults, and `deesser.ratio` is required. The `macros` shorthand maps its 0–1 inputs onto each target's valid range, so `macros.space` reaches the reverb mix ceiling of 0.45 at 1.0 rather than writing an out-of-range value.
- Project and scene JSON reject a non-finite or out-of-range number with its field path instead of silently truncating it, and `channelDelaySamples` is bounded by the alignment-delay maximum rather than only rejecting negatives.
- Both CLIs turn silent no-ops into errors: `--semitones` and `--rate` are required where they had inert defaults, an unknown pitch algorithm or pitch-correction mode is rejected, a reference sample-rate mismatch is an error instead of a quiet resample, and a global DSP option passed to a command that does not consume it exits with the invalid-parameter code. CLI JSON values are no longer rounded before serialization.
- Mastering, mixing and voice-changer output changed with the fixes above: oversampled stages are continuous across blocks, surround beds are latency-compensated, the LFE plane is excluded from linked detectors, and the voice changer's reported latency is fixed rather than mix-dependent. The mastering preset golden hashes were regenerated for the new output.
- Analysis results shift where they were wrong: music-analyzer beats now match the direct beat detector, and `piptrack`, `spectral_contrast` and the CQT/VQT kernels changed as described above.
- Windows CMake configurations are rejected with a pointer to WSL2.

### Platform support

- Linux, macOS, WebAssembly and WSL2 are the declared supported platforms.
- Linux wheels are built inside matching manylinux 2.28 images, repaired with auditwheel, and checked against glibc 2.31; macOS targets 11.0.
- The native Node binding is marked private and is installed as a local dependency only. The published artifacts are the WebAssembly npm package, the Python wheel and the native CLI release archives.

## v1.5.5 (2026-07-27)

This release corrects a set of DSP and analysis defects across the surround, decode, mastering, metering and realtime paths, brings the pitch, constant-Q and rhythm transforms back in line with librosa, and exposes the core capabilities that had no binding entry point. Several analysis defaults and one JavaScript positional signature change with it — see Behavioural changes before upgrading.

### New surfaces

- Configuration-taking variants of the one-shot analysis entry points are available on every surface: `sonare_analyze_json_ex` (seeded by `sonare_music_analyze_options_default`), `sonare_chroma_cens_ex`, `sonare_chroma_cqt_ex`, `sonare_nnls_chroma_ex`, `sonare_mfcc_to_mel_ex` and `sonare_mfcc_to_audio_ex2` make the music-analyzer options, chroma bins-per-octave, NNLS STFT blending and the forward MFCC lifter configurable. The existing entry points forward with their previous defaults, so current calls are unaffected. Node and WASM expose them as additional request fields on `analyze`, `chromaCens`, `chromaCqt`, `nnlsChroma`, `mfccToMel` and `mfccToAudio`; Python takes them as keyword arguments.
- Silence-ratio metering is exposed as `sonare_metering_silence_ratio`, mirrored as `meteringSilenceRatio` (Node, WASM) and `metering_silence_ratio` (Python).
- The mixer gained compiled-bus metering and VCA group membership replacement — `sonare_mixer_bus_meter` and `sonare_mixer_set_vca_group_members` — mirrored as `busMeter` / `setVcaGroupMembers` (Node, WASM) and `bus_meter` / `set_vca_group_members` (Python).
- The realtime engine accepts a full MIDI CC binding descriptor covering 7-bit, 14-bit, RPN and NRPN controllers through `sonare_engine_bind_midi_cc_binding`, mirrored as `bindMidiCcBinding` (Node, WASM) and `bind_midi_cc_binding` (Python).
- Every LUFS result reports the EBU R128 maximum momentary and maximum short-term loudness alongside the final windows, and mastering results flag `loudness_target_limited` when the true-peak ceiling prevented reaching the requested loudness target, across the C ABI, Node, Python and WASM.
- The analysis result carries the core's canonical chord root and bass names on every surface, and Node types the mastering chain configuration concretely instead of a generic nested section.
- The Python CLI gained `pitch-correct-timevarying`, `note-move` and `scale-quantize` subcommands.

### Analysis compatibility

- YIN and pYIN follow librosa's difference function, threshold sweep, trough selection and center padding. Voicing is reported from the threshold crossing while the global-minimum period is still returned, so an unvoiced frame keeps a usable f0 estimate instead of a placeholder.
- `tempogram` uses librosa's zero-ended linear ramp padding and `fourier_tempogram` zero padding, and the centered onset frame offset is taken by floor division.
- `pseudo_cqt` applies its per-bin length scaling internally, so `hybrid_cqt` no longer applies that scaling to the pseudo half a second time. `chroma_cqt` gates on absolute CQT magnitude rather than a fraction of the per-frame maximum.
- VQT selects librosa's ERB-derived automatic gamma when `gamma` is negative or NaN.
- Section analysis runs at a fixed 22.05 kHz and merges short sections into their neighbours, so results no longer shift with the source sample rate. Meter detection normalizes beat strengths and derives the audio-backed time signature from beat-local low-frequency energy, so an onset envelope above unity no longer changes the reported time signature.
- Chord spelling prefers parallel-mode and flat Roman numerals over enharmonic sharps, and the final beat-synchronous chord ends at the chroma duration.

### Bug fixes

- The 7.1 speaker-role table is reordered to `L R C LFE Ls Rs Lss Rss`, matching the `WAVE_FORMAT_EXTENSIBLE` `0x63F` mask the writer already emitted. The side and back pairs were swapped in the plane roles, the downmix folds, the surround panner and the BS.1770 surround weighting.
- FFmpeg decoding configures the resampler from the first decoded frame and rebuilds it when a stream renegotiates rate, format or layout, flushing the resampler delay first. Implicit HE-AAC streams advertise provisional stream parameters and previously decoded at half their real sample rate. `audio_channel_count` now reports the source channel count for containers only FFmpeg can open, instead of returning zero.
- True-peak interpolation is a centered convolution — the polyphase taps were in reverse order — phase 0 is included in the search, and the sample peak is folded into the reported value. The limiter re-applies its ceiling after decimation so the output ceiling stays a hard invariant, and an unsupported oversample factor is rejected when the chain or a flat parameter set is built rather than silently rounded down at measurement time.
- Linkwitz-Riley allpass compensation is applied once per split instead of once per duplicated section, removing the recombination notches around the crossovers of three-way and wider splits.
- The stereo imager and the multiband imager use a signal-independent constant-power width gain in place of a per-sample mid/side energy ratio, so widening no longer injects intermodulation products; a non-finite width is rejected.
- The air band is rebuilt around a 4x oversampled band-limited waveshaper with a control-rate interpolated shelf and an RMS-bounded harmonic level, so its added harmonics no longer track the sample rate and its aliases stay suppressed. The exciter's even-harmonic branch is a true even function with a per-channel DC blocker, and the mono maker collapses low frequencies through a crossover with a `frequencyHz` parameter.
- Spectrum magnitudes are scaled to one-sided amplitude, so a full-scale sine reads 0 dBFS regardless of FFT size, and the dB output is clamped to the shared floor. The scope decimation bucket boundary is computed beyond 32-bit `size_t`, so a long buffer cannot wrap.
- MIDI editing no longer discards user data: MIDI-FX baking processes bounded chunks instead of stopping past a fixed event count, same-timestamp events keep a canonical order, the exported clip window is validated, and a truncated SMF track is reported as truncated instead of ok.
- `pitchCorrectToMidi` applies the full requested transposition. It previously applied only a fraction of it and varied with the sample rate; it now routes through the core constant-transpose path, which preserves the input length.
- Realtime dispatch is ordered and bounded: clip, loop and pending MIDI-FX events are merged by render frame, so an arpeggiator or chord step can no longer leapfrog an earlier event from another clip; a quantized or humanized note-off reuses its note-on's frame shift and stays strictly after it, so a short note cannot collapse to off-before-on; MIDI clock stops scanning a block once its budget is reached; and both the clock and metronome overflows surface as telemetry error codes. Tempo values above 100000 BPM are rejected on the public control plane.
- The metronome renders after metering, scope capture and output capture, only while the transport is playing, and is disabled during offline render, so the cue click stays out of recorded program audio.
- Smoothed parameter and insert-automation slots keep their identity after settling and reset to their first value when newly claimed, so retargeting no longer glides from an unrelated parameter's last value. The record offset is applied to punch boundaries and the capture sink with saturating arithmetic, and the master scope is captured once per host block so an automation split no longer changes spectrum resolution.
- A moved note region fades its tail down instead of reapplying the fade-in curve, which left a dropout and a click at the note offset. Adjacent spectral-edit regions no longer share a boundary frame, which made their combined result depend on application order, and the streaming phase vocoder keeps the two retained frames its interpolation reads.
- Arguments that previously produced malformed output are rejected: odd or zero FFT sizes, non-positive peak-pick post windows, a single-frequency `wavelet_lengths` call with no explicit Q, a positive normalize target while clipping is enabled, negative pad, fix-length and clipping-region sizes, and a pitch track whose voiced array length does not match `f0Hz`. An empty audio slice and an empty resample result keep their requested sample rate instead of returning a rate-zero `Audio`.
- The WASM path routes MIDI-FX JSON through the shared parser and streaming-chain configuration through the canonical flattener, and validates non-finite samples, negative sizes, out-of-range scale masks, metronome settings and clipping-region lengths in the native layer, so it no longer bypasses the C-ABI guards. `Audio.fromBuffer` copies and validates its buffer.
- The compiled mixing graph applies the bus input trim, polarity and stereo width, keeps the absolute automation position across a recompile, and carries surround pan into the live strip. A non-monotonic automation timestamp is distinguishable from an exhausted lane.

### Performance

- CQT and VQT build a row-compressed sparse kernel by cumulative-L1 pruning instead of dense matrices, with explicit bounds on kernel elements, FFT length and inverse-transform inputs.
- WAV decoding runs in bounded chunks and downmixes to mono in the same pass, dropping the full interleaved intermediate buffer. The IIR filterbank streams framed RMS through a bounded ring, NNLS solves all right-hand sides together with a projected FISTA that reuses `AtA` / `AtB`, and the inverse DCT writes into a caller-owned buffer.
- The Node `Audio` PCM snapshot is cached, so the convenience accessors stop copying the whole buffer across N-API on every call, and the Python decode path avoids an intermediate copy of encoded buffers.
- The compressor's program-dependent release coefficients are precomputed and the limiter's adaptive release refreshes at a control interval. Lane faders and bus gains are smoothed in the linear domain, only the delay lanes the host uses are prepared, and the meter's K-weighted energy history is stored as float while its running sums stay double.

### Behavioural changes

- The `phaseVocoder` positional signature on Node and WASM is `(samples, sampleRate, rate, nFft, hopLength)`; `sampleRate` and `rate` were previously the other way round. Both are numbers, so an existing positional call still type-checks and will silently pass the wrong values. Pass a request object, or swap the two arguments.
- The 7.1 plane order changed to `L R C LFE Ls Rs Lss Rss`. Callers that compensated for the previous swapped side/back order must drop that compensation.
- Analysis defaults changed to match librosa: the YIN and pYIN voicing threshold is `0.1` (was `0.3`), VQT `gamma` defaults to `-1.0` for the automatic ERB-derived value (was `0.0`, standard CQT), and `chroma_cqt` analyses 252 CQT bins at 36 bins per octave (was 84 bins at 12). `chroma_cqt`'s `threshold` is now an absolute magnitude rather than a fraction of the per-frame maximum, so an existing non-zero value means something different.
- Other defaults moved to match the core: the peak-pick trailing windows `postMax` and `postAvg` default to `1` (was `0`, which suppressed the trailing comparison), note-edit offsets default to the input length, and the metronome click length is derived from the sample rate when left at zero.
- Node and WASM reject inputs they previously accepted, matching the C ABI: negative pad, fix-length and clipping-region sizes, non-finite samples, out-of-range scale masks, and pitch tracks whose voiced array length does not match `f0Hz`.
- The Node pre-chorus section label is spelled `Pre-Chorus` instead of `PreChorus`, so consumers matching on the previous spelling need updating.

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

## v1.5.3 (2026-07-21)

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
