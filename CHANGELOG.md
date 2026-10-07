# Changelog

## Unreleased

### Upgrade notes

#### Rebuild

- Every binding checks the loaded native module's ABI version when it loads and refuses a mismatch with the new error code `AbiMismatch` (10): Node when the addon is required, WASM in `init()`, Python on first library use (the Python CLI exits with 13). The code is binding-only; the core never reports it.

- `SONARE_FEATURE_ABI_VERSION` is 7 and `SonareBoundaryOptions` grows from 40 to 44 bytes with the new `reference_window` field, so C callers rebuild; a caller that starts from `sonare_boundary_options_default()` gets the field's default.

- Within the same `SONARE_FEATURE_ABI_VERSION` 7, six more shipped structs change layout, so C callers rebuild: `SonareMusicAnalyzeOptions`, `SonareChordDetectionOptions`, `SonareChordAnalysisResult`, `SonareMelResult`, `SonareMfccResult` and `SonareTranscribeResult` gain a leading `struct_version` and the fields listed under New; `SonareTranscribeConfig` is version 2 with a trailing `reference_auto` (version 1 is still read). `sonare_analyze_json_ex` refuses a `SonareMusicAnalyzeOptions` whose `struct_version` is not `SONARE_MUSIC_ANALYZE_OPTIONS_VERSION`, so start from `sonare_music_analyze_options_default()`; the chord options take 0 or the current version. The libraries fill the result structs' `struct_version` themselves.

#### Now refused

- `sonare_project_add_loop_recording_takes` refuses a `SonareProjectLoopRecordingDesc.flags` word with any bit other than `SONARE_PROJECT_LOOP_RECORDING_DROP_PARTIAL_TAIL`; the word was reserved and ignored before, so C callers zero it.
- `sonare_engine_set_track_strip_json`, `sonare_engine_set_bus_strip_json` and `sonare_engine_set_master_strip_json` (and the Node, WASM and Python setters over them) refuse a key the scene reader does not consume with InvalidParameter naming it, e.g. `unknown strip key 'strips[0].faderDB'`; keys starting with `$` or `x-` and the legacy snake_case aliases are still accepted, and a misspelled key used to be ignored.
- `sonare_eq_set_band` and the strip EQ band setters (`sonare_engine_set_track_strip_eq_band_json`, `sonare_engine_set_bus_strip_eq_band_json`, `sonare_engine_set_master_strip_eq_band_json`, and the Node, WASM and Python setters over them) refuse a key the band reader does not consume with InvalidParameter naming it, e.g. `unknown band key 'gainDB'`; keys starting with `$` or `x-` and the snake_case spellings are still accepted, and a misspelled key used to be ignored.
- A track lane whose source channel layout is not stereo is refused with InvalidParameter; mono, 5.1 and 7.1 were accepted but rendered as stereo. A zero-initialised C `SonareEngineTrackLane` reads as mono, so C callers set `SONARE_CHANNEL_LAYOUT_STEREO` (`sonare_engine_set_track_lanes`, `setTrackLanes`, `set_track_lanes`).
- An offline render, bounce, freeze or pre-roll whose block size is larger than the block size the engine was prepared with is refused with InvalidParameter (`sonare_engine_render_offline`, `sonare_engine_render_offline_ex`, `sonare_engine_bounce_offline`, `sonare_engine_freeze_offline`, `sonare_engine_prime_offline_parameters`, and the Node, WASM and Python methods over them); the render used to run silently at the prepared block size. The WASM worklet facade refuses it with a `RangeError`.
- An explicit polyphony `nFft` is rescaled with the input rate, so a size that fit at 44.1 kHz can exceed the STFT limit at a much higher rate and is refused.
- Repair stages refuse a value above a new upper bound, on every surface and in the named-processor path (`InvalidParameter` / `SONARE_ERROR_INVALID_PARAMETER`): declick `threshold` above 10, `neighborRatio` above 100, `maxClickSamples` above 512 (the longest gap the AR fill solves), `lpcOrder` above 36 and `residualRatio` above 1000; declip `lpcOrder` above 36 and `iterations` above 8; decrackle `threshold` above 1000 and `levels` above 24; dehum `fundamentalHz` above 5000, `q` above 100, `searchRangeHz` above 100, `frameSize` above 16384 and `pllBandwidth` above 1; denoise `reductionDb` above 120; dereverb `t60Sec` above 10 and `lateDelayMs` above 500; trim silence `threshold` above 1, `paddingSamples` above 960000 (it was SIZE_MAX/2), `windowMs` above 10000 and `gateLufs` outside [-144, 0]. Each bound is the largest value the stage is exercised at or the limit its own algorithm states.

#### Error classes

- A failure that comes from the library or from an object's state is a `SonareError` carrying the C-ABI code on every surface. On Node and WASM a released, destroyed or uninitialised handle, including a WASM embind object already deleted, throws `SonareError` with `InvalidState` where it threw a plain `Error` or `TypeError`; in Python the bare `RuntimeError`s became `SonareError` (still a `RuntimeError` subclass) with `NOT_SUPPORTED`, `INVALID_STATE` or `UNKNOWN`.
- Two buffers whose lengths must match are refused with a `RangeError` on Node and WASM, where some paths threw `TypeError` or a plain `Error`.
- The streaming `ChordChange` / `BarChord` constructors take `name` after `quality`.

### New

#### Errors and lifetime

- Read the code recorded beside the last error message, so a function that returns a NULL handle reports why (C: `sonare_last_error_code`). Node, WASM and Python use it for the mixer scene load, strip creation and the streaming mastering constructors.
- Release every WASM handle class with `using` (`[Symbol.dispose]`), as on Node; `ProjectTimeline`, `ClipPageProvider` and `ClipPageStreamer` gain the `delete` / `destroy` aliases.

#### Analysis

- Name the streaming key and chords: the progressive estimate carries `keyName`, `keyShortName` and `chordName`, and each chord change and bar chord a `name`, spelled by the same functions batch analysis uses (Python: `key_name`, `key_short_name`, `chord_name`, `name`). No chord reads `N.C.`; an unknown key has no name.

- Set the window the boundary detector's relative threshold is measured in with `referenceWindow` (Python: `reference_window`; CLI: `--reference-window`; C: `SonareBoundaryOptions.reference_window`). It is one-sided in seconds and defaults to 60; 0 disables the relative threshold.

- Measure the recording's tuning with `tuning: 'auto'` (Python: `tuning="auto"`; C: `tuning_auto`) on `analyze`, `detectChords` and `detectKey`, and `referenceHz: 'auto'` (Python: `reference_hz="auto"`; C: `SonareTranscribeConfig.reference_auto`) on `transcribe` and `transcribeToClip`. `tuning` is one unit everywhere, a fraction of a semitone in `[-0.5, 0.5)`, and every result reports the value used, given or measured: `AnalysisResult.tuning`, `ChordAnalysisResult.tuning`, `detectKey`'s new `KeyDetection.tuning` (Python: `KeyDetection`, a `Key` subclass), `TranscribeResult.tuning` (C: the JSON `tuning` key, `SonareChordAnalysisResult.tuning`, `SonareTranscribeResult.tuning`, and `sonare_detect_key_with_tuning`, which reports it through `out_tuning`). `estimateTuning` stays the librosa mirror and returns a fraction of a bin of its `binsPerOctave`; it equals the analysis unit at 12.

- Convert between a reference pitch and the analysis unit with `tuningToReferenceHz(tuning, a4 = 440)` and `referenceHzToTuning(hz, a4 = 440)` (Python: `tuning_to_reference_hz`, `reference_hz_to_tuning`; C: `sonare_tuning_to_reference_hz`, `sonare_reference_hz_to_tuning`). The live `tuningRefHz` of the stream analyzer and `referenceHz` of transcription stay in Hz.

- Mel and MFCC results carry the parameters of their forward transform (`nFft`, `fmin`, `fmax` as applied, `htk`, `isDb`; MFCC also `sampleRate`, `hopLength`, `nMels` and `lifter`), so a result is enough to invert it: the Node and WASM inverse requests take `{ result, ...overrides }` (`melToStft`, `melToAudio`, `mfccToMel`, `mfccToAudio`), refusing with a `RangeError` an explicit field that disagrees with the result and a result in dB with a message naming `dbToPower`; the existing `mel` / `melPower` request forms are unchanged. Python adds `mel_result_to_stft`, `mel_result_to_audio`, `mfcc_result_to_mel` and `mfcc_result_to_audio`; the positional librosa-mirror inverses are untouched. C adds the length-checked, lifter-aware `sonare_mfcc_to_mel_checked_ex` and `sonare_mfcc_to_audio_checked_ex2`.

#### Project

- List unresolved audio sources as full descriptors and read an audio source's URI untruncated (C: `sonare_project_unresolved_audio_source_by_index`, `sonare_project_get_audio_source_uri`; Node and WASM: `Project.unresolvedAudioSources`; Python: `unresolved_audio_sources`). `sourceByIndex` / `source_by_index` now return the full URI.
- Loop recording takes accept planar audio (one array per channel) on Node, WASM and Python, and read whether a take is partial from the take itself: a partial last take carries `"partial": true` in its `takes[]` entry of the project JSON, which round-trips. `partialTail: 'drop'` (C: `SONARE_PROJECT_LOOP_RECORDING_DROP_PARTIAL_TAIL`, the former reserved word of `SonareProjectLoopRecordingDesc`, now `flags`) leaves the partial take out. The project ABI counter does not move: the add call's signature and the struct layout are unchanged, and an unknown flag bit is refused.
- Drive a project bounce from JavaScript instruments on Node and WASM: `project.bounceWithInstruments([{ destinationId, prepare(sampleRate, maxBlockFrames, channels), onEvent({ destinationId, words, renderFrame }), render(outputs, frames) }], options)` (and `bounceWithInstrument` for one), the callback form of the C ABI's `sonare_project_bounce_with_instruments` that Python already had. Callbacks run synchronously on the calling thread; `outputs` are zero-filled scratch arrays the instrument adds into, summed into the bounce after `render` returns. A throw stops further callbacks and is rethrown after the bounce (a native failure is reported first, as in Python); a callback returning a Promise is refused with `TypeError`, and using the same project from inside a callback is refused with `InvalidState`. On Node the bounce runs on a large-stack thread with the callbacks handed back to the JS thread, because the offline render holds nearly all of V8's stack.

#### Mastering

- The insert parameter descriptor states whether each bound is exclusive (`minExclusive`, `maxExclusive`) and marks a ceiling bounded by the processing rate's Nyquist (`maxRelativeTo: "nyquist"`; the effective ceiling is the lower of `max` and the Nyquist).
- The insert parameter descriptor declares what a control needs beyond the accepted range: `unit` is read from the processor that consumes the key, from a closed set (`dB`, `dBFS`, `LUFS`, `Hz`, `ms`, `s`, `samples`, `m`, `cm`, `deg`, `percent`, `degC`, `V`, `inPerSec`, `dBPerOct`, `semitones`, `cents`, `ratio`, `bits`, `count`, `none`) and is no longer derived from the key's spelling, so a key such as `decaySec` or `lengthM` now carries one and `modDepthSamples` reports `samples`; `scale` (`linear` or `log`) names the axis; `uiMin` / `uiMax` give an optional display range inside `[min, max]`; `dependsOn` lists the sibling keys whose live value bounds this one (`min` / `max` stay measured with every sibling at its default). The display ranges of the common dynamics, saturation, stereo and maximizer controls are declared in the library. All of these describe construction-time acceptance; the realtime parameter path clamps. Node and WASM type `unit`, `scale` and the `dependsOn` relation as unions (`MasteringInsertParamUnit`, `MasteringInsertParamScale`, `MasteringInsertParamRelation`), and Python types the descriptor as `MasteringInsertParamInfo` with `MasteringInsertParamDependency` and the same three as `Literal`s.
- Resolve a Nyquist-bounded ceiling for a given processing rate: `masteringInsertParamInfo(name, sampleRate)` on Node and WASM, `mastering_insert_param_info(name, sample_rate=...)` in Python (C: `sonare_mastering_insert_param_info_at_rate`) report the bound the insert accepts at that rate.
- Every `repair.*` entry of the capability catalog publishes its parameters through the same measured descriptor as an insert (`unit`, `scale`, `uiMin` / `uiMax`, `dependsOn`, enum `choices`), measured through the stage's own configuration validation; the entries carry no automation `id` and are never `rtSafe`. `denoiseClassical` and `dereverbClassical` declare `hopLength` against `nFft`; `nFft` and `hopLength` publish no `min` / `max`, since only power-of-two sizes are accepted.
- Every catalog entry carries `causal` (C JSON, Node, WASM and Python types, the schema): false for `repair.declick`, `repair.declip` and `repair.trimSilence`, true for the other repair stages and every insert.

#### Packaging

- The WASM package resolves every entry under the `default` condition and TypeScript's node10 resolution, exports the analysis-only module as `./wasm/analysis`, and its glue no longer makes webpack or esbuild fail on `node:module`.
- The installed pkg-config file resolves its prefix relative to itself, and the exported CMake targets require C++17.

#### Realtime engine and project

- Bounce, freeze and the request form of `renderOffline({ totalFrames, blockSize?, finalize? })` on the WASM worklet `SonareEngine` (`bounceOffline`, `freezeOffline`), with the options and results of the raw engine, plus `finishOfflineRender()` for chunked renders. They run on the facade's main-thread offline engine and block the calling thread for the whole render; the offline engine's transport position is restored afterwards, OPFS-streamed clips are paged in for the span first, and a freeze leaves the facade, the offline engine and the worklet holding only the frozen clip. The WASM `RealtimeEngine` reads a frozen clip's audio back through `prebakedClipChannels`.
- Analyze an audio graph branch live with `SonareStreamAnalyzerNode` in `@libraz/sonare/worklet` (`create(context, { config?, chunkFrames? })`, `ready`, `onFrames`, `onStats`, `destroy`), with `registerSonareStreamAnalyzerWorkletProcessor()` for the worklet module. The worklet side only averages the channels to mono and forwards `chunkFrames` (default 4096) sample chunks through a fixed buffer pool; the `StreamAnalyzer` runs on the main thread, needs no WASM in the worklet realm and no SharedArrayBuffer. A dropped chunk or processor restart resets the analyzer's sample offset instead of splicing, and the node has no output.
- Run the offline pre-roll of bounce and freeze without rendering, so a host driving the engine block by block starts a repeat render from the same state: queued commands applied, mixer and effect processors reset, automation and lane gates resolved, smoothers snapped (C: `sonare_engine_prime_offline_parameters`; Node and raw WASM: `primeOfflineParameters`; Python: `prime_offline_parameters`).
- Queue a reset of every mixer and effect processor to its prepared state at a render frame, so playback queued after it starts from the state an offline bounce starts from; instruments are not reset (C: `sonare_engine_reset_processor_state`; Node, WASM and the worklet engine: `resetProcessorState`; Python: `reset_processor_state`).
- Read the engine's longest audible tail as an upper bound in samples, and its processing latency in 1/256 samples (C: `sonare_engine_tail_samples`, `sonare_engine_graph_latency_samples_q8`; Node, WASM and the worklet engine: `tailSamples`, `graphLatencySamplesQ8`; Python: `tail_samples`, `graph_latency_samples_q8`).

- Write an OPFS clip from the WASM package with `writeOpfsClip(path, channels)` (headerless little-endian interleaved float32, the layout the OPFS page readers and the Node file provider already read) and `importOpfsClip(path, source)` for an `ArrayBuffer`, `Blob` or `AudioBuffer`, which decodes and returns the source `sampleRate` without resampling. Encoded bytes (decoded through `decodeChannels`, with the browser codec fallback) and an `AudioBuffer` both keep their channels. Writes run in the OPFS worker, and a path an open page provider reads is refused with `InvalidState`.
- Decode audio bytes once and keep every channel, and fold channels to a narrower layout with the ITU-R BS.775 rule the decoders use: center and surround at -3 dB, LFE dropped (C: `sonare_decode_channels`, `sonare_downmix`; Node and WASM: `decodeChannels(bytes)` returning `{ sampleRate, channels }` and `downmix(channels, targetLayout)`; Python: `decode_channels(data)` returning `(channels, sample_rate)` with a `(channels, frames)` array, and `downmix(channels, target_layout)`). The functions are additive, so no ABI version moves.
- `SonareEngine.attachOpfsClipStream` no longer requires SharedArrayBuffer: on the postMessage path the worklet reads page misses without allocating and posts a request when the set of missing pages changes, re-posting an unchanged set every 250 ms so a failed read is retried. `capabilities.clipPageRequestsRealtimeSafe` still reports whether the SAB ring is in use.

#### Mixing

- Ask whether a lane, bus or master sidechain binding would be accepted, and why not, without changing anything (C: `sonare_engine_can_set_lane_sidechain`, `sonare_engine_can_set_bus_sidechain`, `sonare_engine_can_set_master_sidechain`, `SonareSidechainRefusal`; Node, WASM and the worklet engine: `canSetLaneSidechain`, `canSetBusSidechain`, `canSetMasterSidechain` returning `SidechainCheck`; Python: `can_set_lane_sidechain`, `can_set_bus_sidechain`, `can_set_master_sidechain` returning `SidechainCheck`).

### Behaviour changes

- Boundary detection gates each novelty peak against the largest novelty within `referenceWindow` seconds on either side instead of the whole-track maximum, so a dominant change no longer hides weaker section changes far from it. This affects tracks longer than 60 s and the sections `analyze()` reports. Minimum spacing keeps the strongest peaks first and no longer depends on peak order, so chains of peaks closer than `peakDistance` resolve differently. `strength` and `noveltyCurve` keep their meaning.
- Beat trimming measures the edge beats against the third-highest peak of the onset envelope instead of its single loudest frame, so one loud burst no longer trims quiet edge beats.
- Analyzers that take audio at its own sample rate (beats, tempo, onsets, key, rhythm, the chord beat grid, timbre) read `n_fft` as a window length in samples at 22050 Hz and rescale it to the input rate; the hop stays in input samples. Results at other rates can change and now agree across rates; 22050 Hz input is unchanged.
- The mastering audio profile and the mixing assistant read window and hop as samples at 48 kHz and rescale both to the input rate, and normalise band levels so they agree across rates; values at 48 kHz are unchanged.
- Chord segments end at the signal's duration instead of past it.
- A loop recording whose last loop is incomplete keeps the previous complete take active; the partial take is kept but not activated. A remainder of one frame or less no longer creates a take.
- Loading a mixing scene reports every key the reader does not consume in `sceneWarnings()` / `scene_warnings` (keys starting with `$` or `x-` are exempt, and the scene schema accepts them). `sonare_project_set_mixer_scene_json` reports them through `sonare_last_warning_message` and `Project.setMixerSceneJson` / `set_mixer_scene_json` return them as a list of strings (empty when the scene is clean), and `sonare_project_deserialize` reports those inside the embedded scene in `out_diag` as `unknown_scene_key` with a path from the document root, e.g. `scene.strips[2].faderDB`; the rest of the project document is not checked for unknown keys.
- A malformed scene document passed to the mixer is `InvalidFormat`.
- A call before WASM `init()` throws `SonareError` with `InvalidState`, and a second `init()` with different options throws `InvalidState` naming the option.
- Pre-fader sends of a lane silenced by mute or solo are silenced with it.
- A solo-safe lane keeps sounding while another lane is soloed.
- Bounce and freeze of a live engine reset its mixer and effect processors before rendering, cutting insert tails and delay lines that were still ringing.
- The WASM worklet engine delivers per-channel meters of surround (5.1, 7.1) targets live on both meter paths: the meter snapshot keeps its stereo fields and gains `channelCount` plus `peakDb`, `rmsDb`, `truePeakDb` and `inputPeakDb` arrays when the target has more than two channels. The SharedArrayBuffer meter ring record grows from 16 to 49 floats and its header slot 3 now carries protocol version 2, so a main thread and a worklet from different package versions must not share a ring.
- `sonare_engine_drain_meter_telemetry_wide_v2` (Node `drainMeterTelemetryWide`, Python `drain_meter_telemetry_wide`, WASM `drainMeterTelemetryWide`) is documented as the one meter drain, with a stereo target's left and right as planes 0 and 1; the other three drains stay and consume the same queue.
- The worklet engine's meter interval is kept per target instead of across all targets, so one target's records no longer delay another's.
- Track, bus and master insert automation ids share one lifetime rule: an id names its strip by identity and the kind of processor in its slot (for `effects.gsEfx`, its EFX type too), so a track insert id now survives `setTrackLanes` reorders and the removal of other tracks exactly as a bus id survives bus changes, and every insert id stays valid until its track or bus is removed or its slot comes to hold another kind of processor. An id held across such a change now applies nothing (an unknown target) instead of silently driving a parameter of the new processor, its queued edits and stored bases are dropped, and it is never reissued. Ids are assigned when the mixer is configured, so the WASM worklet's offline mirror and its audio-thread engine number them identically; an engine holds 8192 insert-id entries for its lifetime, and a strip, bus or lane change that would exceed them is refused with InvalidParameter (a rebuild that keeps every slot's kind needs no new entry). Solo/mute keeps taking a lane position, not an id.
- Boundary detection's sliding reference sets aside a lone dominant event — one no other event within `referenceWindow` comes within `threshold` of — so a short loud burst no longer hides the section changes around it. A section's `energyLevel` is its median frame RMS (normalised by the loudest section) instead of the mean, so the burst no longer makes its own section the loudest.
- The meter estimate leaves out at most two beats whose click a louder event masked (no onset evidence while their low-band energy is among the highest); rests still count. A burst over a downbeat no longer turns 3/4 into 6 or 4/4 into 3. `estimateMeter` without an envelope is unchanged.
- Clip detection takes the flat level from the flat runs themselves — the largest run level once the two highest runs are set aside, with candidates at or above −40 dBFS — instead of a 1 dB window under the channel peak, so a louder unclipped transient no longer hides clipped plateaus. Clipped material quieter than −40 dBFS is no longer reported.
- The mastering assistant withholds the declip proposal when any channel holds audio louder than the pinned level, mono input included.
- The assistant's `attackDensity` counts only onset peaks above a fixed floor of percussive rise, so steady tones report 0. `attackDensity`, `sustainRatio` and the mixing classifier's hit detection measure against the event maximum with the loudest events set aside, one per 10 s of audio up to two; under 10 s nothing is set aside and values are unchanged.
- `MelodyAnalyzer` reads `frameLength` as samples at 22050 Hz and rescales it to the input rate; `hopLength` stays in input samples. Unchanged at 22050 Hz.
- The streaming analyzer reads `nFft` as samples at 44100 Hz; below that rate the window is rescaled to the analysis rate (never shorter than `hopLength`), so a 22050 Hz stream emits more frames with a smaller magnitude spectrum. Unchanged at 44100 Hz and above.
- Polyphony analysis reads `nFft`, `winLength` and `hopLength` — defaults and explicit values alike — as samples at 44100 Hz and rescales window and hop to the input rate; the note transcriber's polyphonic path follows. Unchanged at 44100 Hz.
- GS insertion-effect balance follows the measured two-ramp law: the louder side of dry and effect stays at full level, so the default centre byte now plays both at unity where it played each at half (about 6 dB quieter). Output and tap level bytes no longer stop at −24 dB; each byte maps to its own level and 0 is silence.
- Feedback in the stereo delay, chorus, flanger, phaser and pitch shifter now reaches 98% (was 95%), and chorus and ensemble pre-delay reach 100 ms (were 50 and 25 ms), so GS EFX bytes past those points are no longer clipped.
- Chorus, flanger, ensemble, ring modulator, Dattorro reverb and bitcrusher accept `mixLaw`, as the stereo delay and pitch shifter already did.
- The WASM browser-decoder fallback (`Audio.fromMemoryWithBrowserFallback`) folds a multi-channel `AudioBuffer` to mono with `downmix`, the rule the native decoder applies, instead of the unweighted mean of every channel, so a file folds to the same samples whichever decoder ran. Stereo is unchanged; a 5.1 or 7.1 file now has its center and surrounds at -3 dB and its LFE dropped.
- Every STFT path, including time stretch, pitch shift, HPSS and spectral edit, needs `nFft` of at least 4 (Python: `n_fft`). A two-point Hann window is all zeros. Spectral edit with a rectangular window used to accept 2; it no longer does. HPSS also refuses a window and `win_length` pair whose overlap-added windows leave a gap.
- Time stretch, pitch shift and tempo-sync warps advance phase after emitting each frame, and phase locking keeps DC and Nyquist content. Their output differs from earlier versions.
- 16- and 24-bit WAV output is written on the same 2^(b-1) scale the reader uses, so decoding and saving again reproduces every sample code. A written sample can differ by 1 LSB from earlier versions, and −1.0 now writes the minimum code.
- Tape and transformer saturation evaluate their small-signal response without cancellation error. Mastering presets with a tape stage, and chains with a transformer, render slightly differently.
- Loudness matching solves for the gain that lands the remeasured signal on the target. This covers A/B match, reference matching, LUFS normalize, the chain's loudness stages and `maximizer.loudnessOptimize`. When the gain moves blocks across the absolute gate, the applied gain is no longer `reference − source` (C: `applied_gain_db`).
- The mixing assistant counts the measured peaks of short audible tracks toward master headroom. Phase alignment picks its window by the activity in the span the correlation actually uses.

### Fixes

- Published insert parameter bounds are rounded inward, so a gain ceiling such as 770.637 that the insert itself refused is no longer advertised.
- Percussion keeps its strike level under pitch bend, the organ wind chest stops loading the regulator when a release ends mid-block, harpsichord tails fade with the release law on choke, and SF2 applies GS scale tuning live.
- MIDI FX transpose and chord fan-out follow MIDI 2.0 per-note pitch bend, per-note management and absolute-pitch attributes; same-frame pending FX events keep their order.
- The reed tonehole reflection follows the bent pitch.
- A second bounce or freeze of the same engine renders the same audio as the first instead of starting from the processor state the first one left behind.
- The WASM worklet engine's `renderOffline` primes its offline engine before rendering, so its first block no longer ramps in from default parameter values.
- The transient shaper and vocal rider report their gain reduction, in the per-insert gain-reduction readout and in the mastering chain's stage gain reductions.
- A muted or solo-silenced lane reaches exactly zero gain, and an unmuted lane exactly unity, instead of approaching them without arriving.
- A refused sidechain binding no longer publishes a provisional binding table to the audio thread before it is rolled back.
- A track insert automation lane set before `setTrackLanes` reordered the lanes no longer moves to whichever track took its old position when another automation lane is set afterwards (C, Node, Python and WASM).
- After an instrument switches to the classic GS EFX realisation, retained EFX CONTROL modulation keeps working, and an offline bounce sizes the GS EFX tail from the controller-applied parameters.
- Mixer delay compensation covers direct strip inputs and inserts ahead of a keyed sidechain detector. A recompile keeps the audio already queued on unchanged routes.
- The mixer's reported tail follows automated insert parameters and includes a key-listen sidechain path. A pre-fader send no longer inherits the tail of post-fader inserts. This applies to both the C mixer and the realtime engine.
- Dense mixer automation keeps its intermediate points. Commands issued in order at the same sample time apply in order even when the pending queue is full.
- Removing a send no longer breaks the automation of the sends after it. Scene export writes the live send levels and insert parameters. Removing a bus clears the sidechain keys that named it.
- Re-enabling a bus EQ no longer replays stale filter state.
- A dynamics processor no longer lets an earlier `set_config` overwrite later automation.
- CutFilter automation of IIR parameters no longer allocates on the audio thread. A change that rebuilds a linear-phase FIR (a brickwall corner, or an Equalizer linear-phase or brickwall band) is reported as not realtime-safe instead of being accepted there.
- Short inputs no longer collapse to silence or to nothing: mono clips and warp segments shorter than one hop, short custom HRTF responses when resampled, and empty streaming phase-vocoder jobs, which no longer emit one silent sample.
- SMF import keeps a fragmented SysEx message across meta events, and an empty F7 escape no longer moves the next SysEx earlier. SMF and MIDI Clip File export keep a leading F0 or trailing F7 data byte of a SysEx8 payload.
- Generated cabinet impulse responses keep in-band driver directivity at low sample rates.
- Multiband setters reconfigure the crossover the processor owns, before or after prepare. A sparse dynamic-EQ sub-band keeps the slot its parameter keys name.
- Gated silence trimming no longer rescans the full RMS window for every sample.

## v1.8.2 (2026-10-06)

This release adds offline monophonic vocal editing, compilation of a project into a timeline that a stopped realtime engine can play, per-part rigs with new overdrive and distortion pedal inserts, realtime surround pan on track lanes, and per-insert gain-reduction metering. It makes mixing, sidechain and offline bounce timing independent of lane order and block size, tightens validation across mastering, effects and MIDI file import, and changes how several mixing and repair processors sound.

### Upgrade notes

#### Rebuild

- A project with a part rig entry is written as schema version 5; v1.8.1 refuses it. Documents without part rigs are written as before.
- No ABI counter moved; the vocal edit API carries its own counters (`SONARE_VOCAL_EDIT_API_VERSION`, `SONARE_VOCAL_PROJECT_API_VERSION`).

#### Now refused

- SMF import refuses a format 0 file whose track count is not one and skips an out-of-range key signature, and SMF export refuses a PPQN above `0x7FFF` (`sonare_project_import_smf`, `sonare_project_export_smf`, `sonare_note_targets_from_smf`).
- MIDI 2.0 Clip File import refuses a file without the configuration header or without exactly one Start of Clip followed by one End of Clip.
- A lane sidechain binding that keys its own lane, closes a cycle or needs an alignment past the delay ceiling is refused (`sonare_engine_set_lane_sidechain`, `setLaneSidechain`, `set_lane_sidechain`).
- A bounce whose non-zero source sample rate differs from the engine's prepared rate is refused.
- An auto-length project bounce (`total_frames <= 0`) is refused when an instrument reports an unbounded tail.
- The mixing assistant refuses a non-finite config value, an empty or duplicate profile id, and profiles that do not follow the tracks in order (`sonare_mixing_assistant_suggest` and its binding equivalents).
- The mastering assistant refuses a non-finite target loudness, ceiling or speech mono amount, and an unknown preset or target platform.
- The parallel compressor refuses a non-finite threshold, ratio, attack, release, mix or makeup, and an output ceiling that does not give a finite positive gain.
- Classical denoise and dereverb (offline, linked, streaming and live) refuse a `hop_length` above `n_fft / 2`, an `n_fft` outside the supported power-of-two range, and a Hann overlap that cannot reconstruct every sample.
- Declip refuses NaN or infinite samples, and the amp simulator and cabinet refuse a NaN or infinite sample rate.
- The vowel filter refuses a `driveOn` value other than 0 or 1, and the Dattorro reverb refuses a modulation depth above 672 reference-rate samples.

### New

#### Editing and alignment

- Edit monophonic vocal takes offline: note analysis, pitch plans with note transitions, drafts, undo/redo, unit-wise render jobs and a persisted session state, plus applying edits to a project and rehydrating them after clips are removed, split, duplicated or rebound (C: `sonare_vocal_edit_api_version`, `sonare_project_apply_vocal_edit`, `sonare_project_get_vocal_edit_dependencies`, `sonare_project_rehydrate_vocal_edits`; Node and WASM: `createVocalEditSession`, `restoreVocalEditSession`, `applyVocalEdit`, `getVocalEditDependencies`, `rehydrateVocalEdits`, and on WASM a dedicated worker client at the `./vocal-edit-worker` export; Python: `create_vocal_edit_session`, `restore_vocal_edit_session`, `apply_vocal_edit`, `get_vocal_edit_dependencies`, `rehydrate_vocal_edits`).

#### Realtime engine and project

- Compile a project into a timeline snapshot and install it all-or-nothing into a stopped realtime engine prepared at the project's rate (C: `sonare_project_compile_timeline`, `sonare_project_timeline_destroy`, `sonare_engine_apply_project_timeline`; Node and WASM: `compileTimeline`, `applyProjectTimeline`, `ProjectTimeline`; Python: `compile_timeline`, `apply_project_timeline`, `ProjectTimeline`). On WASM, `applyProjectTimeline` refuses a timeline that did not come from the module's own `ProjectTimeline`.
- The WASM worklet engine accepts multi-word UMP packets in `pushMidiUmp` and raw UMP input through `pushMidiInputUmp`, which `bindWebMidi` uses for MIDI 2.0 ports.

#### Mixing

- Pan a track lane strip in surround in real time, with a 5 ms constant-power glide when the lane feeds a bus wider than stereo (C: `sonare_engine_set_track_strip_surround_pan`; Node and WASM: `setTrackStripSurroundPan`; Python: `set_track_strip_surround_pan`).
- Read each insert's gain reduction for a meter target's strip, in pre/post insert order (C: `sonare_engine_meter_target_insert_gain_reduction`, `SONARE_METER_MAX_INSERTS`; Node and WASM: `meterTargetInsertGainReduction`, and on WASM `insertGainReductionDb` on worklet meter snapshots; Python: `meter_target_insert_gain_reduction`).

#### MIDI and synthesizer

- Choose the rig of a part, or of a whole destination, per project and on the realtime engine: the instrument's default (`bank`), nothing (`none`, the direct signal) or an explicit chain of up to eight inserts (`chain`). C: `sonare_project_set_part_rig`, `sonare_project_get_part_rig`, `sonare_project_clear_part_rig`, `sonare_engine_set_part_rig`. Node and WASM: `setPartRig`, `getPartRig`, `clearPartRig` on the project and `setPartRig` on the engine, with `PART_RIG_ALL_PARTS` and `PART_RIG_MODES`. Python: `set_part_rig`, `get_part_rig`, `clear_part_rig` on the project and `set_part_rig` on the engine. Project bounces apply the entries to SoundFont and physical-model destinations; SMF export does not write them. A chain that contains an amplifier takes a mono input up to its last amplifier, as the default rig does.
- The physical-model synth plays the electric guitars through the default amplifier rig when the host supplies an insert factory, and reads GS insertion-effect messages while GM program selection is on. A live engine applies the messages the host pushes and does not interpret insertion-effect messages scheduled inside a clip.
- NativeSynth parameter changes reach notes already held, with envelope, LFO and body-resonator settings retuning live.
- GS part NRPN and Sound Controller CC 71–78, with their SysEx aliases, edit the parts of the native synth, honouring Rx.NRPN.

#### Mastering and repair

- Add the `saturation.overdrive` (`gainDb` 0–41, `toneHz` 500–8000, `levelDb`) and `saturation.distortion` (`gainDb` 0–60, `toneHz` 475–20000, `levelDb`) inserts, each reporting 48 samples of latency.
- GS overdrive and distortion insertion effects run as a drive pedal into an amplifier: Drive sets the pedal gain, Amp Type selects the amplifier and Amp Sw switches the cabinet on every amplifier stage.

### Behaviour changes

- Programs 29 and 30 (Overdriven and Distortion Guitar) put a drive pedal ahead of the amplifier, with new amplifier presets and levels, on the SoundFont player.
- Electric guitars on the physical-model synth are no longer the direct signal in hosts that supply an insert factory; `set_part_rig` with part `0xFF` and mode `none` restores it.
- A part routed into a GS insertion-effect unit keeps the default bank rig ahead of the unit, in series, for every effect type; this reverts the v1.8.1 behaviour in which a routed part dropped it.
- GS overdrive and distortion insertion effects sound different: they gain the pedal stage, and Amp Type now selects the amplifier rather than the cabinet.
- The pipe organ's wind and swell shutter run per part ahead of the part's rig and also apply when GM program selection resolves a pipe organ, and mono legato retunes a voice's body resonator; the physical-model voices other than the acoustic piano are still being tuned and their output will move in 1.8.x patch releases.
- MPE pressure and timbre combine at full MIDI 2.0 width, a released MPE note's pressure, timbre and bend freeze at its note-off, and a MIDI 2.0 absolute per-note pitch selects the drum piece on the physical-model synth.
- A MIDI 2.0 note-on of velocity 0 converts to MIDI 1.0 velocity 1, and registered, assignable and relative controller bank and index are masked to 7 bits.
- Offline engine bounces default to the engine's prepared rate and keep the source rate when no target rate is given, instead of resampling to 48000 Hz.
- Track lanes render in sidechain-key order with the key delay-compensated to the destination strip, so results no longer depend on lane order or block size, and the reported engine latency includes the compensation.
- A strip's channel delay shifts only that strip and no longer counts as latency, so delay compensation no longer delays the other lanes.
- Stereo pan smooths by position, so panned strips and pan automation render differently; a surround panner move keeps unity non-LFE power.
- A strip's stereo width applies before its post-insert effects instead of after them.
- The parallel compressor's output limiter applies one gain across all channels and keeps its state across linked-detection switches.
- The multiband compressor, expander, limiter and spectral shaper report their gain reduction to the strip meters.
- Dehum tracks stereo hum per channel, removing opposite-polarity and one-sided hum.
- The mastering assistant measures clicks, clipping, noise and hum per channel of a stereo profile and withholds the declip suggestion when its shared threshold would rewrite a channel without clipped runs.
- Streaming denoise starts on a hop-aligned zero prefix, so its first output frames differ.

### Fixes

- A stereo channel strip at rest (centred pan, width 1) passes audio bit-exact.
- Clips on tracks without a lane are delay-compensated against lane latency, and sidechain keys are timed for the latency of the target insert.
- A failed master strip update leaves the previous strip and its sidechain bindings in place, and a send on a strip wider than eight channels keeps every plane.
- Project and stem bounces match a plain offline render for processors whose state advances without input.
- Removing a lane's automation returns its fader to 0 dB and its pan to centre.
- Repeated `set_track_lanes` calls with new track ids no longer exhaust the engine's 32 owned track strips.
- Phaser, adaptive-release and spectral-shaper bound automation no longer depends on write order, the phaser keeps its sweep bounds across sample-rate changes, and the adaptive release accepts equal crest bounds.
- Classical WPE dereverb uses the Hermitian covariance, so its output changes.
- The phase-align delay search stays within the input length.
- Bit-depth reduction above 25 bits keeps the top code below full scale.
- MIDI routing remaps per-note pressure, bend and controllers with their note's channel, as it already did for note-off.
- A GS EFX DT1 run applies only inside the EFX block its start address selects.
- SMF2 clip export skips Utility and Stream messages, and SMF export drops SysEx that cannot be written as SysEx7.
- WASM `bindWebMidi` forwards pitch bend, channel pressure and polyphonic pressure, and accepts one-data-byte messages with running status.
- The mixing assistant compares masking only between tracks measured at the same sample rate, FFT size and hop.

## v1.8.1 (2026-10-03)

This release adds the GS insertion effect as a standalone mastering insert, a stereo A/B pair processor with loudness matching, live parameter edits on the streaming mastering chain, explicit voicing for note rendering and pitch decomposition, input peaks and a loudness reset in engine metering, and physical source offsets and loop anchors on arrangement clips. It tightens validation across mastering, MIDI and note editing, and retunes several physical-model voices.

### Upgrade notes

#### Rebuild

- The feature ABI moved from 5 to 6: `sonare_render_notes` and `sonare_decompose_note_pitch` take a `voiced` argument after `f0_hz`, so C callers must pass it (`NULL` keeps the previous behaviour). The Python binding refuses a shared library built from a different tree; rebuild the library alongside the binding.
- A project with a clip or take carrying a physical source offset or loop anchor is written as schema version 3, and one with comp render parts as version 4; v1.8.0 refuses both. Documents without those fields are written as before.
- The AudioWorklet meter ring record grows from 14 to 16 floats to carry the input peaks; code reading the ring directly must size it with `SONARE_METER_RING_RECORD_FLOATS`.
- `SonareEngineCommandType` gains `ResetMasterLoudnessMeter` (28), appended after the existing values.

#### Removed and renamed

- The worklet `syncMixer` message's `forceInsertResets` is now `insertBaseResets`, and it clears the strips' manual insert bases after the strip replay rather than resetting insert state.

#### Now refused

- A dB gain whose float linear multiplier overflows (above about 770 dB) on every gain-bearing processor, including utility gain, limiter post gain, compressor makeup, saturation drives and output gains, loudness targets and EQ band gains; the published parameter metadata carries the new maximum.
- Non-finite timing, ceiling or release values and a lookahead too long for the sample rate on the limiters.
- A bit depth of 1 on the bitcrusher, which has no quantization levels; the minimum is 2.
- Non-finite configuration or sample rate on the sidechain router, and a lookahead too long for the sample rate; a null channel plane or non-finite sample rate on `StreamingMasteringChain`.
- An empty track id in the mixing assistant, and an impossible channel count in linked stem decomposition and repair.
- A note render span past the end of the audio, non-finite samples or a non-finite gain times envelope in note rendering, and a split or merge that leaves an empty span or carries an invalid edit on any note in the set; in C++, a merge over notes whose sample spans run out of order, a split that would drop the note, and a non-positive `reference_hz`.
- On WASM, an `f0Hz` that is not a `Float32Array` in `renderNotes` and `decomposeNotePitch`, as on Node.
- A clip SysEx the destination instrument cannot prepare (the whole schedule fails with `SONARE_ERROR_INVALID_PARAMETER`), binding an instrument that cannot prepare a SysEx already scheduled for it, a live SysEx the destination cannot prepare, and a single SysEx payload larger than the retention budget.
- An SMF2 clip opened by Start of Clip without End of Clip, an SMF2 export whose UMP word counts disagree with their message types, and a GM reset whose body is not exactly four 7-bit bytes.
- A pitch-correction frame marked voiced whose F0 is zero, negative or non-finite.
- A CC-learn `minMovement` outside 0–127, the 7-bit CC units the threshold is compared in, on every surface.
- An SMF2 tempo that is zero or outside the public tempo range, which is now skipped and counted like other invalid SMF2 entries.
- A project document declaring schema version 0.
- On WASM, a mastering repair option of the wrong type, which used to fall back to the default, as on Node.

### New

#### Mastering and repair

- Run a GS insertion effect as a named insert (`effects.gsEfx`, with `typeMsb`, `typeLsb`, `realization` and `byte0`–`byte19`), available with `BUILD_FX=ON`.
- Run the A/B crossfade pair processor on stereo pairs and match the pair's loudness (`masteringPairProcessStereo` with `match.abCrossfade`, `masteringAbMatchLoudnessStereo`, `mastering_pair_process_stereo`, `mastering_ab_match_loudness_stereo`, `sonare_mastering_apply_pair_processor_stereo[_ex]`, `sonare_mastering_ab_match_loudness_stereo`).
- Change a parameter of a running `StreamingMasteringChain` without rebuilding it (`setParameter`, `set_parameter`, `sonare_streaming_mastering_chain_set_parameter`).
- Automate the ceiling of `TruePeakLimiter`, `Maximizer`, `AdaptiveRelease` and `SoftKneeMax` as a realtime insert parameter.
- Automate the GS-bound controls of the pitch shifter, rotary, Dattorro reverb and bitcrusher live, within the ranges the GS EFX tables reach.

#### Editing and alignment

- Pass per-frame voicing to note rendering and pitch decomposition (`voiced` on Node, WASM, Python and C).

#### MIDI and synthesizer

- GS system reverb CHARACTER 6 (Delay) and 7 (Panning Delay) render as delays with their own time and feedback, and Panning Delay alternates outputs.
- The GS delay keeps a negative feedback setting, giving alternating-sign echoes.

#### Realtime engine and project

- Read the pre-trim input peaks from engine meter telemetry (`inputPeakDbL` / `inputPeakDbR` / `inputPeakDb`, `input_peak_db_l` / `input_peak_db_r` / `input_peak_db`, `sonare_engine_drain_meter_telemetry_v2` / `_wide_v2`); `gainReductionDb` now also reports strip and bus gain reduction.
- Reset the master integrated-loudness meter at a render frame (`resetMasterLoudnessMeter`, `reset_master_loudness_meter`, `sonare_engine_reset_master_loudness_meter`).
- Read the value an insert parameter was constructed with (`insertParameterConstructedValue`, `insert_parameter_constructed_value`, `sonare_engine_insert_parameter_constructed_value`).
- Give an arrangement clip a physical source offset in seconds and a loop anchor, and keep comp render parts across a trim and re-extend.
- Project bounce routes through the mixer when buses are authored, with automation latency compensated.

#### Distribution and build

- The GS protocol layer and the GS insertion effect build as their own library targets (`sonare_gs_protocol`, `sonare_gs_efx`).
- The WASM AudioWorklet delivers scope telemetry through `postMessage` when no `SharedArrayBuffer` is available.
- WASM `decomposeStems` defaults `sampleRate` to 22050, as on Node and Python.

### Behaviour changes

- Transcription with `fmin`/`fmax` left at 0 uses the selected path's own range (65–2093 Hz monophonic, 55–1760 Hz polyphonic), and the polyphonic path honours an explicit range.
- Dither and the bitcrusher keep independent noise-shaping state per channel and feed back the clamped error, so dithered output differs.
- SF2 playback aligns latent GS insertion stages with their dry path, so output through those stages differs.
- 7.1 layouts pan Ls/Rs at ±135°.
- The sidechain router honours a lookahead above 1000 ms instead of clamping it, which raises its reported latency.
- Binaural panner, HRTF and amp-sim cabinet impulse responses keep their level across host sample rates.
- The playback renderer's `limiter_gain_reduction_db` diagnostic reports the deeper of the main and LFE limiters.
- An empty compiled timeline resets the engine to 120 BPM and 4/4, and time-signature segments are sorted and deduplicated on project load, the last one at a tick winning.
- Switching a MIDI destination to or from external output takes effect at the next block, releasing its notes and resetting its controllers through the old route.
- On WASM, a failed instrument bind throws `InvalidParameter` or `OutOfMemory` instead of `InvalidState`, and a failed clear throws and leaves the instrument bound.
- Splitting a note shifts the tail's time offset by the head's stretch, omits a half with no sample span, and resamples the envelopes on the sample grid.
- On the SF2 player, Reset All Controllers also clears the CC1, CC2, CC11, pedal and RPN/NRPN positions that drive controller-assigned modulation, and CC2 refreshes channel modulation immediately.
- On WASM, a non-finite `frameRate` in `renderNotes` and `decomposeNotePitch` throws `RangeError` instead of `SonareError`.
- The MIDI 1.0 to 2.0 translator keeps bank and RPN state per group as well as per channel.
- MIDI Clip File time signatures read and write the denominator as a power-of-two exponent, as the specification defines, so files written by earlier versions read with a different denominator.
- Physical-model voice output changes: the acoustic piano's half-pedal and sustain resonance, the plucked, bowed, reed, free-reed, brass and modal engines, and the electric guitars' pickup under pitch bend (GM 26–30); voices other than the piano are not yet calibrated and keep changing in patch releases.

### Fixes

- A muted channel strip keeps its insert state advancing, so unmuting does not replay a stale tail, and a seek publishes the automation value at the block start.
- A lane reorder keeps queued commands, insert automation, sends and lane state with their track, and a failed topology update leaves the previous graph intact.
- A bypassed insert resumes without a discontinuity, and a stopped strip's tail keeps decaying.
- Track lane changes keep pending insert automation, and bus EQ history follows the bus across moves.
- The WASM mixer keeps its JavaScript state when the native side refuses a routing edit, and reacquires its buffer views after topology or heap growth.
- The hard clipper's antialiasing no longer spikes on low-frequency input.
- A rejected streaming mastering block no longer cancels a flush in progress.
- The sidechain router recovers from non-finite input once per block.
- Pitch correction keeps exact 6-semitone passes, holds correction to the end of a voiced run, and passes through frames it cannot represent.
- Note rendering clears every edited source span before writing destinations, so the later note wins.
- An SMF declaring more tracks than it holds loads the tracks present and reports truncation.
- MIDI clip envelope and sequencer timing no longer overflow at extreme positions.
- Tempo and time-signature segments that share a start position keep the last one supplied, however many there are.
- SMF export no longer writes a SysEx payload's leading F0 twice, and an F7-escaped message carrying its own F0 imports like an F0 event.
- A project bounce fails instead of binding fader and pan automation to the wrong track when the engine refuses the lane layout.
- Dither output stays below full scale at target widths of 26 bits and above.
- A fader, trim and VCA offset that are each valid but sum past the float range keep the strip gain finite.
- A refused graphic EQ band gain leaves the band unchanged.
- A realtime parameter snapshot can no longer be replaced by an older one queued before it.
- GS EFX, system and EQ SysEx written directly to the SF2 player no longer reverts scheduled state, and a routed part drops the default bank rig.
- A tempo ramp whose endpoints both fall below the minimum tempo plays at a constant tempo.
- Arrangement clip loops keep their take and source position through a left-edge trim.
- The library builds with arrangement, mixing, effects and mastering switched off.
- C entry points clear their out-parameters before returning an argument error, including in builds without the feature.
- The controller reset sent on stop, seek, loop and route changes now reaches every channel played since the last reset, not only channels with a note still sounding.
- All Notes Off and All Sound Off no longer lift the sustain or sostenuto pedal on the built-in synth, the physical-model synth and the SF2 player.

### Performance

- The MIDI sequencer finds each block's first event by binary search rather than scanning every clip event already played.
- `sonare.wasm` is 125 KB smaller raw (38.8 KB gzipped), and `sonare-analysis.wasm` 16 KB smaller raw (5.5 KB gzipped).

## v1.8.0 (2026-09-30)

This release adds a rule-based mixing assistant, bus-to-bus routing with sends and track- or bus-keyed sidechains, surround-width buses, MIDI 2.0 input at full resolution, GS SysEx reception, host-supplied sample playback and a harpsichord engine, two realisations of the GS insertion effects, polyphonic note editing and audio-to-MIDI transcription, stereo and linked repair with restoration presets, take alignment and tuning to a MIDI reference, stem decomposition, and a playback renderer for headphones and speakers. The C++ library now installs as a CMake package.

### Upgrade notes

#### Rebuild

- Three C-ABI versions moved: feature 4 → 5, acoustic 3 → 4, project 1 → 2. The engine and voice-changer ABIs are unchanged.
- Rebuild any C consumer rather than relinking it, because these structs shipped in earlier releases and grew: `SonareNoteSegmenterConfig` (`voiced_threshold`, read only at `struct_version` 2), `SonareMasteringResult`, `SonareMasteringStereoResult`, `SonareMasteringChainResult` and `SonareMasteringChainStereoResult` (`non_finite_substitution_count`), `SonareDehumConfig` (`mode`), `SonareEngineBus` (`output_bus_id`, `sends`), `SonareEngineMidiClipSchedule` (`gain`, `fade_in_samples`, `fade_out_samples`), `SonareRirSynthConfig` and `SonareRoomMorphConfig` (three atmospheric-absorption fields), and `SonareProjectClipCompSegment` (`crossfade_ppq`, 24 → 32 bytes).
- A zero-initialized `SonareEngineMidiClipSchedule` is silent (`gain` 0); Node, WASM and Python default an omitted `gain` to 1.
- The Python binding refuses a shared library built from a different tree; rebuild the library alongside the binding.
- The Python package requires numpy 2.4.6 or later (previously 1.24).
- C++: `rt::ProcessorBase::set_parameter` is no longer virtual and refuses a non-finite value; a subclass overrides the protected `set_parameter_impl` instead. The unread `RhythmConfig::swing_threshold` is removed.

#### Removed and renamed

- The native CLI no longer accepts `mix`; use `mix-strip`. `sonare mix` on the Python CLI is unchanged and remains the scene mixer.
- `mastering-processor` has no `--stereo` flag; a two-channel file takes the stereo path on its own and is written as two channels.
- `sonare boundaries --min-distance` is now `--peak-distance`.
- `genreCandidates` is gone from the mastering assistant's suggestion and profile documents (`genre_candidates` from the CLI payloads); the assistant starts from `preset`, `streaming` by default.
- The dynamic EQ band's `lookaheadMs` is renamed `detectorDelayMs`; the old key and the CLI's `--lookahead-ms` are still accepted as aliases of `--detector-delay-ms`.
- `repair.denoise.gainFloor` (a linear floor) is now `repair.denoise.reductionDb` (maximum attenuation in dB, default 26); the old key is still read and converted.
- A bus insert automation id from `resolveBusInsertAutomationId` now names the bus rather than its position; an id resolved before this release must be resolved again.
- `saturation.multibandExciter` publishes five controls per band (`band0.` to `band2.`); parameter `p` of band `b` is id `b * 5 + p`, so automation by id must be renumbered.
- `roomMorph` returns `{ audio, sampleRate, diagnostics }` (`RoomMorphResult`) on every surface; read the samples from `.audio`.
- Catalog entries for construction-only settings carry a null `id`; code reading `id` from every entry must skip them (`id: number | null` on Node and WASM). `MasteringInsertParamInfo.unit` is `string | null`, and `dynamics.compressor`'s `detector` is typed `"enum"`.
- A mixer scene's processor `params` is written as a JSON object instead of a nested JSON string; both are read, but a document written now needs a reader from this version. `MixSceneInsert.params` is typed as a record.
- `EngineMidiEvent.renderFrame` is optional on Node and WASM (default 0), so reading it yields `number | undefined`.
- Node's `SonareError` is a runtime class, as on WASM; `instanceof` works and `isSonareError` is unchanged.
- WASM `pushMidiUmp` takes a word array; a bare number is still read as a one-word message.
- `SynthEnumTables` gains `controllerInputs`, `controllerAxes`, `articulations`, `mpeDimensions` and `noteTrackings`, which a Node or WASM type implementing it must add.
- The Node and WASM mastering assistant params type is `MasteringAssistantParams`, which admits a string value; existing calls still compile.

#### Now refused

- Node, WASM and Python refuse values they used to coerce: a fractional number where an integer is read (`RangeError` on Node and WASM), an integer outside its C type, a wrong-typed option field, a float no 32-bit float can hold, and an out-of-range enum ordinal. A fractional mastering-assistant parameter is refused rather than truncated, and a negative optional scalar at the C ABI is refused rather than promoted to its default.
- On Node a wrong-typed argument raises `TypeError` before any native state changes, async mastering and analysis entry points reject their promise instead of throwing, and streaming readers throw after `destroy`.
- Python buffer and argument validation raises `libsonare.SonareValueError`, a subclass of both `SonareError` and `ValueError` carrying `ErrorCode.INVALID_PARAMETER`. About 120 buffer-taking entry points now raise it with a message naming the function and argument (`spectral_centroid: samples contains NaN or Inf at index 0`) instead of `SonareError: [4] Invalid parameter`; code matching on message text must be updated.
- Python: `zero_crossings` and `pitch_tuning` refuse empty or non-finite input; `trim_silence`, `split_silence` and `fix_frames` refuse an empty buffer; `tempogram_ratio` refuses an empty `tempogram_data` and a non-finite or non-positive `factors` entry; `mix_stereo` refuses a scene whose strips are all empty or carry NaN.
- Python: an `Audio` accessor raises once the handle is closed, a multi-dimensional sample array is refused rather than flattened, an analysis ordinal outside its enum raises, and a `synthesize_rir` / `room_morph` seed outside `uint32` raises instead of being masked.
- The mastering assistant and profile param lists refuse an unknown key with `InvalidParameter`, naming the key and the accepted set. A restoration preset is refused as the assistant's `preset`, and an unknown `targetPlatform` name is refused.
- An enum value no enumerator declares is refused by key name on every mastering and effects selector, where it used to become the first enumerator or pass through.
- A non-default pan or width on a surround bus or the master is refused by name on every path.
- `setTrackBuses` refuses a bus list that forms a cycle, names an undeclared bus, routes a bus to itself, drops a bus a lane still targets, or exceeds the alignment-delay budget.
- A MIDI clip fade-out on an open-ended clip (`length_samples <= 0`) is refused.
- `sonare_engine_drain_external_midi` refuses a `max_events` under four, and the Node, WASM and Python drains use the same minimum.
- A synth mod routing with source or destination `none`, and a built-in synth waveform outside its enum, are refused.
- A non-finite pan or width in a mixer scene is refused.
- `griffinLim`, HPSS, time stretch, pitch shift and the phase vocoder refuse a hop above half the window; `griffinLim` takes at most 256 iterations. The facades accept any even `nFft`.
- The Mel band count is capped at 4096, and the `tempogram`, `fourierTempogram` and `plp` window at 12288 frames.
- `sonare_note_move` refuses a target with no room left in the buffer; a target that fits partly is truncated.
- `effects.reverb.room`, `effects.acoustic.roomMorph`, a directly built `RoomReverb`, and the CLIs' `--absorption` refuse an absorption outside [0, 1] instead of clamping it.
- YIN and pYIN refuse an `fmax` above Nyquist, an `fmin` no frame can hold, and swapped bounds; `pseudoCqt` and `hybridCqt` refuse bins above Nyquist.
- The streaming analyzer refuses a tuning reference outside 220–880 Hz at creation and on every later set.
- The meters and one-shot analyses (peak, RMS, DC offset, crest factor, true peak, dynamics, waveform peaks) refuse a non-finite buffer; the inverse reconstructions refuse a non-finite input matrix.
- WASM: a nested chain configuration passed straight to the module (not the flattened envelope) is refused by name; `masteringChain` and `masterAudio` are unaffected.
- WASM: errors from the mixing and project C-ABI entry points arrive as their documented codes instead of raw C++ exceptions; `Mixer.fromSceneJson` with an unknown insert or malformed JSON now reports `InvalidState`.
- `sonare_project_load_soundfont` returns `SONARE_ERROR_INVALID_FORMAT` for a malformed or over-budget SF2; the Python CLI's exit code for it moves from 3 to 5.
- Every caller-supplied JSON document is parsed under one resource budget and refuses duplicate keys where its entry point says so.
- CLI: numeric options are range-checked before a command runs, and the Python parser no longer resolves an abbreviated long option.
- CLI: `chords`, `dynamics`, `rhythm`, `sections`, `key`, `melody`, `pitch-shift`, `time-stretch`, `mel`, `cqt`, `vqt`, `tone`, `chirp`, `clicks`, `synthesize-rir` and `room-morph` refuse the values the library refuses on every other surface, including a non-positive `--n-fft` or `--hop-length`, a negative `--fmin` / `--fmax`, and a generator `--sr` the decoder cannot read back.
- CLI: `sonare boundaries` refuses a zero size and disabling both feature streams; the native CLI refuses a file with a non-finite sample or an unsupported rate, and a `--config` document whose root is not an object.
- CLI: `mastering-suggest` refuses a setting named both by an option and by `--params`, and an index for `targetPlatform` or `preset` in `--params`.

### New

#### Mixing

- Suggest a mixer scene — trims, faders, pans, widths, corrective EQ, dynamics, effect buses and sends — with a written reason per decision, from a set of tracks (`suggestMixScene` on Node and WASM, `suggest_mix_scene`, `sonare_mixing_assistant_suggest_scene_json`, `suggest-mix` on both CLIs).
- The assistant is rule-based, suggests without applying, and is a separate target removed by `-DBUILD_MIXING_ASSISTANT=OFF`.
- It carves an EQ band only where one part is built around it and the other can spare it; `enableHighPass` is off by default.
- Vocal, lead, keys and strings are taken from a track's name; `drumKit` covers a whole kit on one track, and a track mostly below the mono crossover is centred.
- Route a realtime bus into another bus and give it pre- or post-fader sends (`output_bus_id` and `sends` on `SonareEngineBus`; `outputBusId` / `output_bus_id` and `sends` in `setTrackBuses` / `set_track_buses`); latency is compensated per edge.
- Key a bus or master insert from a track or another bus (`setBusSidechain` / `setMasterSidechain`, `set_bus_sidechain` / `set_master_sidechain`, `sonare_engine_set_bus_sidechain` / `sonare_engine_set_master_sidechain` with `SonareSidechainSourceKind`); the binding table holds 32 entries, up from 16.
- Give return and group buses an output pan and an EQ of up to 24 bands, and give a strip the same `eq` (`pan`, `panMode`, `panLaw`, `dualPanLeft`, `dualPanRight`, `eq` in the scene; `setBusStripPan`, `setBusStripPanLaw`, `setBusStripPanMode`, `setBusStripDualPan`, `setBusStripEqBand` and `sonare_engine_set_bus_strip_*`).
- Render a surround bus at its declared width in the mixer graph and project bounce, with its fader, inserts and sends on every plane; `sonare_project_bounce` accepts 1, 2, 6 or 8 channels up to the master layout.
- Apply a MIDI clip's gain and fades to the rendered instrument audio; `sonare_project_set_clip_gain` / `sonare_project_set_clip_fade` now reach MIDI clips.
- Replay live insert edits across a strip replacement (`applyTrackStripInsertParamByNameNow`, `restoreTrackStripInsertParamByName`, `clearTrackInsertParameterBases`, `settleInsertParameters`, `applyCommandsDueNowPreservingFuture`, with master and bus forms and C and snake-case equivalents); the AudioWorklet does this itself and keeps strip setter changes across a mixer re-sync.
- Add a channel strip to a built mixer (`addStrip`, `add_strip`, `sonare_mixer_add_strip_ex`), and meter the WASM mixer's master true peak (`configureMeter`, `meterSnapshot`).
- Snap a strip's smoothers to their set values before an offline render (`sonare_strip_settle`, `settle`).
- Validate a mixer scene against `schemas/mixer-scene.schema.json`, shipped in the Python wheel and sdist and as the npm subpath `./schemas/mixer-scene.schema.json`.
- `Mixer` is a context manager on Python.

#### Mastering and repair

- Name the preset the mastering assistant starts from (`preset`, `sonare_mastering_preset_from_name`, `--preset` with `--assistant` on `mastering` and on Python `master`) and a delivery target (`targetPlatform`, `sonare_mastering_platform_from_name`, `masteringPlatformNames()`, `--target-platform`).
- Get the assistant's suggestion as a flat chain configuration that goes straight back in as `overrides` (`masteringAssistantSuggestChain`, `mastering_assistant_suggest_chain`, `sonare_mastering_assistant_suggest_chain_json`, each with a stereo form).
- Read a preset's flat parameters (`masteringPresetParams`, `mastering_preset_params`, `sonare_mastering_preset_params_json`) and per-preset targets from the catalog's new `masteringPresets` key (`name`, `kind`, `targetLufs`, `truePeakCeilingDb`, `maxLimiterGainReductionDb`).
- Bound how hard the loudness stage may drive the limiter with `maxLimiterGainReductionDb` (default 12 dB; 0 restores the previous headroom clamp).
- Restore damaged recordings with five restoration presets, `vinyl`, `tapeHiss`, `fieldRecording`, `voiceMemo` and `shellac78`, which run repair only and leave level alone; they treat a sustained tone as noise.
- Run every repair stage channel-linked on a stereo pair (`sonare_mastering_repair_*_stereo`) or across any number of channels (`_denoise_classical_linked`, `_dereverb_classical_linked`), and call each detector on its own (`sonare_mastering_repair_detect_*`, `sonare_mastering_repair_noise_band_bins`, `sonare_mastering_repair_dereverb_apply_room_estimate`), on every surface.
- Detect clipping from flat tops (`flat_run_count`, `longest_flat_run_samples`, `flat_sample_count`, `flat_level` on `SonareClipDetection`), and get a measured defect block in the assistant's profile with `detectDefects`.
- Choose hum removal by subtraction (default) or by notches (`SONARE_DEHUM_MODE_NOTCH`, `mode`), and a speech-presence-probability noise estimator (`SONARE_DENOISE_NOISE_ESTIMATOR_SPP`); the streaming chain builds `repair.denoise` with any recursive estimator.
- Use a 4x oversampled path on the hard clipper, soft clipper, waveshaper, exciter and presence enhancer (`aliasing`).
- Add an all-pass EQ band (`AllPass`) to align two sources that cancel; the linear-phase EQ refuses it.
- Match A/B loudness (`masteringAbMatchLoudness`, `mastering_ab_match_loudness`, `sonare_mastering_ab_match_loudness`), trim level with the `utility.gain` insert, list the amp presets (`masteringAmpPresetCatalog`), and read a band set's composite response (`sonare_eq_magnitude_response`, `magnitudeResponse`).
- Read momentary and short-term loudness series from a multichannel measurement (`sonare_lufs_series_interleaved`); `loudnessTargetLimited` is reported on the stereo results too.
- Ask what latency and tail a configured insert will report (`masteringInsertTiming`, `mastering_insert_timing`, `sonare_mastering_insert_timing`).
- Read a type, default and measured range for every catalog parameter, plus every construction-only setting (null `id`), `choices` for enumerated values, and `slot` / `slots` for conditional band groups (`MasteringInsertSlot`); the catalog grows from 1,130 to 5,455 entries.
- `saturation.multibandExciter` accepts any crossover split, and the multiband processors list every band up to nine.
- Count the non-finite samples a processor discarded or substituted (`non_finite_substitution_count`, `sonare_*_non_finite_discard_count`, `nonFiniteDiscardCount()`).
- `mastering-suggest` takes the assistant options by name on both CLIs, and `--config-out` / `--chain-config` pass a chain between them.
- WASM exports `CapabilityCatalog`, `CapabilityCatalogProcessor`, `CapabilityCatalogParameter`, `CapabilityCatalogPresets` and `CapabilityCatalogMasteringPreset`.

#### Analysis

- Separate a signal into listenable stems that keep the source's phase and sum back to it (`decomposeStems`, `decompose_stems`, `sonare_decompose_stems`), or across up to 64 channels with components that line up (`decomposeStemsLinked`, `decompose_stems_linked`, `sonare_decompose_stems_linked`).
- Transcribe audio to MIDI note events (`transcribe`, `sonare_transcribe`) or straight into a project clip (`transcribeToClip`, `transcribe_to_clip`, `sonare_project_transcribe_to_clip`).
- Find structural boundaries (`detectBoundaries`, `detect_boundaries`, `sonare_detect_boundaries`, `sonare_boundary_options_default`).
- Score a meter over an existing beat series (`estimateMeter`, `estimate_meter`, `sonare_estimate_meter_json`) with `grouping` (for example `[3, 2, 2]`) and `searched`; odd meters need `candidateNumerators` widened. Both CLIs take `--meter-candidates` and `--meter-denominator`, and `beatObservations.onsetStrength` is the intended accent source.
- Get a per-beat tempo curve from `analyze` with `computeTempoCurve` (`beatLocalBpm`).
- Pass a tuning offset to `analyze`, `analyzeWithProgress`, `detectChords` and `chordFunctionalAnalysis` (`tuning`, as `estimateTuning` returns it).
- Read a Roman numeral for each `analyze` chord (`romanNumeral`).
- Recognise eight more chord qualities when `useTriadsOnly` is off: `6`, `m6`, `mM7`, `7sus4`, `11`, `13`, `7b9` and `7#9`.
- Pass `analyze` options with progress reporting (`sonare_analyze_json_ex_with_progress`).
- Set the note segmenter's `voicedThreshold` (default 0.5) for low-register material.
- Add ISO 9613-1 air absorption to a synthesized room (`airAbsorptionEnabled`, `airTemperatureC`, `airHumidityPercent`) in `synthesizeRir`, `roomMorph` and the room inserts; off by default.
- Normalize a stereo pair on one gain (`normalizeStereo`, `normalize_stereo`, `normalize_rms_stereo`, `sonare_normalize_stereo`, `sonare_normalize_rms_stereo`) and load one channel of a file (`Audio.fromFileChannel`, `Audio.from_file_channel`, `sonare_audio_from_file_channel`).
- Call eleven offline meters as `Audio` methods (`sonare_audio_peak_db` and siblings).
- Read diagnostics with code, message and severity (`sonare_last_diagnostic_count` and its accessors); Python's `RirResult` and `RoomMorphResult` carry `diagnostics`, and `acoustic.rir_length_floored` and `acoustic.rir_tail_truncated` are new warnings.
- `SONARE_ERROR_ENCODE_FAILED` (`EncodeFailed` / `ENCODE_FAILED`) reports a failed WAV write; the public C enums have a fixed `int32_t` underlying type, and every WASM handle class releases through `destroy`.

#### Editing and alignment

- Edit notes in a monophonic take as objects — pitch, timing, formant, envelope, vibrato, drift, split and merge (`extractNotes`, `renderNotes`, `sonare_extract_notes`, `sonare_render_notes`, `sonare_split_note`, `sonare_merge_notes`, `sonare_decompose_note_pitch`) — and do the same for percussive hits.
- Edit notes in polyphonic material (`analyzePolyphonic` / `PolyphonicAnalysis`, `sonare_polyphonic_analyze`, `sonare_polyphonic_set_note_edit`, `sonare_polyphonic_render`; `polyphonic-notes` / `polyphonic-render` on both CLIs).
- Align a take to a reference take as a project warp map (`alignTakeToReference`, `align_take_to_reference`, `sonare_align_take_to_reference`; `project align-takes` on both CLIs).
- Tune a take to a melody in a MIDI file (`noteTargetsFromSmf`, `assignNoteTargets`, `note_targets_from_smf`, `assign_note_targets`, `sonare_note_targets_from_smf`, `sonare_assign_note_targets`; `tune-to-midi` on both CLIs), with `minOverlapRatio`, `maxCorrectionSemitones` and `unmatchedPolicy`.
- Find cut points several takes share as silence (`splitSilenceCommon`, `split_silence_common`, `sonare_split_silence_common`), with a report of why (`splitSilenceCommonWithReport`, `sonare_split_silence_common_ex`).
- Resolve one set of zero-crossing cut points for all channels (`remixAlignedIntervals`, `remix_aligned_intervals`, `sonare_remix_aligned_intervals`).
- Crossfade a comp seam (`crossfade_ppq` on a comp segment, default 0).
- Play a warped clip without transposing (`'time-stretch'` warp mode), with a voice pool of 8 by default, settable from 0 to 64 (`setWarpVoiceCapacity`, `set_warp_voice_capacity`, `sonare_engine_set_warp_voice_capacity`).
- Pass options to project tempo analysis (`SonareProjectTempoOptions`, `sonare_project_analyze_tempo_with_options`, `sonare_project_auto_tempo_with_options`) and read the tempo and time-signature maps back (`tempoSegmentByIndex`, `timeSignatureByIndex`).
- Preview and apply rule-based composition-assist note placements through the undo history (`sonare_project_assist_preview_json`, `sonare_project_assist_apply_json`).

#### MIDI and synthesizer

- Receive MIDI 2.0 channel voice messages at full resolution on NativeSynth, Sf2Player and BuiltinSynth, including per-note pitch bend, Pitch 7.25 / 7.9, per-note management, relative controllers and full-width MPE; a MIDI 1.0 performance renders exactly as before.
- Push multi-word UMP messages (`pushMidiUmp`, `pushMidiInputUmp`, `sonare_engine_push_midi_ump`, `sonare_engine_push_midi_input_ump`) and build them with `Project.midi2*` / `Project.midi2_*` and fifteen `sonare_midi2_*` builders.
- Push per-note bend and pressure at full width (`pushMidiPitchBend`, `pushMidiChannelPressure`, `pushMidiPolyPressure` and their Python and C equivalents), and choose which note a channel-addressed MPE value reaches (`setControllerNoteTracking`).
- Bind a controller, pressure, bend or velocity to an expression axis with a range and curve (`bindController`, `bind_controller`, `sonare_engine_bind_controller`).
- Play a channel mono with retrigger or legato (`setArticulation`; fallbacks counted by `sonare_engine_legato_fallback_count`).
- Play host-supplied PCM through the `sample` engine mode and a `SampleBank` (`SampleBank`, `sample_bank`, `sonare_sample_bank_create`).
- Play the `harpsichord` engine, a jack-and-plectrum model with string-choir registrations.
- Use a 12 dB/oct highpass (`hpCutoffHz`), sample-and-hold and bit reduction (`sampleHoldHz`, `bitDepth`), a pitch offset in cents (`pitchOffsetCents`), new mod sources and destinations, and a patch `gain` up to 16.
- Restart per-voice randomness at each note for repeatable renders (`retrigger: 'note'`, `SonareSynthPatch` struct version 7).
- Follow GM program changes in a realtime NativeSynth (`useGmPrograms` on `setSynthInstrument`, `sonare_engine_set_synth_instrument_binding`).
- Automate hosted instrument parameters from a lane (`sonare_engine_resolve_instrument_automation_id`); `parameterInfo` describes the engine's reserved automation ids.
- Receive GS SysEx: system mode, master tune, volume, pan and key shift, the reverb, chorus and delay blocks, master EQ, part parameters, the controller-destination block, the drum setup block with user drum kits, and EFX control source and depth.
- Choose between two realisations of the 64 GS insertion effects, modern (the library's own inserts, default) and classic (whole-type models at a 32 kHz clock), with `gs_efx_realization` / `gsEfxRealization` (`SonareEngineSf2InstrumentConfig` struct version 4); switching crossfades.
- Select 30 GS variation tones and all 26 GS drum sets, and query them (`synthGsDrumKitName`, `sonare_synth_gs_drum_kit_name`, `sonare_synth_gs_drum_kit_is_voiced_apart`, `sonare_synth_gs_variation_is_voiced_apart`); Bank Select LSB selects the tone map (`GsToneMap`).
- Get the direct signal of the GM electric guitars instead of the bound amplifier (`clear_bank_rig` / `clearBankRig`, SoundFont instrument config struct version 3).
- Give a snare's wire bed its own damping (`wire_decay_ms`).
- New effect controls: `interpolation` on the modulation and delay inserts, `antiAlias` and a second voice on the pitch shifter, feedback and phase on chorus and flanger, an LFO on the wah, two-speed rotary rotors and `model` 1, stereo-delay taps and modulation, plate-reverb tank sets, limiter `ratio` and `postGainDb`, and two amp-sim cabinet voicings.
- New inserts: `stereo.binaural` (HRTF panner, headphone or speaker output) and `effects.filter.vowel` (formant filter over a, i, u, e, o).
- The amp sim gains a triode-cascade preamp, passive tone stack, speaker cone stage and an optional second cabinet microphone.

#### Realtime engine and project

- Render offline in chunks and finish once (`renderOffline({ finalize: false })`, `finishOfflineRender`, `sonare_engine_render_offline_ex`, `sonare_engine_finish_offline_render`).
- Use the streaming retune stage on every surface (`StreamingRetune`, `sonare_streaming_retune_*`).
- The realtime clip streamer requests pages ahead of the playhead, half a second by default.

#### Playback renderer

- Render mono, stereo, 5.1 or 7.1 PCM for headphones or speakers — layout conversion, loudness matching, night mode, bass management and HRTF rendering with head tracking (`PlaybackRenderer`, `sonare_playback_renderer_*`); removed by `-DBUILD_PLAYBACK=OFF`.
- Render a whole signal in one call with latency trimmed (`renderPlayback`, `render_playback`, `sonare_playback_render_interleaved`, `sonare playback` on both CLIs).
- Latency depends only on the output target and distance compensation: 288 samples at 48 kHz to stereo speakers, 1312 to 5.1, 7.1 or headphones.
- Load an HRTF set (`HrtfSet`, `sonare_hrtf_set_*`) from the embedded default or a SHRF v1 document; the npm package ships the default as `./hrtf/default.shrf` for WASM, and `tools/playback/sofa_to_shrf.py` converts SOFA files.
- Measure programme loudness for it (`PlaybackLoudnessMeter`, `sonare_playback_loudness_meter_*`), and validate its configuration against `schemas/playback-renderer-config.schema.json`.

#### Command-line tools

- New on both front-ends: `transcribe`, `decompose-stems`, `suggest-mix`, `repair` (`--detect`, `--explain`), `tune-to-midi`, `project align-takes`, `polyphonic-notes`, `polyphonic-render` and `playback`.
- `midi-render` is now native; `mix-strip` and `split-silence` are now on Python; the native CLI gains the mastering preset list, the assistant's audio profile, the streaming-platform preview, `scale-quantize`, `note-move` and `pitch-correct-timevarying`; Python gains `sections`, `mastering-pair-processor` and `mastering-stereo-analyze`.
- `project bounce` binds audio with `--audio <source_id>=FILE` or `--resolve-audio` for `file://` URIs.
- `project bounce` and `midi-render` accept `--channels 6` and `--channels 8`.
- `split-silence` takes several takes (`--input`, `--write-takes`) and explains its intervals with `--report`.
- `boundaries` gains `--absolute-threshold`, `--n-mfcc`, `--n-chroma`, `--no-mfcc` and `--no-chroma`.
- Python `mix` addresses inputs as `--input ID=WAV`, Python `master` takes `--assistant`, and `suggest-mix --scene-out` writes the scene.
- Both front-ends accept `--flag=true`, the native parser takes an attached short-option value, and `--seed` accepts `[0, 4294967295]`.

#### Distribution and build

- The C++ library installs: `cmake --install` places the archives, headers, CMake package files and CLI, and `find_package(sonare)` gives `sonare::sonare` plus per-subsystem targets; `sonare.pc` is installed for the shared build.
- The C ABI header stays at `<sonare/sonare_c.h>` and the C++ headers install under `include/sonare/cpp`; Eigen is not a usage requirement.
- The vendored FFT archives install as `libsonare_kissfft.a` and `libsonare_pffft.a`.
- The capability descriptor's `features` reports eleven build gates, adding `mixingAssistant`, `instrumentParamAutomation`, `arrangement`, `acousticSim`, `playback`, `pitchEditor` and `voiceChanger`.
- A build tree survives an FFmpeg upgrade, and the installed `sonareConfig.cmake` does the same for a consumer.
- `LICENSE` and `NOTICE` ship in the wheel, npm package and CLI tarball; Eigen is built with `EIGEN_MPL2_ONLY`.
- The macOS CLI asset targets macOS 14.0.
- The full WASM module grows from 4,065,446 to 6,206,371 bytes (2,106,722 compressed); the analysis-only module is 1,023,564 bytes (382,215 compressed).
- The WASM package ships one declaration file per source module, and the npm tarball no longer carries declarations for deleted sources.

### Behaviour changes

#### Mastering and repair

- Twenty-two of the twenty-five mastering presets v1.7.2 shipped produce different output for the same input.
- The mastering assistant no longer guesses a genre and starts from the named preset (`streaming` by default); outside repair its chain no longer adds stages from the measured profile, and the speech de-esser and mono fold follow the preset.
- The loudness stage now drives the limiter toward its target, so presets separate in loudness as their targets say.
- The true-peak limiter enforces its ceiling per sample, so output no longer depends on block size; a meter finer than the limiter reads about 0.02 dB over the ceiling.
- The true-peak limiter adds a final output guard and its latency grows; oversampled stages share a flatter filter, moving 4x latency from 12 to 48 samples and the 1 ms limiter from 71 to 107.
- Detect-only true-peak limiting and the maximizer's adaptive release no longer depend on block size, and the streaming loudness limiter honours `release_ms = 0`.
- The tape and transformer hysteresis stays stable up to +24 dB drive, lowering peaks there.
- The denoise default floor is 26 dB; repair runs declip before declick and dereverb after denoise; stereo repair is channel-linked.
- Hum removal defaults to subtraction, the declick fill is anchored at both ends, dither quantizes in every mode, a 6 dB/oct parametric shelf is first-order, and match EQ spreads bands on a curve without extrema.
- The FIR match EQ returns to unity outside the matched band.
- Any one key of an EQ band creates the band on every EQ family.
- A presence enhancer with zero drive is accepted and adds no harmonics.
- Processors substitute or discard a NaN or infinity and keep running; `power_to_db` and `spectral_contrast` report NaN for an empty bin, and the audio writer writes a non-finite sample as silence.

#### Analysis

- `cqt`, `vqt`, `pseudoCqt` and `hybridCqt` return librosa's magnitudes, 47–94 dB higher than before.
- Every decoder folds multichannel to mono by ITU-R BS.775 without the LFE: a 5.1 WAV or MP3 reads about 9.5 dB louder, and every FFmpeg-decoded file, stereo included, about 3 dB quieter; a mono fold of a stereo WAV or MP3 is unchanged.
- Chord recognition weighs the bass register, recognises eight new qualities, grades cadences by quality, and no longer depends on the sample rate.
- `Key.confidence` is a softmax over the candidates and no longer saturates at 1.0.
- Downbeats land on the accented beat.
- Song structure no longer invents sections in uniform material, reports unsupported sections as `Unknown`, can report `Bridge`, and a `minSectionSec` floor no longer collapses sections shorter than it.
- `segmentRecurrenceMatrix` and lag conversion, YIN/pYIN period range and onset backtracking follow librosa; onset detection with a config resamples to the analysis rate.
- Straight sixteenths no longer read as swing, `syncopation` no longer saturates, 3/4 is not promoted to 6/8 without evidence, and an unsearched meter reports confidence 0.
- One-shot chord entry points honour `useBeatSync`, `timbre.roughness` no longer scales with level, and a banded DTW narrows its band.
- NNDSVD seeding in `decomposeStems` and `decomposeWithInit` / `decompose_with_init` / `sonare_decompose_with_init` (and `decompose` on Node) runs in double precision and returns different factors; the default `"random"` is unchanged.
- `pitchCorrectToMidiTimevarying` and `pitchCorrectTimevarying` no longer scale correction by `voicedProb`, so low registers are corrected more strongly.
- `noteStretch` changes the clip length by exactly the stretched region's change and leaves the rest untouched.
- Synthesized room responses decay per band as designed and sound brighter; a band truncated by `maxSeconds` reports NaN.
- `estimateRoom` reports volume and dimensions as NaN (CLI `null`) when no decay is measurable.
- `bandScattering` applies whatever wall material was selected.
- A zero `reference_absorption` in `SonareRoomEstimateConfig` selects the default 0.15.
- The streaming analyzer's chord progression is keyed by bar.
- The FFT uses SIMD kernels, so results move by floating-point rounding.
- JSON parsing accepts subnormal numbers.

#### MIDI and synthesizer

- The built-in synth's GM fallback voices render differently: the acoustic piano is calibrated against a reference recording, and the other physical-model voices are still being tuned and will change in 1.8.x patch releases.
- The built-in synths act on GS SysEx, so a GS file sounds different wherever it uses system, part, controller or drum parameters.
- GS insertion-effect parameters now reach the audio; selecting an effect type loads its power-on parameters, so a parameter written before its type is lost, and overdrive, distortion and the guitar and bass multis read their drive and EQ from the correct slots.
- A GS variation bank selects its variation voice where one exists, and the rhythm-part program selects from 26 drum sets instead of 9.
- The six GM electric guitars play through an amplifier by default.
- `wire_threshold` is a fraction of full-velocity swing in `[0, 1]`, so snare-rattle settings change meaning.
- The fallback synth handles CC7, CC10 and CC11 with the other synths' laws and renders in stereo, and Reset All Controllers returns expression to full.
- A synth patch requesting the vocal body gets one, so GM programs 85 and 91 sound different.
- An explicit `gain: 0` on a synth patch renders silence.
- A MIDI 2.0 Note On with velocity 0 plays at velocity 1.
- A routed MIDI event keeps its UMP group, polyphonic aftertouch transposes with its note, and a malformed SMF track is recovered with the rest kept.
- Effects: the Dattorro reverb modulates fractionally in quadrature, the phaser's LFOs are a quarter cycle apart, the pitch shifter passes unity ratio through undelayed, the rotary tremolo peaks at unity, the velvet reverb spans its tail, and the amp sim's crossover no longer inverts.

#### Mixing and engine

- Strip EQ follows the scene's `eq`, so a resend without `eq` is flat; an identical strip resend no longer rebuilds inserts, and a bus reorder carries state by id.
- A bus, send or key feeding a narrower destination downmixes by ITU-R BS.775 instead of keeping only the front pair.
- The mixer graph and project bounce render a surround bus at its width and the master at the output width.
- MIDI clip gain and fades now apply to the rendered instrument.
- A group bus's non-linear inserts run once on the summed signal, and lane solo and mute ramps take their documented time.
- Lane sends are tapped after delay compensation and follow the fader and gate.
- A hosted synth lane's fader, mute and gate reach its share of the reverb, insert effects and other shared stages.
- Clearing an automation lane returns its target to the last manual value, or a strip insert to its built value; telemetry code 21 (`PARAMETER_BASE_OVERFLOW`) reports manual values past the tracked limit.
- `setAutomationLane` with no points removes the lane.
- `mixStereo` applies pan and pan mode independently, and the engine's master true-peak meter measures instead of reporting the floor.
- Every voice-changer preset renders at a different level, and its latency grows by one grain; on WASM a partial retune configuration merges with the current one.

#### Command-line tools

- `mastering-processor` processes both channels of a stereo file and writes two channels.
- `normalize` keeps a stereo file stereo with one gain.
- `pitch-correct` applies the whole requested interval.
- Shared commands publish the same `--json` keys and units, Python no longer rounds JSON values, decimals ignore the host locale, native `chords` gains `quality`, and project subcommands report on stdout.
- The Python CLI quantizes PCM as the native writer does and writes more than two channels as `WAVE_FORMAT_EXTENSIBLE`.
- A global option is no longer replaced by its registry default, a repeatable option before the command word is kept, and a repeated value containing a comma is not split.
- The native CLI runs `acoustic` blind as other surfaces do.
- `voice-change` and `vqt` report the formant factor and gamma actually applied.
- `repair` fits declipped output to full scale with one gain, reported as `output_gain_db`.

### Fixes

- Dropping a sidechain key no longer leaves the processor reading a freed buffer.
- The Python `SendTiming` stub had its values backwards; it now matches the runtime.
- `sonare melody --hop-length 0` no longer hangs.
- `chroma_dtw_align` no longer rejects every call.
- `remix` with `alignZeros` no longer returns an empty result for material without zero crossings.
- An oversized Mel band count no longer writes past a buffer.
- Clipping detection no longer hangs on NaN.
- The realtime clip streamer no longer renders a block of silence at every page boundary.
- A realtime NativeSynth no longer drops `useGmPrograms`.
- BuiltinSynth honours MIDI 2.0 registered controller 0/0 (bend range).
- Held treble notes on the plucked-string programs no longer grow without bound, and the sympathetic strings follow the sustain pedal.
- Decays and the flute and organ pitch no longer shift between 44.1 and 48 kHz.
- An SF2 with more than 32767 samples or instruments loads.
- Raw ADTS AAC and ID3-tagged FLAC or AAC reach the FFmpeg decoder, and pipes and other non-seekable input load.
- Long audio no longer drifts out of tune in pitch correction, scale labels no longer flip a semitone, and note editing reads a non-positive or non-finite F0 frame as unvoiced.
- A fully reflective band synthesizes a late tail.
- A streaming analyzer past 2^31 samples still finalizes.
- The amp sim reports its cabinet IR in its tail, so offline bounces no longer cut it, and reserves state only for the channels it processes.
- `stereo.monoCompatCheckLogBands` measures each whole band.
- The linear-phase EQ returns to its partitioned path after a ragged block.
- Both CLIs accept the band keys of `eq.minimumPhase`, `eq.linearPhase`, `eq.dynamic`, `eq.midSide` and `multiband.dynamicEq`.
- WASM `RealtimeEngine` methods accept an omitted trailing render frame.
- WASM returns plain arrays and objects, so meter readings can be posted to a worker, and delivers each meter record once per interval; the Node project facade accepts `curveToNext`.
- A warped loop period is no longer clamped to the source length, the playhead folds into the loop before a block renders, and a tempo-sync clip accepts `length_samples == 0`.
- A failed engine prepare leaves the engine unprepared.

### Performance

- The FFT runs on SIMD kernels (PFFFT), roughly halving STFT time and cutting `analyze()` by about a third; `-DSONARE_USE_PFFFT=OFF` restores KissFFT, which the WASM build keeps.
- Offline LUFS memory is bounded by the 3 s short-term window regardless of clip length.
- True-peak measurement skips interpolations that cannot exceed the peak found so far, about 10x faster on material with transients; results are exact.
- `hpssWithResidual` peaks at 3.5 spectrograms instead of 8.5, the offline formant warp and `griffinLim` build their FFT plans once, and the offline mono mastering chain releases its measurement copies early.
- The offline linear-phase EQ runs 5–152 times faster at its largest resolution.
- In a browser, `analyze()` completes on songs up to about eight minutes.

## v1.7.2 (2026-08-18)

This release opens three paths a host could describe but not take: routing the cue bus to its own AudioWorklet output, running the streaming mastering chain inside the worklet realm, and carrying a selection across a destructive MIDI-FX bake. The ABI is unchanged and no existing call behaves differently.

### New

- The WASM zero-copy realtime path can separate the cue bus from the program output. `prepareMonitorChannels`, `getMonitorChannelBuffer` and `processPreparedWithMonitor` are the monitor-tap counterparts of `prepareChannels` / `getChannelBuffer` / `processPrepared`, and the AudioWorklet node takes a `cueOutput` option that gives the processor a second output fed by the PFL/AFL tap. Per-track monitor modes have been settable since v1.7.0, but the worklet render path only ever called the plain `process`, which folds the cue into the program mix — so a host could select PFL and still have nowhere for the cue to go. The copy-in `processWithMonitor` and the C entry point it mirrors were already available and are unchanged; a node built without `cueOutput` keeps one output and the folded mix, sample for sample.
- `StreamingMasteringChain` is exported from the AudioWorklet entry, so a live mastering preview no longer has to round-trip audio to the main thread. Its class documentation now states the contract that reaching the render thread makes load-bearing: `prepare` allocates and belongs in a message handler, an enabled loudness stage still requires the offline-measured `loudnessStaticGainDb`, flush output leads by the reported latency, and the chain is a host-side stage outside the engine's own delay compensation.
- `sonare_project_bake_midi_fx_ex` reports where each baked event came from: one index per transformed event naming the input it derives from, in the same canonical order the bake commits. Chord and arpeggiator fan-out lands several outputs on one source index, which is what lets a host tell the surviving event from the newly generated ones and keep a selection or an editorial annotation across the bake. `sonare_project_preview_midi_fx_count` runs the same deterministic transform without mutating the project, so the buffer can be sized exactly. Node and WASM fold both into a request form of `bakeMidiFx` plus `previewMidiFxCount`; Python takes a `with_source_index` keyword and `preview_midi_fx_count`. The positional and keyword forms without provenance are byte-identical to before, and only a caller that asks for the map pays for the narrower drain that produces it.

### Documentation

- The insert-automation resolvers say which mastering processors they cover. The `eq.*`, `dynamics.*`, `saturation.*`, `spectral.*`, `stereo.*`, `maximizer.*` and `multiband.*` processors are all available as strip inserts, so resolving one through `sonare_engine_resolve_track_insert_automation_id` and its master and bus counterparts already drives it at audio-block precision. The stages with no automation id are the whole-signal ones — `repair.*`, `loudness` and the match stages — which buffer the entire signal and do not run on the realtime path at all.

## v1.7.1 (2026-08-15)

**The feature ABI moved from 3 to 4.** `SonareSynthPatch` gained a trailing `present_fields` word, so the Python binding refuses a shared library built from a different tree; rebuild the library alongside the binding. Existing C callers are otherwise unaffected, at both the source and the call level: the new field is read only when `struct_version` is 2 or higher, so a struct filled as before keeps its previous behaviour. On Python, the `SynthPatch` numeric fields default to `None` instead of `0.0`, so reading a field that was never set returns `None` rather than zero.

This release restores the offline formant path, which lost the whole LPC prediction gain, adds stereo variants of the mastering analysis entry points, lets a synth patch override a field with an explicit zero, and corrects two JavaScript declarations that rejected input the runtime accepts.

### New

- Mastering analysis reads both channels through `sonare_mastering_audio_profile_stereo`, `sonare_mastering_assistant_suggest_stereo`, `sonare_mastering_streaming_preview_stereo` and `sonare_metering_crest_factor_db_stereo`, which take the planar left and right pair the rest of the stereo mastering surface uses. The mono entry points required a `0.5*(L+R)` downmix, which reads 6.02 dB below the BS.1770 channel sum on decorrelated material and cancels entirely on an anti-phase pair; the streaming preview derived both its normalization gain and its ceiling-risk flag from that loudness, so the error reached the verdict a host shows its user. Only the profile's loudness block is measured from the channels — the spectral, dynamics and tempo fields describe shape and timing rather than absolute level, so they stay on the downmix and remain comparable with the mono entry point field by field. Mirrored as keyword arguments on Python and as request objects on Node and WASM.
- `SonareSynthPatch` distinguishes a field set to zero from an omitted one. Every numeric field previously read zero as "keep the base", so a host could not ask a preset for no stereo spread, no bus drive or a zero sustain. `present_fields` under `struct_version` 2 names the fields the caller set on purpose: a set bit overrides even when the value is zero, a clear bit keeps the version-1 behaviour, and the modulation-matrix bit lets an empty routing table clear the base matrix rather than keep it, which the routing count alone could not express. The preset read direction reports every bit set, so a preset field that happens to be zero survives a round-trip instead of decaying into "keep base". Node and WASM set a bit for each key the descriptor actually carries, and Python's dataclass fields default to `None` so a supplied zero is a real override.

### Fixes

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

### New

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
- The GM fallback voicing tables and the preset catalogue's fallback aliases no longer compile to start-up code that fills them field by field: the module's code section is 12% smaller and the compressed download nearly 5% smaller, and a voicing edit now costs the bytes of the values it changes rather than the instructions that would have written them.
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

### Fixes

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

### Behaviour changes

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

### New

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

### Fixes

- Every C-ABI getter that builds a `thread_local` string, and the EQ and scene-JSON factories, return null with a diagnostic on allocation failure instead of escaping the ABI boundary; the two audio-thread process entries stay free of `thread_local` diagnostics. Quick-analysis output arrays are staged in temporary owners so a later allocation failure cannot leak the arrays already built.
- One shared error-code table backs the C ABI, Node and WASM instead of per-binding copies, including the previously missing cancellation mapping.
- The voice changer moves every live control onto per-sample smoothers, so adopting a config snapshot no longer steps values at a block boundary. The retune and whole-chain dry paths align to the overlap-add latency, so reported latency is fixed instead of scaling with the wet and retune mixes. The inter-sample-peak limiter gained a delayed detector with attack-lead gain compensation and no longer hard-clips base-rate samples, which recreated the peaks it was meant to prevent. Only the formant frequency displacement scales with the formant amount, so body, brightness and nasal still apply at amount zero.
- Preset ids, complete schema documents and the flat camelCase POD route through one shared parser used by the C ABI, Node and WASM, rejecting partial documents that previously fell back to unrelated defaults.
- The CoreAudio render callback zeroes the active output scratch before each block; a callback that only called the engine replayed the previous block's samples. The CoreMIDI SysEx staging ring uses release/acquire single-producer single-consumer cursors instead of a mutex, so the MIDI callback performs only bounded copies and can never block behind the control thread.
- Alignment delay is bounded by a named maximum applied to both the integer and Q8 setters, and out-of-range requests are rejected at the C-API channel-delay setter instead of silently clamped.
- The wah and auto-wah sweep is clamped below the SVF stability limit, and `frequency_to_w0` no longer inverts its clamp at low sample rates.
- Engine SysEx payloads are bounded at 512 bytes on both the C ABI and the WASM path.

### Behaviour changes

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

### New

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

### Fixes

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

### Behaviour changes

- The `phaseVocoder` positional signature on Node and WASM is `(samples, sampleRate, rate, nFft, hopLength)`; `sampleRate` and `rate` were previously the other way round. Both are numbers, so an existing positional call still type-checks and will silently pass the wrong values. Pass a request object, or swap the two arguments.
- The 7.1 plane order changed to `L R C LFE Ls Rs Lss Rss`. Callers that compensated for the previous swapped side/back order must drop that compensation.
- Analysis defaults changed to match librosa: the YIN and pYIN voicing threshold is `0.1` (was `0.3`), VQT `gamma` defaults to `-1.0` for the automatic ERB-derived value (was `0.0`, standard CQT), and `chroma_cqt` analyses 252 CQT bins at 36 bins per octave (was 84 bins at 12). `chroma_cqt`'s `threshold` is now an absolute magnitude rather than a fraction of the per-frame maximum, so an existing non-zero value means something different.
- Other defaults moved to match the core: the peak-pick trailing windows `postMax` and `postAvg` default to `1` (was `0`, which suppressed the trailing comparison), note-edit offsets default to the input length, and the metronome click length is derived from the sample rate when left at zero.
- Node and WASM reject inputs they previously accepted, matching the C ABI: negative pad, fix-length and clipping-region sizes, non-finite samples, out-of-range scale masks, and pitch tracks whose voiced array length does not match `f0Hz`.
- The Node pre-chorus section label is spelled `Pre-Chorus` instead of `PreChorus`, so consumers matching on the previous spelling need updating.

## v1.5.4 (2026-07-22)

This is a follow-up release to v1.5.3, adding a musical-beat playhead and configurable undo history to the realtime and project surfaces, a physically motivated air-absorption term for large-hall reverberation, and a round of realtime-safety, voice-changer and CLI/binding correctness fixes.

### New

- The engine transport snapshot now reports the musical `beat` (one-based) and `beat_fraction` (in `[0, 1)`) alongside the existing bar index, so hosts can render a bar:beat:tick playhead. The C `SonareTransportState` grows the two fields (appended after the time signature to preserve existing offsets) and the Node, Python and WASM readers plus their type declarations surface them.
- The project edit history exposes a configurable undo depth and an explicit history reset across every surface: `sonare_project_set_max_undo_depth` / `sonare_project_clear_history` on the C ABI, mirrored as `setMaxUndoDepth` / `clearHistory` (Node, WASM) and `set_max_undo_depth` / `clear_history` (Python). Shrinking the depth evicts the oldest entries immediately (clamped to at least one), letting callers trade undo history for resident memory or reset it between sessions.

### Acoustic model

- Reverberation time gains an optional atmospheric-absorption term: an ISO 9613-1 pure-tone air-absorption coefficient feeds the `4mV` denominator of the Sabine/Eyring `shoebox_reverb_time`, shortening the high bands of large halls the most to match the physical air roll-off. The parameter is opt-in and defaults off, so the geometry-only reverberation tail is byte-identical.
- Early reflections are coloured per octave band and the polyhedral image-source search is bounded, so non-shoebox rooms render more accurately without unbounded reflection enumeration.

### Fixes

- Realtime audio output is hardened against non-finite state and torn reads, and hot-path scratch buffers are reused to remove a realtime allocation on the render path.
- The voice changer applies a flat configuration POD through `setConfig` on Node and WASM, accepts a narrower interleaved channel count on WASM, folds `retune.mix` into its reported realtime latency, and matches the C-ABI error contract on the WASM path.
- Section analysis runs on the shared analysis-rate signal, and a handful of DSP / analysis parity and edge-case bugs are corrected in the core.
- The realtime engine seeds its fallback tempo map, and the embedded-scene version mismatch is classified as an explicit error.
- Arrangement editing restores split-MIDI order exactly and skips redundant store clones on undo; the composition assistant budgets its iterations by consumed count rather than slot count.
- Direct-call bindings require an explicit sample rate and validate WASM inputs, and the Python CLI corrects its output handling and surfaces project diagnostics.
- Streaming computes the per-frame smoothed chord once per frame.

## v1.5.3 (2026-07-21)

This release is a cross-surface input-validation and realtime-safety hardening pass over the offline, streaming, CLI and macOS-host paths, rounded out by a request-object call form for the one-shot JS facades and a handful of additive analysis, engine and mastering surfaces.

### New

- The top-level one-shot analysis, effects, mastering, metering, feature and mixer/voice-changer functions accept a request object as their canonical call form, so each input is named and optional settings can grow without disturbing argument order. Positional signatures remain as compatibility overloads and normalize through the same path, keeping defaults, validation, errors, results and progress behaviour identical between both forms. Mirrored on Node and WASM with matching field names and defaults (the WASM entry point also re-exports the request-object types); the embind and N-API calls underneath stay positional, and Python keeps its idiomatic keyword arguments.
- Every mastering result now reports the chain output true peak (dBTP, at the chain's configured oversample factor), the output loudness range (LRA) and per-stage gain reductions, surfaced as a `StageGainReduction` type on Node, Python and WASM so callers can confirm a preset ceiling was met without a second oversampled scan.
- The realtime engine gained `sonare_engine_set_tempo_segments` / `sonare_engine_set_time_signature_segments` (Node `setTempoSegments` / `setTimeSignatureSegments`, Python `set_tempo_segments` / `set_time_signature_segments`, and the WASM equivalents), letting callers install a piecewise tempo / time-signature map instead of a single value; an empty list clears the map back to the single value.
- Streaming frame results expose `feature_flags` and `n_chroma` so consumers can tell which arrays are physically present; disabled features emit empty arrays with zero strides instead of implied full widths, across the C ABI, Node, Python and WASM.

### Fixes

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

### Behaviour changes

- The Node and WASM acoustic room APIs now reject a per-band absorption or scattering coefficient outside `[0, 1]` (or non-finite) with an invalid-parameter error instead of clamping it, matching the C ABI and every other surface. Callers that relied on out-of-range values being silently clamped must pass in-range coefficients.
- Pure-analysis CLI commands reject an `-o` / `--output` destination (they print to stdout), and audio-rendering effect commands now require one on both the native and Python CLIs. Scripts that passed `-o` to an analysis command, or omitted it from an effect command on the Python CLI, will now receive a parameter error.

## v1.5.2 (2026-07-16)

This release adds a spectral-reconstruction path and a handful of additive analysis, project and streaming surfaces, and continues the v1.5.1 hardening pass across the mastering, mixing, MIDI-import and realtime-thread paths.

### New

- `sonare_griffinlim_cqt` / `sonare_griffinlim_vqt` reconstruct a time-domain signal from a constant-Q or variable-Q magnitude spectrogram via Griffin-Lim, callable on the Node, Python, WASM and C-ABI surfaces.
- The chord analyzer now reports an explicit no-chord (N.C.) interval whenever the frame correlation falls below the detection threshold, surfaced on every binding instead of silently dropping the segment.
- Compound clip edits go through the C-ABI project surface as a single undo transaction, so a multi-clip operation is undone or redone in one step across Node, Python and WASM.
- The mastering processor catalog reports each insert's decay-tail length alongside its latency, wired into the typed Node, Python and WASM surfaces.
- The Node realtime voice changer gained an explicit `destroy()` so its native resources can be released deterministically rather than on GC.
- The WASM entry point re-exports the `ExternalMidiEvent` type.
- Insert-automation scheduling failures are now classified through the mixing C ABI instead of returning a single opaque error.

### Fixes

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
- The amp-sim insert gained a circuit-level preamp topology (a cascade of triode stages into the real passive tone-stack ladder inside one oversampling region, where the tone controls interact and lose level the way the network does), a microphone stage on the cabinet with capsule voicing, off-axis position and distance plus an optional second mic that combs against the first through their path-length difference, a speaker cone stage with suspension nonlinearity and excursion-driven Doppler, a class-AB crossover dead zone, and blocking distortion. Every one is off by default and bit-identical when off; `topology` stays voiced-filter, so the shipped sound does not move.
- **The amp's three least tangible controls now carry physical units.** `powerTube` scales the power stage from each tube's maximum plate dissipation (6L6GC 30 W, EL34 25 W, 6V6GT 14 W, EL84 12 W) rather than from a chosen table — the 6L6 scale is exactly 1.0, so the default stage is untouched. `crossover` is anchored on idle plate dissipation as a fraction of maximum, which is the number an amp is actually biased to. `sag` is now the fractional rail droop at full output (`I·R/B+`), so the same setting no longer means twelve different things depending on the drive and power knobs; it is gated on `power` for the same reason `nfb` is, since a supply with no output stage draws no plate current. The supply model is corroborated rather than fitted: one free constant, the power transformer's secondary resistance, is solved independently by two published rail drops and lands within a volt of both.
- Ten named amp rigs — `cleanCombo`, `chimeEdge`, `classicCrunch`, `tweedGrind`, `britStack`, `modernLead`, `rectifierChug`, `coldBiasBuzz`, `bassDi`, `bassRig` — reachable as `"preset"` in the insert's param bag, where the named rig is the base the numeric params ride on. Presets exist because six of the insert's switches (topology, preamp stage count, power tube, amp / cab / mic model) are construction-time and no automation lane can reach them. Their supply and bias numbers are derived from the rectifier and bias point rather than chosen, which is why the tweed's 5Y3 comes out the saggiest of the ten without anyone deciding that it should.
- **A cabinet impulse response can now be synthesized instead of supplied** (`cabIrGenerate` in the param bag), so an IR cab is available without sourcing a recording — the same stance the convolution reverb already takes. What it adds over the analytic cab is the cabinet's geometry: a microphone in front of one driver also hears the others, later, quieter and off their own axes, and the summation is a comb whose spacing follows the mic distance. The miked driver's response is the analytic voicing unchanged, so with `cabIrDrivers` off the generated IR reproduces it to better than 0.001 dB; the neighbours add spherical spreading, path delay and rigid-piston directivity, and nothing else. The whole cabinet is matched to the single driver's energy rather than to its low end, which keeps the low-frequency coupling gain that makes a multi-driver cabinet darker than one of its own drivers.
- A cab IR loaded at another rate is resampled instead of transposed — a 48 kHz capture used in a 96 kHz session was previously played an octave high, silently — and the retained length is now a duration rather than a sample count, so the same IR no longer voices a shorter cabinet as the session rate rises. A non-finite sample anywhere in the source is refused, including past the truncation point.
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

### Fixes

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

### Fixes

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

### Fixes

- Engine: warped mid-clip comp parts no longer double-offset the source read; `time_to_frames` saturates instead of casting an out-of-range float to int; lane remap is skipped on an unchanged config in hot mixer commands; block-final automation values are preserved past the per-block event cap; engine markers are staged atomically to avoid a use-after-free on rejection; and the compiled graph topology is invalidated when sidechain ports change.
- MIDI: `MidiFxChain::process` sorts its fixed-capacity output buffer in place (binary insertion sort) instead of via `std::stable_sort`, which requested a temporary heap buffer — restoring zero heap allocation on the audio thread while keeping the same render-frame / off-before-on event ordering.
- Mastering: `dynamics` `set_parameter` is RT-safe via an in-place working config (with a noexcept in-place `release_ms` setter); standalone `loudnessOptimize` honours `releaseMs` and `applyGainAtInputRate`; and mastering name-getter return pointers are stabilized across repeated calls and gated on a write-once flag.
- Mixing: VCA group offset accumulates with an atomic read-modify-write; send timing defaults to post-fader across all surfaces.
- Analysis & util: `OnsetAnalyzer` detects flat-topped onset peaks; DTW/RQA and NNLS guard integer index overflow; the acoustic image-source reflection order is clamped.
- Audio I/O: WAV write rounds float samples to the nearest PCM integer.
- Validation hardening: the Node addon throws on an invalid compressor detector instead of falling back; WASM rejects a voice-changer channel count that differs from the prepared layout, rejects non-positive melody/sections params, aligns detailed-analysis config validation with the C ABI, validates offline audio input through a shared core helper, and validates engine bounce/freeze/lane inputs against the C-ABI oracle; `StreamAnalyzer` rejects malformed config geometry on every surface; and undoing `RemoveMarker` restores all marker fields.

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

### Fixes

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

### Fixes

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

### Fixes

- Graph nodes size their sidechain channel storage in `prepare()`, removing a potential out-of-bounds access at high port counts.
- The Python CLI bounce WAV writer supports arbitrary channel counts, fixing surround (>2 ch) bounce output.
- The Python engine retains references to active `ClipPageProvider` objects so they are not garbage-collected while clips still use them.
- WASM Web MIDI: `requestMIDIAccess` is invoked with the correct `this` binding, running status is cleared on system-common messages, and incomplete channel-voice messages are dropped.
- WASM live audio: extra `getUserMedia` constraints from the options object are forwarded to the native call, and `stopTracksOnClose` reliably defaults to true.
- The Node binding reads `startPpq` for offline track freeze as a double, preserving sub-millisecond precision, and exports the `EngineCaptureSource` type alias.

## v1.3.0 (2026-06-06)

### New

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
  - New versioned `SonareSynthPatch` C struct: the base is a named catalog preset (or the default subtractive patch) and every non-zero field overrides the wrapper sections all engines share (oscillator / filter model / envelopes / LFOs / glide / body / stereo spread / mod matrix / bus). The engine-mode field selects any of the seven synthesis engines.
  - Named preset catalog (`sonare_synth_preset_names` / `sonare_synth_preset_patch`): sine, saw-lead, square-lead, sub-bass, warm-pad, e-piano, bell, brass, pluck, electric-guitar, harp, marimba, glass, organ, drum-kit and acoustic-piano — data-only patches over the voiced GM fallback bank. The `drum-kit` preset plays the full GM drum map (note-on resolves the struck key's kit piece).
  - Offline bounce (`sonare_project_bounce_with_synth_instruments`) and a realtime engine entry (`sonare_engine_set_synth_instrument`) alongside the existing built-in/SF2 instruments — live MIDI input plays NativeSynth patches.
  - Python (`SynthPatch` / `synth_preset_names()` / `Project.bounce_with_synth_instrument` / `RealtimeEngine.set_synth_instrument`), Node and WASM (`SynthPatch` / `synthPresetNames()` / `project.bounceWithSynthInstrument(s)` / `engine.setSynthInstrument`) facades accept a preset-name string (a `"va:"` routing prefix is accepted) or a patch object with shared enum names.

- Added the NativeSynth realism-polish layer:
  - Body/formant resonance on every voice (the cheap end of commuted synthesis): unit-peak-normalized low-Q bandpass mode banks voiced as a guitar body, a violin body or the note-tracked wood tube under a marimba/xylophone bar, mixed over the dry voice. The GM acoustic guitars, harp and wooden mallets now carry their bodies (solid-body electrics intentionally do not).
  - Seeded per-voice stereo spread: a deterministic pan scatter per voice (0 keeps every voice centre-panned bit-exactly); the GM string, choir, organ and pad families spread into a section image.
  - Mix-bus glue: an optional gain-neutral tanh bus drive plus an always-on (config-defeatable) DC blocker that keeps the physical-model voices' small DC components off the output bus.

- Added a `effects.modulation.ensemble` insert — the Solina-style BBD string-machine ensemble: three delay taps per channel swept by a slow and a fast 3-phase LFO bank simultaneously, with the BBD bucket-bandwidth lowpass on the wet path and inverted right-channel LFO polarity spreading a mono source into stereo. Exposed through the insert factory and the automatable set_parameter surface on every binding.

- Added an extended-waveguide acoustic-piano mode to the NativeSynth voice — the no-SF2 data-free grand sketch. The four piano-defining elements are all present: stiff-string dispersion via an allpass cascade in each waveguide loop (partials stretch sharp, the inharmonicity growing up the keyboard, with the exact loop phase delay compensated so f0 tuning stays accurate), a nonlinear felt hammer (Hertz-contact velocity scaling of contact time and force plus a felt-stiffness lowpass — hard strikes are shorter and brighter), 2-3 coupled micro-detuned unison strings with the characteristic two-stage prompt-sound/aftersound decay, and a fixed soundboard resonator bank that also radiates the immediate hammer knock. The GM acoustic-piano programs play through it.

- Added modal, additive and percussion synthesis modes to the NativeSynth voice, completing the mallet / organ / drum coverage of the data-free GM floor:
  - Modal resonator bank with physical mode-ratio data (uniform-bar glockenspiel 1:2.756:5.404:8.933, deep-arch marimba/vibraphone 1:4:10), mallet-hardness velocity weighting, per-mode decay scaling, decay stretching and note-off damping; the chromatic-percussion mallets (glockenspiel, vibraphone, marimba, xylophone) now ring as modal bars.
  - Additive drawbar organ: the nine Hammond drawbar pitches with stepped stop levels, seeded free-running partial phases and the key-click contact transient; the GM organ family plays a drawbar registration.
  - Membrane percussion: Rayleigh circular-membrane modes (1:1.59:2.14:2.30:2.65) with a descending strike-pitch envelope layered under seeded filtered noise; the GM drum kit (kick, snare shell + wires, toms, hats, cymbals with inharmonic ring modes) is rebuilt on it, still one-shot and bit-deterministic.

- Added a Karplus-Strong plucked-string mode to the NativeSynth voice (the guitar / harp / banjo family): a fractional-delay waveguide loop with phase-exact tuning compensation, plus the Jaffe-Smith realism extensions — decay stretching (low strings ring longer), a pick-position comb on the excitation, a velocity-driven dynamic-level lowpass (hard pluck = bright) and note-off loop damping (finger/palm mute). The GM fallback bank now plays the guitar family (nylon / steel / jazz / clean / muted / overdriven / distortion), the orchestral harp and the plucked ethnic family through KS patches.

- Added a `saturation.ampSim` guitar amp insert to the mastering insert factory (drive -> tone stack -> cab-EQ): an oversampled 12AX7 triode drive stage behind one [0,1] drive knob with a drive-scaled pre-emphasis shelf, bass/mid/treble tone controls, and a fixed data-free cab voicing (low cut, body bump, presence peak, steep 4.8 kHz roll-off) that can be bypassed for a DI tone. Reachable from every binding through the existing mastering-insert names surface, with drive/tone/presence/level automatable via `set_parameter`.

- Added an FM synthesis mode to the NativeSynth voice (the e-piano / bell / brass / clav family): a 2-4 operator phase-modulation stack with a small algorithm table, exponential operator envelopes, a feedback operator, velocity-to-index (brightness) scaling and key-rate scaling (higher notes decay faster). The GM fallback bank now plays electric pianos, clavi/harpsichord, the chromatic-percussion bells and the brass family through FM patches.

- Added a modulation matrix, a second LFO and glide/portamento to the NativeSynth voice: up to 8 free-form routings from envelopes / LFOs / velocity / key tracking / mod wheel / seeded per-voice random to pitch, filter cutoff, amplitude and stereo pan, on top of the hardwired patch modulations; portamento glides each new note from the channel's previous note through a one-pole pitch ramp. All modulation stays deterministic.

- Added selectable virtual-analog filter models to the NativeSynth voice — the core of each classic synth "character": TPT state-variable (SEM family), 4-pole transistor ladder (ZDF, saturating loop, self-oscillates), diode ladder (VCS3 / TB-303 family, coupled-stage ZDF, self-oscillates) and Korg35 Sallen-Key lowpass (MS-10 / early MS-20, self-oscillates) — plus a gain-compensated pre-filter drive stage per patch. All models stay stable and zipper-free under per-sample cutoff/resonance modulation and self-oscillation is deterministic; the GM fallback bank routes bass, brass and synth-lead families through the transistor ladder.

- Added a NativeSynth virtual-analog engine and made it the data-free floor of the SoundFont player — MIDI never renders silent for lack of data:
  - Antialiased PolyBLEP oscillators (sine / saw / square / triangle plus a seeded deterministic noise source), unison stacking up to 7 oscillators with seeded detune and per-voice pitch drift, a TPT state-variable filter (low/band/highpass) with cutoff envelope, velocity-to-brightness and keyboard tracking, and exponential DAHDSR amplitude/filter envelopes.
  - A patch-driven `NativeSynth` MidiInstrument (16 channels, sustain / channel-mode CCs, CC1 vibrato, CC7/11 gain, CC10 pan, pitch bend) built on the shared voice pool; rendering is deterministic (seeded per-voice variation, no RNG).
  - A GM fallback bank covering all 128 programs by family plus the GM drum map (one-shot kick / snare / hats / toms / cymbals / percussion), used by the SF2 player whenever a program is not covered by the loaded SoundFont — or no SoundFont is loaded at all. `bounce_with_sf2_instruments` and the realtime engine's `set_sf2_instrument` therefore no longer require a prior SoundFont load; the manifest keeps reporting the honest per-program backend (`sf2` vs `synth`).

- Added a GS-compatible SoundFont 2 instrument so MIDI arrangements render with real sampled sounds (the SF2 file is host-supplied data; nothing is baked into the binaries):
  - SF2 parsing and a 16-part multitimbral player: preset/instrument zone layering with generator/modulator semantics (volume + modulation DAHDSR envelopes, vibrato/mod LFOs, low-pass filter with velocity tracking, exclusive classes, loop modes), the SF2 default modulator set (velocity / CC7 / CC11 square-law gain, CC1 vibrato, CC91/93 sends), pitch bend with RPN 0 bend range, and deterministic voice stealing.
  - GS architecture on top: variation-bank fallback to the capital tone, bank-128 drum kits on channel 10, NRPN part edits (TVF cutoff/resonance, TVA envelope, vibrato) and per-note drum NRPNs, GS Reset / GM System On / "use for rhythm part" SysEx (recognised both from hosts and from SysEx events inside an arrangement), and reverb / chorus / delay send-return effects with a per-part drive insert.
  - New C ABI: `sonare_project_load_soundfont` (+ clear / preset count), `sonare_project_soundfont_manifest` (reports per-program source backend: SF2 or synthesizer fallback), `sonare_project_bounce_with_sf2_instruments`, and the realtime-engine pair `sonare_engine_load_soundfont` / `sonare_engine_set_sf2_instrument` so live MIDI input plays through the SoundFont. Exposed across the Python, Node, and WASM bindings.

- Added a headless DAW / arrangement runtime, exposed through a new project C ABI and across the Python, Node, WASM, and CLI bindings:
  - Author projects with audio and MIDI tracks and clips. Clip edits (add / split / trim / move), tempo, and routing changes all route through an undoable `EditHistory`, so `undo` / `redo` cover every mutation. Musical positions are PPQ (quarter notes).
  - Sequence MIDI 1.0 and MIDI 2.0 channel-voice events, set per-clip program / bank and a MIDI-FX chain, and route a track's MIDI to a host-instrument destination id.
  - Import / export Standard MIDI Files, plus a MIDI 2.0 Clip File (`SMF2CLIP`) format that preserves 16-bit velocity, 32-bit CC, per-note controllers, and bank-valid Program Change without loss.
  - `auto_tempo` detects and installs a project tempo from audio; `snap_to_grid` quantizes a PPQ coordinate to the project grid.
  - `compile` produces a renderable timeline with structured diagnostics, and `bounce` renders the project offline to interleaved float audio. Both are deterministic; project JSON serialization is byte-stable within one build.
  - New `sonare project` CLI subcommands: `abi`, `new`, `validate`, `compile`, `bounce`, `export-smf`, `import-smf`, `export-midi2`, `import-midi2`.
- Wired a flag-gated MIDI sequencer into the realtime engine and added audio / MIDI / plugin host integration seams for embedding hosts.

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

### Fixes

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

### New

- Added a geometric room-acoustics module (built with `BUILD_ACOUSTIC_SIM`):
  - `synthesize_rir` synthesizes a mono room impulse response from shoebox geometry, combining image-source early reflections with a deterministic, seeded late tail. Invalid geometry is reported via a diagnostics flag rather than an error.
  - `estimate_room` performs blind equivalent-room estimation from a recording or impulse response, returning volume, representative dimensions, direct-to-reverberant ratio, per-octave-band absorption/RT60, and an honest confidence score.
  - `room_morph` applies an offline room-character morph toward a target room (a creative effect, not dereverberation).
  - Streaming `RoomReverb` and `RoomMorphProcessor` engines are reachable through the generic insert API by name (`effects.reverb.room`, `effects.acoustic.roomMorph`).
- Exposed the new module across the C ABI, Python, Node, and WASM bindings.

### Concurrency & real-time safety

- `ClipPlayer::clip_count()` now reads a published atomic instead of calling the audio-thread-only `RtPublisher::acquire()`, removing a data race when a host polls clip count (via the C ABI / WASM) during playback.

### DSP & analysis correctness

- Room impulse-response synthesis now measures the early-reflection level over a window that excludes the direct sound, so the late tail is no longer over-scaled in small rooms; per-band late-tail noise is energy-normalized so the tail's spectral balance is set by the materials, not the filter bandwidth.
- `estimate_tuning` now thresholds piptrack peaks against a single global median (matching librosa); `pitch_tuning` returns the librosa bin left edge.
- `onset_strength` defaults to `detrend=false` and `tempogram` normalizes each column by its max (L-infinity), both matching librosa defaults. The internal beat/tempo/music analyzers opt into detrend explicitly, preserving behavior.
- Mel `delta` uses Savitzky-Golay `mode='interp'` at the frame edges; chord per-frame confidence is computed against the smoothed chroma used for the decision; BPM peak picking covers the full tempo range and no longer throws on a single-frame onset envelope; 6/8 syncopation no longer counts the secondary strong beat.

### Fixes

- `declip` now honors `lpc_blend`, blending the LPC estimate with the interpolation fallback instead of ignoring the parameter.
- Stereo dither / output-chain now uses a decorrelated per-channel seed instead of identical noise on both channels.
- Multiband processors built through the named/insert API now accept a custom number of crossover cutoffs instead of throwing.
- Time-stretch / pitch-shift honor `n_fft` / `hop_length` on the default spectral backend.
- Streaming analyzer construction clamps `magnitude_downsample` / `hop_length` to safe values, preventing a divide-by-zero from direct Node/WASM use.
- `mfcc_to_mel` can invert MFCC liftering when the lifter is supplied.

### Bindings & API consistency

- RIR synthesis exposes `late_model` (Sabine/Eyring), `mixing_time_ms`, and `crossfade_ms` across the C ABI and all bindings; the room estimator forwards its full acoustic config. Node and WASM acoustic entry points now validate sample rate and input like the C ABI / Python.
- The CLI gained `--max-seconds` (synthesize-rir, room-morph) and `--n-octave-bands` (estimate-room).
- The absolute-threshold trim is renamed `trim_absolute` to disambiguate it from the librosa-compatible relative-to-peak `trim`.

## v1.2.2 (2026-06-02)

### Breaking changes

- Replaced stdlib exceptions (`std::invalid_argument`, `std::logic_error`, etc.) with `SonareException` across the C API, RT, EQ, mixing, mastering, and WASM surfaces so all failures throw a single, catchable type.
- Unified the `AutomationCurve` enum across the engine and mixing modules; code referencing the previous per-module enums must use the shared definition.
- Aligned binding facade parameter names to the canonical C API and aligned the melody/section/acoustic analyzer defaults to the documented values, which changes keyword-argument names and default behaviour for existing callers.
- Unified the `bounceOffline` LUFS default between the C API and WASM bindings.

### DSP & analysis correctness

- Fixed EQ/saturation, stereo-image, gate, de-esser, maximizer, and formant DSP in the mastering and editing engines.
- Switched the `chroma_cqt` default norm to L-infinity and corrected the chroma `fmin`, chord decoding, and overlap growth in the streaming analyzer for librosa parity.
- Hardened numerical robustness in feature/core paths, replacing remaining raw constants with the centralised `util/constants.h` values.
- Added an FFT null guard and beat-tracker frame-bounds checks, a bus denormal guard, BS.1770 surround weighting, and denormal flushing in the voice changer.
- Added the missing `<cstdint>` include so `streaming_reverb` builds under GCC.
- TD-PSOLA now preserves duration: the output-epoch-driven synthesis loop maps each grain to the nearest analysis pitch mark, so a constant pitch shift no longer time-compresses sustained voiced regions.
- Fixed mono fold-down for FDN reverb, velvet reverb, chorus, and flanger, which previously wrote two wet signals to the same aliased output buffer.
- The true-peak meter uses the history-preserving (RT-safe) upsample path, fixing block-size-dependent inter-sample peak misses.
- HPSS soft masking applies the margin before the power (`margin^power`) to match the reference, and `hybrid_cqt` rescales the pseudo-CQT half to the full-CQT amplitude convention, removing the magnitude step at the split bin.
- VQT (`gamma>0`) builds the analytic sinusoid with the same `+sin` convention as CQT/reference, so its complex phase is no longer conjugated.
- Restored the `a==b => hash(a)==hash(b)` invariant for the chroma/CQT/VQT kernel caches (strict float equality with quantized keys), ending silent cache misses and rare wrong hits.
- Corrected the KeyAnalyzer profile normalization no-op, slash-chord bass detection, `iirt` frame-count off-by-one, and the metronome click step discontinuity (now fades in and decays to zero).
- GraphicEq clamps band centers below Nyquist so high bands no longer throw at low sample rates; stereo width uses the standard M/S law so widening no longer attenuates the center/mono component.
- `ChordChange` records the completed chord's own held confidence; streaming `compute_onset` now coerces `compute_mel` so BPM is no longer silently zero; short-term LUFS uses the spec 100 ms hop.
- Mastering tape/exciter color stages engage only when they would actually color the signal (explicit `enabled` wins; otherwise drive/saturation/amount above zero), instead of running at zero strength whenever merely mentioned.
- Hardened degenerate inputs: DynamicsAnalyzer floors the loudness window/hop to
  >=1 sample, the phase-vocoder helper rejects `n_bins<2`/zero hop/zero rate, `BoundaryList::clear()` resets the overflow flag, and the C-API `spectral_flatness`/`zero_crossing_rate`/`onset_strength` zero their out-parameters on the error path.
- `detect_key` now stable-sorts key candidates so silent/tonally-empty input deterministically yields the documented C-major fallback on every platform instead of a libstdc++/libc++-dependent winner.

### Real-time safety

- Fixed RT thread-safety across the engine, graph, mixing, transport, and automation modules; capped insert vectors and documented the `AutomationLane` SPSC contract.
- Tape oversampling and AdaptiveRelease no longer allocate on the audio thread (preallocated scratch; in-place release update), and `RealtimeEngine::bind_mixing_strip` is no longer `noexcept` since it allocates on the control thread.
- `monitor_runtime` size is now atomic with acquire/release ordering; `send_automation` returns `OUT_OF_MEMORY`/`INVALID_PARAMETER` consistently and `validate_stereo_pair` validates both channels.

### Performance

- Replaced the O(N) LRU promotion with an O(1) splice in the mel/chroma filter caches and optimised additional hot paths while hardening API boundaries.
- Streaming onset and full-chroma histories use a sliding-window deque (O(1) trim, bounded memory on long sessions), the graph plugin-delay-compensation pass is O(V+E), the DCT reuses its cached matrix, and `spectrum` `to_db` uses the single-allocation overload.

### Bindings & API

- Added imperative `Mixer` strip setters and planar-stereo voice processing, hand-written offline effects/dynamics bindings for Node and Python, offline dynamics TypeScript typings for WASM, and backfilled Python `.pyi` stubs for runtime-exposed analyzer functions.
- Added `fill_na` / `fillNa` to YIN and pYIN pitch APIs across the C ABI, Python, Node, and WASM. The default keeps unvoiced frames as `NaN`; enabling the option returns `0` for unvoiced `f0` frames.
- Added time-varying timbre output to `analyze_timbre` / `analyzeTimbre`. Results now include per-window brightness, warmth, density, roughness, and complexity entries via `timbre_over_time` / `timbreOverTime`.
- Exposed additional librosa-compatible feature, decomposition, effect, and loudness APIs across the C ABI, Python, Node, and WASM: spectral contrast, polynomial spectral features, zero-crossing indices, pitch tuning, tuning estimation, NMF decomposition, nearest-neighbour filtering, interval remix, phase-vocoder time scaling, HPSS with residual, multichannel LUFS, and EBU R128 loudness range.
- Surfaced voice-character preset accessors (`voice_character_preset_id`, `realtime_voice_changer_preset_config`) across Python, Node, and WASM, with a consistent `preset` parameter name.
- Wired previously ignored mastering chain parameters through the named-processor and JSON paths (`repair.declip` `lpcBlend`, `multiband.*` per-band params, compressor detector/sidechain-HPF/PDR), and round-tripped the realtime voice-changer ISP limiter enable flag and dBTP ceiling through JSON presets.
- Hardened binding inputs: WASM `remix` reads interval boundaries as exact integer sample indices (no float truncation of large indices), Node `scaleQuantizeMidi`/`scaleCorrectionSemitones` reject a `modeMask` outside `[0, 4095]`, and Node time-stretch requires an explicit numeric `sampleRate`.
- Preserved mixer pan mode when serialising scenes after `sonare_strip_set_pan` and removed a per-call allocation from latest goniometer reads.

## v1.2.1 (2026-05-27)

### Bindings & API

- Added a `StreamingRetune` WASM binding (prepare/reset/setConfig/config/grainSize/processMono) backed by `editing/voice_changer/streaming_retune.h`, with TypeScript types and Vitest coverage.

### CLI

- Added VQT, mel-to-audio/MFCC-to-audio (Griffin-Lim) reconstruction, meter, clipping, dynamic-range, stereo, and phase analysis commands.
- Added normalize, gain, fade, biquad filter, and resample processing commands.
- Added tone, chirp, and clicks synthesis generators.

### Platform

- Dropped `windows-latest` from the native build matrix; MSVC source-portability fixes are retained so building from source on Windows still works.

## v1.2.0 (2026-05-26)

### Mixing engine

- Added the mixing engine surface: channel strips, pan modes, width controls, sends, FX buses, goniometer/true-peak metering, JSON scene presets, and offline stereo rendering.
- Added channel-strip input trim, insert gain scale/output gain/pan controls, external sidechain parameters, bus insert hosting, graph PDC, and scene-loaded persistent mixer APIs.
- Added hold and s-curve automation shapes plus per-target insert/send lanes.
- Added automation lanes, scene/preset API, and an AudioWorklet bridge.
- Added a native mixing benchmark target and expanded CI coverage for macOS and Windows native builds.
- Added mixing QA coverage for golden hashes, no-allocation process checks, graph routing/PDC integration, meter/goniometer snapshots, and CLI/binding smoke tests.

### Mastering engine

- Added a monitor bus output with automation telemetry diagnostics and sample-accurate, bind-feedback automation routing.
- Made the dynamics processors real-time-safe via channel pre-allocation, with a centralised channel preallocation limit.
- Resolved loudness targets per streaming platform and honoured platform normalisation.
- Registered ducking and loudnessOptimize processors and added a de-esser bandpass Q with stereo preservation.
- Added assistant/profile/streaming-preview JSON output and a configurable speech mono-maker amount.

### Analysis & features

- Added a cosine-similarity mode to the tempogram.
- Derived streaming-retune grain size from the sample rate.
- Improved DSP correctness for iSTFT windowing, chroma folding, K-weighting, spectral/VQT/iirt/melody/CQT features, and percentile interpolation (now matching NumPy's linear interpolation).

### Bindings & API

- Exposed mixing presets and rendering through C, Python, Node, WASM, and CLI APIs.
- Exposed mastering assistant/profile/preview, ducking, streaming chord/pattern progression, stream window/output-format config, and inverse Mel/MFCC reconstruction across the C, Node, and WASM bindings.

### Fixes

- Preserved per-channel mastering state on channel-count change and tightened config validation.
- Made engine counters and smoothing atomic and excluded shared strips.
- Fixed exact cumulative sample counting and bounded chroma history in the streaming analyzer.
- Dropped the spurious sidechain reset in the Node streaming equalizer.

### Internal

- Centralised numeric constants in `util/constants.h` and routed IIR, crossover, and mastering filters through shared biquad/loudness helpers.
- Fixed the stale `SONARE_VERSION_*` macros in `sonare.h` so the runtime `version()` reports the correct value.
