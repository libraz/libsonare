#!/usr/bin/env python3
"""Isolated go/no-go probe for the DawDreamer + The Grand 3 reference-oracle path.

Headlessly instantiates the Steinberg "The Grand 3" VST3, renders a single
middle-C note, and reports whether we got non-silent audio. This is the make-or-
break check for the plugin-hosted (route B) oracle: if it renders sound here,
the whole voice-match harness can target real high-quality plugins offline.
"""

from __future__ import annotations

import sys
import time
import wave
from pathlib import Path

import numpy as np

import dawdreamer as dd

PLUGIN = "/Library/Audio/Plug-Ins/VST3/Steinberg/The Grand 3.vst3"
PRESET = (
    "/Library/Audio/Presets/Steinberg Media Technologies/"
    "The Grand 3/01 Yamaha C7/Close/Natural Ambience.vstpreset"
)
SR = 48000
BUF = 512
NOTE = 60          # middle C
VEL = 100
NOTE_DUR = 2.0     # seconds the key is held
RENDER = 4.0       # seconds rendered (lets the tail ring out)
OUT = Path("/tmp/voicematch_grand_probe.wav")


def _write_wav(path: Path, audio: np.ndarray, sr: int) -> None:
    """Write a (channels, samples) float array to a 16-bit PCM WAV."""
    if audio.ndim == 1:
        audio = audio[None, :]
    interleaved = audio.T  # -> (samples, channels)
    clipped = np.clip(interleaved, -1.0, 1.0)
    pcm = (clipped * 32767.0).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(pcm.shape[1])
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


def main() -> int:
    if not Path(PLUGIN).exists():
        print(f"FAIL: plugin not found: {PLUGIN}")
        return 2

    eng = dd.RenderEngine(SR, BUF)
    plugin = eng.make_plugin_processor("grand", PLUGIN)
    print(f"loaded plugin: {PLUGIN}")
    print(f"  is instrument       : {getattr(plugin, 'get_num_input_channels', lambda: '?')()} in-ch")
    print(f"  reported parameters : {plugin.get_plugin_parameter_size()}")

    if Path(PRESET).exists():
        ok = plugin.load_vst3_preset(PRESET)
        print(f"loaded preset       : {Path(PRESET).name} -> {ok}")
    else:
        print(f"WARN: preset missing: {PRESET}")

    eng.load_graph([(plugin, [])])

    # The Grand 3 streams samples from disk on a background thread. An offline
    # render never advances wall-clock, so a cold instance renders silence.
    # Trigger the note once to enqueue the streamer's load requests, sleep in
    # real time so the background thread can fault the samples in, then render
    # for real.
    plugin.add_midi_note(NOTE, VEL, 0.0, NOTE_DUR)
    eng.render(RENDER)  # warm-up pass: requests samples, likely silent
    time.sleep(20.0)
    eng.render(RENDER)  # real pass: samples should now be resident

    audio = eng.get_audio()  # (channels, samples)
    audio = np.asarray(audio, dtype=np.float32)
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    rms = float(np.sqrt(np.mean(audio**2))) if audio.size else 0.0

    print(f"rendered shape : {audio.shape}")
    print(f"peak           : {peak:.6f}")
    print(f"rms            : {rms:.6f}")

    if peak < 1e-4:
        print("FAIL: output is silent (activation / async sample load / no default program?)")
        return 1

    _write_wav(OUT, audio, SR)
    print(f"OK: non-silent audio written to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
