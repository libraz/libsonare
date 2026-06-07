# libsonare

[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)

A C++-core audio DSP toolkit for Python — librosa-compatible analysis plus
broadcast-grade mastering, mixing, editing, synthesis, and a real-time engine.

Built on a C++ core with NumPy as the only Python dependency. Analysis defaults
match librosa (validated against generated librosa reference values in CI), and
mastering ships 66 named DSP processors implemented against published
references (ITU-R BS.1770-4 true-peak limiting, Linkwitz-Riley crossovers,
Vicanek matched-Z biquads, ADAA-antialiased saturation) — Apache-2.0, no model
weights.

## Installation

```bash
pip install libsonare
```

Supported platforms: Linux (x86_64, aarch64), macOS (Apple Silicon).

## Quick Start

`Audio` is the recommended entry point. The top-level `libsonare.detect_*` /
`libsonare.analyze` functions are thin wrappers around `Audio` for
one-shot calls and are kept for convenience.

### Load from a file

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")  # or "song.wav"
print(f"BPM: {audio.detect_bpm():.1f}")
print(f"Key: {audio.detect_key().root.name} {audio.detect_key().mode.name}")

# Advanced key options are opt-in; defaults preserve existing behavior.
key_with_options = audio.detect_key(
    use_hpss=True,
    loudness_weighted=True,
    high_pass_hz=80.0,
)

result = audio.analyze()  # BPM + key + time signature + beats
print(f"BPM: {result.bpm:.1f}  Key: {result.key.root.name} {result.key.mode.name}")
```

### Room acoustics

Use `detect_acoustic` for blind RT60/EDT estimation from ordinary audio.
Use `analyze_impulse_response` when you have a measured impulse response and
need clarity metrics (`c50`, `c80`, `d50`). Blind mode returns `nan` for
clarity metrics because they are not reliable without an impulse response.

```python
import libsonare

audio = libsonare.Audio.from_file("recording.wav")
blind = audio.detect_acoustic(
    n_octave_bands=6,
    n_third_octave_subbands=24,
    min_decay_db=30.0,
    noise_floor_margin_db=10.0,
)
print(blind.rt60, blind.edt, blind.is_blind)

ir = libsonare.Audio.from_file("room_ir.wav")
params = ir.analyze_impulse_response()
print(params.rt60, params.c50, params.c80, params.d50)
```

`estimate_room` infers room geometry (`volume`, `length`/`width`/`height`,
`drr_db`, per-band `absorption_bands` / `rt60_bands`) from a recording.
`synthesize_rir` renders an image-source-model RIR for a shoebox room, and
`room_morph` convolves audio with such an RIR to relocate it into that room.

```python
import numpy as np
import libsonare

room = libsonare.estimate_room(samples, sample_rate=48000)
print(room.volume, room.length, room.width, room.height, room.rt60_bands)

rir = libsonare.synthesize_rir(
    length_m=6.0, width_m=4.0, height_m=2.8,
    absorption=0.2, sample_rate=48000, ism_order=3,
)
print(len(rir.rir), rir.sample_rate)

wet = libsonare.room_morph(
    samples, 48000, length_m=6.0, width_m=4.0, height_m=2.8, wet=0.5,
)
```

### Load from a numpy array / in-memory samples

`from_buffer` accepts any 1D float sequence. With numpy, use a **mono float32**
array (stereo must be downmixed first, e.g. `samples.mean(axis=1)`).

```python
import numpy as np
import libsonare

sr = 22050
samples = np.asarray(my_mono_float32_signal, dtype=np.float32)

audio = libsonare.Audio.from_buffer(samples, sample_rate=sr)
bpm = audio.detect_bpm()

# Or call the function directly (equivalent shortcut)
bpm = libsonare.detect_bpm(samples, sample_rate=sr)
```

### Pitch, timbre, and spectral APIs

Pitch tracking keeps unvoiced `f0` frames as `nan` by default. Pass
`fill_na=True` when downstream code needs finite values and should treat
unvoiced frames as `0`. Timbre analysis returns aggregate metrics plus
`timbre_over_time`; `timbreOverTime` is also available as an alias.

```python
yin = libsonare.pitch_yin(samples, sample_rate=sr, fill_na=True)
pyin = audio.pitch_pyin(fill_na=True)

timbre = libsonare.analyze_timbre(samples, sample_rate=sr)
print(timbre.brightness, timbre.timbre_over_time[0].brightness)

contrast = libsonare.spectral_contrast(samples, sample_rate=sr)
poly = libsonare.poly_features(samples, sample_rate=sr)
crossings = libsonare.zero_crossings(samples)
tuning = libsonare.estimate_tuning(samples, sample_rate=sr)
offset = libsonare.pitch_tuning(yin.f0)

w, h = libsonare.decompose(spectrogram, n_features, n_frames, 8)
filtered = libsonare.nn_filter(spectrogram, n_features, n_frames)
remixed = libsonare.remix(samples, [0, sr, 2 * sr, 3 * sr], sample_rate=sr)
stretched = libsonare.phase_vocoder(samples, sample_rate=sr, rate=1.5)
hpss = libsonare.hpss_with_residual(samples, sample_rate=sr)

multi = libsonare.lufs_interleaved(interleaved, channels=2, sample_rate=sr)
lra = libsonare.ebur128_loudness_range(samples, sample_rate=sr)
```

### Mastering

The Python binding exposes the same mastering processors as the C, CLI, Node,
and WASM APIs.

```python
import libsonare

names = libsonare.mastering_processor_names()  # e.g. "dynamics.compressor"

compressed = libsonare.mastering_process(
    "dynamics.compressor",
    samples,
    sample_rate=sr,
    params={"thresholdDb": -24.0, "ratio": 1.5},
)

widened = libsonare.mastering_process_stereo(
    "stereo.imager",
    left,
    right,
    sample_rate=sr,
    params={"width": 1.1},
)

pair_names = libsonare.mastering_pair_processor_names()  # e.g. "match.abCrossfade"
crossfaded = libsonare.mastering_pair_process(
    "match.abCrossfade",
    source,
    reference,
    sample_rate=sr,
    params={"mix": 0.25},
)

loudness_json = libsonare.mastering_pair_analyze(
    "match.referenceLoudness",
    source,
    reference,
    sample_rate=sr,
)
mono_compat_json = libsonare.mastering_stereo_analyze(
    "stereo.monoCompatCheck",
    left,
    right,
    sample_rate=sr,
)
```

### Mastering specialist processors

For direct access to individual dynamics and restoration processors (each
returns `(processed_ndarray, latency_samples)` for the dynamics family, and a
processed `ndarray` for the repair family):

```python
import libsonare

comp, latency = libsonare.mastering_dynamics_compressor(
    samples, sample_rate=sr, threshold_db=-18.0, ratio=2.0, attack_ms=10.0,
)
gated, _ = libsonare.mastering_dynamics_gate(samples, sample_rate=sr, threshold_db=-50.0)
shaped, _ = libsonare.mastering_dynamics_transient_shaper(
    samples, sample_rate=sr, attack_gain_db=3.0,
)

declicked = libsonare.mastering_repair_declick(samples, sample_rate=sr)
declipped = libsonare.mastering_repair_declip(samples, sample_rate=sr)
decrackled = libsonare.mastering_repair_decrackle(samples, sample_rate=sr)
dehummed = libsonare.mastering_repair_dehum(samples, sample_rate=sr, fundamental_hz=50.0)
denoised = libsonare.mastering_repair_denoise_classical(samples, sample_rate=sr)
dereverbed = libsonare.mastering_repair_dereverb_classical(samples, sample_rate=sr)
trimmed = libsonare.mastering_repair_trim_silence(samples, sample_rate=sr)
```

Insert (FX) introspection for scene/UI builders:

```python
inserts = libsonare.mastering_insert_names()              # e.g. "dynamics.compressor"
params = libsonare.mastering_insert_param_names("dynamics.compressor")  # camelCase keys
```

### Mastering chain

`mastering_chain` runs the full configurable mastering pipeline (EQ, dynamics,
saturation, repair, stereo, loudness, ...). The Python binding accepts **flat
dot-notation keys** for the config dict — addressing any module parameter
directly — and an optional `on_progress(progress, stage)` callback that is
invoked after each stage (progress in ``[0, 1]``).

```python
import libsonare

result = libsonare.mastering_chain(
    samples,
    sample_rate=sr,
    config={
        "eq.tilt.tiltDb": 0.5,
        "dynamics.compressor.thresholdDb": -24.0,
        "dynamics.compressor.ratio": 1.5,
        "dynamics.transientShaper.attackGainDb": 2.0,
        "repair.declick.enabled": True,
        "loudness.targetLufs": -14.0,
        "loudness.ceilingDb": -1.0,
    },
    on_progress=lambda p, stage: print(f"[{p * 100:5.1f}%] {stage}"),
)

stereo_result = libsonare.mastering_chain_stereo(
    left, right, sample_rate=sr,
    config={"stereo.imager.width": 1.1, "loudness.targetLufs": -14.0},
)
```

`MasteringChainResult` exposes the rendered samples plus loudness telemetry
(`input_lufs`, `output_lufs`, `applied_gain_db`, `latency_samples`).

### Mastering presets

Named presets ship sensible defaults for common targets. `master_audio`
applies a preset and lets you override any individual parameter using the
same flat dot-notation keys as `mastering_chain`.

```python
import libsonare

libsonare.mastering_preset_names()
# -> ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech', 'streaming', 'youtube', 'broadcast', 'podcast', 'audiobook', 'cinema', 'jpop', 'ambient', 'lofi', 'classical', 'drumAndBass', 'techno', 'metal', 'trap', 'rnb', 'jazz', 'kpop', 'trance', 'gameOst']

result = libsonare.master_audio(
    samples,
    sample_rate=sr,
    preset_name="aiMusic",
    overrides={
        "loudness.targetLufs": -13.0,
        "dynamics.multibandComp.enabled": True,
    },
)

# Audio shortcut
audio = libsonare.Audio.from_file("song.wav")
pop_mastered = audio.master_audio("pop")
```

### Mixing

```python
import libsonare

libsonare.mixing_scene_preset_names()
# -> ['vocalReverbSend', ...]
scene_json = libsonare.mixing_scene_preset_json("vocalReverbSend")

offline = libsonare.mix_stereo(
    [(vocal_l, vocal_r), (music_l, music_r)],
    sample_rate=sr,
    input_trim_db=[3.0, 0.0],
    fader_db=[-3.0, -12.0],
    pan=[0.0, -0.2],
    width=[1.0, 0.9],
)

mixer = libsonare.Mixer.from_scene_json(scene_json, sample_rate=sr, block_size=512)
try:
    block_l, block_r = mixer.process_stereo(
        [vocal_block_l, return_block_l],
        [vocal_block_r, return_block_r],
    )
finally:
    mixer.close()
```

### Headless DAW project

`Project` is a headless arrangement model: audio & MIDI tracks and clips, MIDI
sequencing, SMF / MIDI 2.0 Clip File I/O, deterministic JSON save/load, and an
offline `bounce`. Every mutation routes through an undoable history, musical
positions are PPQ (quarter notes), and the object is a context manager.

```python
import libsonare

with libsonare.Project() as project:
    project.set_sample_rate(48000)
    track_id, clip_id = project.add_midi_clip(0.0, 4.0)
    project.set_midi_events(clip_id, [
        libsonare.Project.midi_note_on(0.0, 0, 0, 60, 100),  # ppq, group, channel, note, velocity
        libsonare.Project.midi_note_off(1.0, 0, 0, 60),
    ])

    json_str = project.to_json()            # deterministic, byte-stable within a build
    smf = project.export_smf()              # bytes — Standard MIDI File
    midi2 = project.export_clip_file()      # bytes — MIDI 2.0 Clip File (lossless)

    result = project.compile()              # has_timeline / messages / diagnostics
    audio = project.bounce(num_channels=2)  # (frames, channels) float32 ndarray
```

### Instruments and synthesis

A plain `Project.bounce()` renders MIDI tracks to silence (no instrument is
bound). The `bounce_with_*_instrument` methods route bound MIDI destinations
through a sound source: the simple built-in oscillator (`BuiltinSynthConfig`),
the full patch-driven NativeSynth (`SynthPatch` / preset name), a SoundFont
player (`Sf2InstrumentConfig`, after `load_soundfont`), or your own host
instrument (`bounce_with_instruments`). `synth_preset_names()` lists the
NativeSynth catalog, and `synth_preset_patch(name)` returns a tweakable
`SynthPatch`.

```python
import libsonare

print(libsonare.synth_preset_names())   # ['sine', 'saw-lead', 'e-piano', ...]

with libsonare.Project() as project:
    project.set_sample_rate(48000)
    _, clip_id = project.add_midi_clip(0.0, 4.0)
    project.set_midi_events(clip_id, [
        libsonare.Project.midi_note_on(0.0, 0, 0, 60, 100),
        libsonare.Project.midi_note_off(2.0, 0, 0, 60),
    ])

    # NativeSynth preset (or a SynthPatch, or None for the default patch).
    audio = project.bounce_with_synth_instrument("saw-lead", num_channels=2)

    # Build a patch from a preset base, overriding only what you need.
    patch = libsonare.SynthPatch(preset="e-piano", cutoff_hz=4000.0, gain=0.8)
    audio2 = project.bounce_with_synth_instrument(patch, num_channels=2)

    # Simple built-in oscillator.
    audio3 = project.bounce_with_builtin_instrument(
        libsonare.BuiltinSynthConfig(waveform="saw", gain=0.5),
    )
```

### Real-time engine

`RealtimeEngine` is a block-based transport + clip/MIDI playback engine with
instrument binding, MIDI input/output, parameter automation, capture, an
offline bounce, and a telemetry ring. It is a context manager.

```python
import libsonare
from libsonare import EngineBounceOptions

with libsonare.RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
    engine.set_tempo(120.0)
    engine.set_synth_instrument("saw-lead", destination_id=0)
    engine.push_midi_note_on(0, 0, 0, 60, 100)   # destination, group, channel, note, velocity
    engine.play()

    out = engine.process([[0.0] * 128, [0.0] * 128])  # -> planar [[...left], [...right]]
    telemetry = engine.drain_telemetry()

    bounced = engine.bounce_offline(EngineBounceOptions(total_frames=48000, num_channels=2))
    print(bounced.frames, bounced.num_channels, bounced.integrated_lufs)
```

Capabilities:

- Transport: `play` / `stop` / `seek_sample` / `seek_ppq` / `set_tempo` / `set_time_signature` / `set_loop`.
- Clips: `set_clips([EngineClip(...)])`, paged streaming via `ClipPageProvider` / `FileClipPageProvider`.
- Instruments: `set_synth_instrument` / `set_builtin_instrument` / `set_sf2_instrument` (after `load_soundfont`).
- MIDI: `push_midi_note_on` / `push_midi_note_off` / `push_midi_cc` / `push_midi_panic`, live input via `push_midi_input_*`, and `bind_midi_cc` to parameters.
- Rendering: `process` / `process_with_monitor` / `render_offline` / `bounce_offline` / `freeze_offline`.
- Automation, markers, metronome, capture (`arm_capture` / `captured_audio`), and `drain_telemetry` / `drain_meter_telemetry`.

### Real-time voice changer

`RealtimeVoiceChanger` runs a block-based vocal chain (retune, formant shift,
EQ, gate, compressor, de-esser, reverb, limiter) configured from a named preset
or a config mapping. `voice_change_realtime` is a one-shot convenience for an
entire buffer. Preset helpers list, fetch, and validate configs.

```python
import libsonare

print(libsonare.realtime_voice_changer_preset_names())  # ['neutral-monitor', 'bright-idol', ...]

with libsonare.RealtimeVoiceChanger(48000, "bright-idol", max_block_size=128) as vc:
    out_block = vc.process_mono([0.0] * 128)      # -> np.ndarray
    vc.set_config("deep-narrator")                # swap preset mid-stream
    cfg = vc.config_pod()                         # RealtimeVoiceChangerConfig (POD)

# One-shot over a whole buffer.
processed = libsonare.voice_change_realtime(samples, sample_rate=48000, preset="bright-idol")

cfg = libsonare.realtime_voice_changer_preset_config("neutral-monitor")
check = libsonare.validate_realtime_voice_changer_preset_json(
    libsonare.realtime_voice_changer_preset_json("bright-idol")
)
print(check["ok"])  # True
```

### Streaming frame analyzer

`StreamAnalyzer` analyzes audio incrementally: feed chunks with `process`, then
drain analyzed frames (mel / chroma / onset / RMS / spectral / chord) with
`read_frames` (or the quantized `read_frames_u8` / `read_frames_i16`). Query the
running BPM/key/chord estimate via `stats`. It is a context manager.

```python
import libsonare

with libsonare.StreamAnalyzer(libsonare.StreamConfig(sample_rate=22050)) as sa:
    for chunk in audio_chunks:
        sa.process(chunk)
    sa.finalize()

    frames = sa.read_frames(sa.available_frames())
    print(frames.n_frames, frames.n_mels)

    stats = sa.stats()
    print(stats.bpm, stats.key, stats.chord_root)
```

### Metering

A compact metering family operates on plain sample buffers. Names (all
`libsonare.<name>`):

| Function | Returns |
|----------|---------|
| `metering_peak_db` / `metering_rms_db` / `metering_true_peak_db` | `float` |
| `metering_crest_factor_db` / `metering_dc_offset` | `float` |
| `metering_stereo_correlation` / `metering_stereo_width` | `float` (L/R) |
| `metering_detect_clipping` | `ClippingReport` |
| `metering_dynamic_range` | `DynamicRangeReport` |
| `metering_spectrum` / `metering_spectrum_frame` | `SpectrumReport` |
| `metering_vectorscope` / `metering_vectorscope_decimated` | `VectorscopeReport` |
| `metering_phase_scope` / `metering_phase_scope_decimated` | `PhaseScopeReport` |
| `waveform_peaks` / `waveform_peak_pyramid` | `WaveformPeaksReport` (min/max buckets) |

```python
import libsonare

print(libsonare.metering_true_peak_db(samples, sample_rate=sr))
report = libsonare.metering_detect_clipping(samples, sample_rate=sr)
peaks = libsonare.waveform_peaks(samples, channels=1, samples_per_bucket=512)
```

### Error handling

Native return-code failures raise `libsonare.SonareError`, a subclass of
`RuntimeError` carrying a numeric `.code`. Input-validation failures (empty /
NaN / Inf buffers, bad shapes) raise `ValueError`, and missing-feature builds
raise `RuntimeError`.

```python
import libsonare

try:
    libsonare.synth_preset_patch("does-not-exist")
except libsonare.SonareError as exc:
    print(exc.code, str(exc))
```

### Streaming mastering chain

`StreamingMasteringChain` runs the same pipeline block-by-block for real-time
or chunked workflows. The constructor takes the same flat dot-notation config
as `mastering_chain`; non-streamable stages (`repair.denoise`, `loudness`)
cause the constructor to raise, so omit those keys for streaming use. The
object is also a context manager.

```python
import libsonare

with libsonare.StreamingMasteringChain({
    "eq.tilt.tiltDb": 0.5,
    "dynamics.compressor.thresholdDb": -20.0,
    "dynamics.transientShaper.attackGainDb": 1.5,
}) as chain:
    chain.prepare(sample_rate=48000, max_block_size=512, num_channels=1)
    print(chain.stage_names())
    print(f"latency = {chain.latency_samples()} samples")

    out_block = chain.process_mono([0.0] * 512)
    # Stereo:
    # chain.prepare(48000, 512, 2)
    # out_l, out_r = chain.process_stereo(left_block, right_block)
    chain.reset()
```

### Library version

```python
import libsonare
libsonare.__version__   # e.g. "1.3.2"  (preferred — matches importlib.metadata)
libsonare.version()     # native library version string
```

## Supported audio formats

| Format | Default build | With FFmpeg support |
|--------|---------------|---------------------|
| WAV (PCM 16/24/32, float32) | yes | yes |
| MP3 | yes | yes |
| M4A / AAC / FLAC / OGG / Opus / WMA / ... | no | yes |

If `Audio.from_file()` is given an unsupported format you will see a clear
error such as:

```
RuntimeError: Unsupported audio format: '.m4a'. Supported: WAV, MP3.
Rebuild with -DSONARE_WITH_FFMPEG=ON for M4A/AAC/FLAC/OGG,
or convert via: ffmpeg -i "song.m4a" output.wav
```

The PyPI wheels are pinned to **`SONARE_WITH_FFMPEG=OFF`** so the distributed
wheel never silently links against the build host's FFmpeg. From-source builds
default to **AUTO detection** via pkg-config: if FFmpeg dev libraries are
present they are enabled, otherwise WAV/MP3 only.

To enable FFmpeg-backed decoding when building from source:

```bash
git clone https://github.com/libraz/libsonare
cd libsonare
SONARE_FFMPEG=1 bash bindings/python/build_wheel.sh   # require FFmpeg
# or
SONARE_FFMPEG=AUTO bash bindings/python/build_wheel.sh # detect via pkg-config
pip install bindings/python/dist/*.whl
```

This links against the system FFmpeg shared libraries (LGPL by default), so
ensure they are installed (e.g. `brew install ffmpeg`,
`apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev`).

## Function vs `Audio` method

Both APIs return the same results. Use whichever is more convenient:

```python
# Functional (good for ad-hoc numpy work)
bpm = libsonare.detect_bpm(samples, sample_rate=22050)        # -> float
key = libsonare.detect_key(samples, sample_rate=22050)        # -> Key(root, mode, confidence)
key_hpss = libsonare.detect_key(
    samples,
    sample_rate=22050,
    n_fft=4096,
    hop_length=512,
    use_hpss=True,
    loudness_weighted=True,
    high_pass_hz=80.0,
)
result = libsonare.analyze(samples, sample_rate=22050)        # -> AnalysisResult

# Audio class (recommended for files, multiple operations on the same audio)
audio = libsonare.Audio.from_file("song.wav")
bpm = audio.detect_bpm()
key = audio.detect_key()
result = audio.analyze()
```

`Audio` caches the decoded samples and is the only way to load files, so it is
the recommended entry point when you do more than a single computation on the
same signal.

## Input format expectations

| API | dtype | shape | range |
|-----|-------|-------|-------|
| `Audio.from_buffer(samples, sample_rate=...)` | float32 (float64 also accepted) | 1D mono | nominally `[-1.0, 1.0]` |
| `Audio.from_memory(data)` | `bytes` of an encoded WAV / MP3 / (FFmpeg) file | — | — |
| `Audio.from_file(path)` | path to an encoded audio file | — | — |
| `libsonare.detect_bpm(samples, sample_rate=...)` etc. | float32 (float64 also accepted) | 1D mono | nominally `[-1.0, 1.0]` |

Stereo input is **not** downmixed automatically when passed as samples —
downmix yourself (e.g. `samples.mean(axis=1, dtype=np.float32)`). File loaders
downmix to mono internally.

## CLI

```bash
sonare analyze song.mp3
# > Estimated BPM : 161.00 BPM  (conf 75.0%)
# > Estimated Key : C major  (conf 100.0%)

sonare bpm song.mp3 --json       # {"bpm": 161.0}
sonare key song.mp3              # Key: C major (confidence: 100.0%)
sonare spectral song.mp3         # Spectral features table
sonare pitch song.mp3            # Pitch tracking (pYIN)
sonare mel song.mp3              # Mel spectrogram shape
sonare chroma song.mp3           # Chromagram with visualization

sonare rhythm song.mp3           # Rhythm primitives
sonare timbre song.mp3           # Timbre / spectral shape
sonare dynamics song.mp3         # Dynamics / loudness analysis
sonare lufs song.mp3 --series    # Integrated LUFS (+ momentary/short-term)
sonare tempogram song.mp3        # Autocorrelation tempogram
sonare plp song.mp3              # Predominant local pulse
sonare nnls-chroma song.mp3      # NNLS chroma
sonare onset-envelope song.mp3   # Onset strength envelope

sonare acoustic recording.wav                 # Blind RT60 / EDT estimation
sonare estimate-room recording.wav            # Inferred room geometry
sonare synthesize-rir -o rir.wav              # Render a shoebox-room RIR
sonare room-morph song.wav -o wet.wav         # Convolve into a synthesized room

sonare master song.wav -o mastered.wav --preset pop
sonare mastering song.wav -o mastered.wav --target-lufs -14
sonare mastering-processor song.wav --processor dynamics.compressor --params thresholdDb=-24,ratio=1.5
sonare mastering-chain song.wav -o out.wav --params loudness.targetLufs=-14
sonare mastering-processors           # List solo processors
sonare mastering-pair-processors      # List two-input processors
sonare mastering-pair-analyze source.wav --reference reference.wav --analysis match.referenceLoudness
sonare mastering-presets              # List mastering preset names
sonare mastering-suggest song.wav     # Suggested chain as JSON
sonare eq song.wav -o eq.wav --frequency-hz 1000 --gain-db 3 --q 1.0
sonare declip song.wav -o fixed.wav   # Repair clipped audio

sonare pitch-correct vocal.wav -o tuned.wav   # Snap pitch to a scale/MIDI
sonare note-stretch song.wav -o out.wav       # Per-note time stretch
sonare voice-change vocal.wav -o out.wav --preset bright-idol
sonare voice-presets                  # List realtime voice-changer presets
sonare voice-preset --preset bright-idol --json   # Print a voice-changer preset

sonare mixing-presets                 # List mixer scene presets
sonare mixing-preset --preset vocalReverbSend
sonare mix --preset vocalReverbSend
sonare mix --scene scene.json --input a.wav --input b.wav -o mix.wav

sonare synth-presets                  # List NativeSynth presets

# Headless project / MIDI
sonare project new --sample-rate 48000 -o project.json
sonare project bounce --in project.json -o out.wav --synth saw-lead
sonare project export-smf --in project.json -o out.mid
sonare project import-smf --smf in.mid -o project.json
sonare midi-render --in project.json -o out.wav --synth saw-lead
```

## Features

- **Detection**: BPM (`float`), key (`detect_key` / `detect_key_candidates`), beats, downbeats (`detect_downbeats`), onsets, chords (`detect_chords` / `chord_functional_analysis`)
- **Analysis**: Full music analysis (`analyze`), plus `analyze_melody`, `analyze_sections`, `analyze_rhythm`, `analyze_dynamics`, `analyze_timbre`
- **Effects**: HPSS, HPSS with residual, pitch shift, time stretch, phase vocoder, normalize, trim, remix
- **Features**: STFT, mel spectrogram, MFCC, chroma, CQT/VQT, spectral contrast, poly features, zero crossings, pitch tracking (YIN / pYIN with optional `fill_na`)
- **Decomposition & loudness**: NMF decomposition, nearest-neighbour filtering, multichannel LUFS, EBU R128 LRA
- **Mastering & metering**: 66 named processors, preset chains (`master_audio`), dynamics / repair specialist functions, and a `metering_*` / `waveform_peaks` family
- **Mixing**: scene-driven `Mixer`, `mix_stereo`, built-in scene presets
- **Instruments & synthesis**: NativeSynth (`SynthPatch` / presets), built-in oscillator, SoundFont (SF2) playback, host-instrument bounce
- **Real-time**: `RealtimeEngine` (transport / clips / MIDI / capture), `RealtimeVoiceChanger`, `StreamAnalyzer`
- **Room acoustics**: blind RT60 / EDT, `estimate_room`, `synthesize_rir`, `room_morph`
- **Conversions**: Hz / mel / MIDI / note, frames / time
- **Headless DAW**: `Project` arrangement model — audio/MIDI tracks & clips, undo/redo, MIDI sequencing, SMF / MIDI 2.0 Clip File I/O, deterministic JSON, offline `bounce`
- **I/O**: Load WAV / MP3 (and M4A/AAC/FLAC/OGG when built with FFmpeg), resample

## librosa-compatible defaults

| Parameter | Default |
|-----------|---------|
| Sample rate | 22050 Hz |
| `n_fft` | 2048 |
| `hop_length` | 512 |
| `n_mels` | 128 |
| `fmin` / `fmax` | 0.0 / `sr/2` |

## Also available

```bash
npm install @libraz/libsonare  # JavaScript / TypeScript (WASM)
```

## License

Apache-2.0
