"""Render a short MIDI phrase with a NativeSynth preset."""

from __future__ import annotations

import sys

import libsonare
from pcm16_wav import write_pcm16_wav


def write_wav(path: str, samples: object, sample_rate: int) -> None:
    interleaved = samples.reshape(-1)
    write_pcm16_wav(path, interleaved, sample_rate, 2)


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
