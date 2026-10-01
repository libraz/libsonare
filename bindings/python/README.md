# libsonare

[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net)

**Turn audio into data and back, from Python.** Analyze songs (BPM, key, chords, loudness), master and mix to broadcast loudness, and render MIDI through built-in instruments — a fast C++ core whose only Python dependency is NumPy 2.4.6 or later. Mastering ships 92 named DSP processors implemented against published references; analysis defaults match librosa where the two overlap. Apache-2.0, no model weights.

**Guides, API reference and CLI docs: [libsonare.libraz.net](https://libsonare.libraz.net)**

## Installation

```bash
pip install libsonare
```

Supported platforms: Linux (x86_64, aarch64), macOS (Apple Silicon).

## Quick Start

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")  # or "song.wav"
result = audio.analyze()  # BPM + key + time signature + beats
print(f"BPM: {result.bpm:.1f}  Key: {result.key.root.name} {result.key.mode.name}")

# Master toward a target loudness with a named preset
mastered = libsonare.master_audio(
    audio.data, sample_rate=audio.sample_rate, preset_name="streaming",
)
print(mastered.output_lufs, mastered.applied_gain_db)
```

Render a MIDI arrangement through a built-in instrument with the headless `Project` (a context manager):

```python
with libsonare.Project() as project:
    project.set_sample_rate(48000)
    _, clip_id = project.add_midi_clip(0.0, 4.0)
    project.set_midi_events(clip_id, [
        libsonare.Project.midi_note_on(0.0, 0, 0, 60, 100),  # ppq, group, channel, note, velocity
        libsonare.Project.midi_note_off(2.0, 0, 0, 60),
    ])
    audio = project.bounce_with_synth_instrument("saw-lead", num_channels=2)
```

Samples passed directly must be mono float32 (downmix stereo first); the defaults follow librosa (22050 Hz, `n_fft` 2048, `hop_length` 512). See [librosa compatibility](https://libsonare.libraz.net/docs/librosa-compatibility).

## Capabilities

- **Analysis** — BPM, key, chords, downbeats, sections, melody, tuning, pitch, timbre, spectral features and metering. → [Analysis](https://libsonare.libraz.net/docs/python-api-analysis)
- **Stem decomposition and transcription** — `decompose_stems` splits a mix into stems, and `transcribe` turns audio into MIDI events. → [Analysis](https://libsonare.libraz.net/docs/python-api-analysis), [CLI examples](https://libsonare.libraz.net/docs/cli-examples)
- **Mastering** — 92 named DSP processors, the configurable `mastering_chain`, 30 named presets via `master_audio`, and reference-matching. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Restoration** — five repair-only presets, `vinyl`, `tapeHiss`, `fieldRecording`, `voiceMemo` and `shellac78`. → [Mastering processors](https://libsonare.libraz.net/docs/mastering-processors)
- **Mixing** — offline `mix_stereo`, the block-based `Mixer` with scene presets, and `suggest_mix_scene`, which suggests a mixer scene with a written explanation without applying it. → [Mixing](https://libsonare.libraz.net/docs/mixing), [Mixing assistant](https://libsonare.libraz.net/docs/mixing-assistant)
- **Editing DSP** — time-stretch, pitch-shift, HPSS, phase vocoder, normalize, trim, remix. → [Editing DSP](https://libsonare.libraz.net/docs/editing-dsp)
- **Note editing and takes** — edit the notes of monophonic takes (`extract_notes`) and polyphonic material (`PolyphonicAnalysis`), align a take to a reference with `align_take_to_reference`, and build tuning targets from a MIDI melody with `note_targets_from_smf`. → [Project editing](https://libsonare.libraz.net/docs/project-editing-midi), [Recording and takes](https://libsonare.libraz.net/docs/recording-and-takes)
- **Room acoustics** — blind RT60, impulse-response metrics, `estimate_room`, `synthesize_rir`, `room_morph`. → [Room acoustics](https://libsonare.libraz.net/docs/acoustic-analysis)
- **Realtime and streaming** — `RealtimeEngine` with MIDI 1.0 / 2.0 input, bus-to-bus routing with sends, sidechain keys (`set_bus_sidechain`), `StreamAnalyzer`, `StreamingMasteringChain` and `RealtimeVoiceChanger`. → [Realtime engine](https://libsonare.libraz.net/docs/realtime-engine)
- **Instruments and synthesis** — built-in synth, the patch-driven NativeSynth, and a GS-compatible SoundFont (SF2) player that receives GS SysEx and offers two GS insertion-effect realisations (`gs_efx_realization`). The acoustic piano is calibrated; the other physical-model voices are still being tuned and will change in 1.8.x patch releases. → [GM and GS](https://libsonare.libraz.net/docs/gm-gs), [Native synth](https://libsonare.libraz.net/docs/native-synth)
- **Headless DAW** — `Project` with audio and MIDI tracks, undo/redo, SMF and MIDI 2.0 Clip File I/O, and offline `bounce`; `transcribe_to_clip` writes a transcription into a clip. → [Project editing](https://libsonare.libraz.net/docs/project-editing)
- **Playback** — `PlaybackRenderer` renders PCM up to 7.1 to speakers or HRTF binaural headphones. → [Playback](https://libsonare.libraz.net/docs/playback)

The whole surface is listed in the [Python API reference](https://libsonare.libraz.net/docs/python-api).

## CLI

The `sonare` command exposes the analysis, mastering, mixing, effects and project surfaces.

```bash
sonare analyze song.mp3                              # BPM + key summary
sonare master song.wav -o mastered.wav --preset pop  # preset mastering
sonare project bounce --in project.json -o out.wav --synth saw-lead
```

Run `sonare --help`, or see the [CLI reference](https://libsonare.libraz.net/docs/cli).

## Supported audio formats

WAV and MP3 decode out of the box. M4A / AAC / FLAC / OGG / Opus need FFmpeg, which the PyPI wheels are built without; build from source with `SONARE_FFMPEG=1` to enable it (see the [installation guide](https://libsonare.libraz.net/docs/installation)).

## Documentation

[Getting started](https://libsonare.libraz.net/docs/getting-started) · [Python API](https://libsonare.libraz.net/docs/python-api) · [CLI](https://libsonare.libraz.net/docs/cli) · [Examples](https://libsonare.libraz.net/docs/examples) · [Binding parity](https://libsonare.libraz.net/docs/binding-parity). The wheel bundles the JSON Schemas for realtime voice changer presets; see [Realtime voice changer](https://libsonare.libraz.net/docs/realtime-voice-changer).

## Also available

```bash
npm install @libraz/libsonare  # JavaScript / TypeScript (WASM)
```

## License

Apache-2.0
