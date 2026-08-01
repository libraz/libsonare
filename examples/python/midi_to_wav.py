#!/usr/bin/env python3
"""Render a short MIDI phrase with a NativeSynth preset."""

from __future__ import annotations

import struct
import sys
import wave

import libsonare


def write_wav(path: str, samples: object, sample_rate: int) -> None:
    interleaved = [float(value) for value in samples.reshape(-1)]
    frames = struct.pack(f"<{len(interleaved)}h", *(round(max(-1.0, min(1.0, sample)) * 32767) for sample in interleaved))
    with wave.open(path, "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(frames)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT_WAV", file=sys.stderr)
        return 2

    with libsonare.Project() as project:
        project.set_sample_rate(48000)
        _, clip_id = project.add_midi_clip(0.0, 4.0)
        project.set_midi_events(
            clip_id,
            [
                libsonare.Project.midi_note_on(0.0, 0, 0, 60, 100),
                libsonare.Project.midi_note_off(1.0, 0, 0, 60),
                libsonare.Project.midi_note_on(1.0, 0, 0, 64, 100),
                libsonare.Project.midi_note_off(2.0, 0, 0, 64),
            ],
        )
        audio = project.bounce_with_synth_instrument("saw-lead", num_channels=2)
    write_wav(sys.argv[1], audio, 48000)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
