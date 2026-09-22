"""Fixture materialization and argv placeholder resolution for a contract run."""

from __future__ import annotations

import json
import math
import re
import struct
import wave
from pathlib import Path
from typing import Any


def _write_wav(path: Path, fixture: dict[str, Any]) -> None:
    sample_rate = int(fixture["sample_rate"])
    frames = int(fixture["frames"])
    frequency = float(fixture["frequency_hz"])
    amplitude = float(fixture["amplitude"])
    samples = bytearray()
    for index in range(frames):
        sample = round(
            max(
                -1.0,
                min(
                    1.0,
                    amplitude * math.sin(2.0 * math.pi * frequency * index / sample_rate),
                ),
            )
            * 32767.0
        )
        samples.extend(struct.pack("<h", sample))
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(bytes(samples))


def _variable_length_quantity(value: int) -> bytes:
    """Encode a delta time the way a MIDI track spells it, high group first."""
    encoded = bytearray([value & 0x7F])
    value >>= 7
    while value:
        encoded.insert(0, 0x80 | (value & 0x7F))
        value >>= 7
    return bytes(encoded)


def _write_smf(path: Path, fixture: dict[str, Any]) -> None:
    """Write a melody as a single-track SMF from the manifest's note list.

    Built here rather than exported by the library, because this module is stdlib
    only and runs before either front-end does: producing the reference through
    one CLI would make the other's fixture depend on a command's own output. The
    note list also stays readable in the manifest, which a byte blob would not.
    """
    tempo_microseconds = round(60_000_000 / float(fixture["tempo_bpm"]))
    events: list[tuple[int, int, bytes]] = []
    for note in fixture["notes"]:
        pitch = int(note["midi"])
        start = int(note["start_ticks"])
        # A note-off is ordered ahead of a note-on at the same tick, so a
        # repeated pitch does not end the note that follows it.
        events.append((start, 1, bytes((0x90, pitch, int(note["velocity"])))))
        events.append((start + int(note["length_ticks"]), 0, bytes((0x80, pitch, 0))))
    events.sort(key=lambda event: (event[0], event[1]))
    track = bytearray(
        _variable_length_quantity(0) + b"\xff\x51\x03" + tempo_microseconds.to_bytes(3, "big")
    )
    previous_tick = 0
    for tick, _order, message in events:
        track.extend(_variable_length_quantity(tick - previous_tick) + message)
        previous_tick = tick
    track.extend(_variable_length_quantity(0) + b"\xff\x2f\x00")
    header = (
        b"MThd"
        + (6).to_bytes(4, "big")
        + (0).to_bytes(2, "big")
        + (1).to_bytes(2, "big")
        + int(fixture["ppq"]).to_bytes(2, "big")
    )
    path.write_bytes(header + b"MTrk" + len(track).to_bytes(4, "big") + bytes(track))


def _write_fixtures(directory: Path, manifest: dict[str, Any]) -> dict[str, str]:
    fixtures = manifest["fixtures"]
    paths: dict[str, str] = {}
    audio_path = directory / "contract.wav"
    _write_wav(audio_path, fixtures["audio"])
    paths["audio"] = str(audio_path)
    # The same tone at a second length, for the commands that take two recordings
    # of one part. Written from the audio fixture's own descriptor so the pair
    # cannot drift apart in rate or frequency, which is what an alignment needs.
    take_path = directory / "contract-take.wav"
    _write_wav(take_path, {**fixtures["audio"], "frames": fixtures["audio"]["take_frames"]})
    paths["audio_take"] = str(take_path)
    for name, text in fixtures["projects"].items():
        project_path = directory / f"project_{name}.json"
        project_path.write_text(text, encoding="utf-8")
        paths[f"project_{name}"] = str(project_path)
    for name, value in fixtures["presets"].items():
        preset_path = directory / f"preset_{name}.json"
        preset_path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")
        paths[f"preset_{name}"] = str(preset_path)
    for name, value in fixtures["melodies"].items():
        melody_path = directory / f"melody_{name}.mid"
        _write_smf(melody_path, value)
        paths[f"melody_{name}"] = str(melody_path)
    paths["project_warning_output"] = str(directory / "canonical_project.json")
    paths["mastering_report"] = str(directory / "mastering-report.json")
    paths["preset_missing"] = str(directory / "preset-does-not-exist.json")
    paths["melody_missing"] = str(directory / "melody-does-not-exist.mid")
    paths["output"] = str(directory / "rejected-output.wav")
    # The WAV writers are the only place the two surfaces hold separate code for
    # the same bytes, and a sample with no PCM image is the one input where they
    # can disagree. The writers' guard is not against a non-finite the caller
    # supplied -- the decoder refuses that before any writer runs, so no fixture
    # can carry one -- but against a non-finite the library computed, which is
    # why these cases reach it through an output gain past what a 32-bit float
    # holds. The sibling gain that still fits is the control: without it a
    # writer emitting silence for everything would satisfy the other two.
    paths["eq_non_finite_output"] = str(directory / "eq-non-finite-output.wav")
    paths["eq_non_finite_output_24bit"] = str(directory / "eq-non-finite-output-24bit.wav")
    paths["eq_saturating_output"] = str(directory / "eq-saturating-output.wav")
    paths["rir_output"] = str(directory / "rir-output.wav")
    paths["render_output"] = str(directory / "render-output.wav")
    paths["resample_output"] = str(directory / "resample-output.wav")
    paths["morph_output"] = str(directory / "morph-output.wav")
    paths["bounce_output"] = str(directory / "bounce-output.wav")
    paths["new_project_output"] = str(directory / "new-project.json")
    paths["align_takes_output"] = str(directory / "aligned-takes.json")
    # A second destination, so the case that varies the chroma resolution does not
    # overwrite the document whose bytes the case above pins.
    paths["align_takes_finer_output"] = str(directory / "aligned-takes-finer.json")
    paths["align_takes_many_output"] = str(directory / "aligned-takes-many.json")
    # The import paths read what the export paths write, so the export contracts
    # are ordered ahead of them in the manifest.
    paths["smf_output"] = str(directory / "export.mid")
    paths["midi2_output"] = str(directory / "export.midi2")
    paths["smf_import_output"] = str(directory / "import-smf.json")
    paths["midi2_import_output"] = str(directory / "import-midi2.json")
    return paths


_PLACEHOLDER = re.compile(r"\{([a-z0-9_]+)\}")


def _resolve_argv(argv: list[str], paths: dict[str, str]) -> list[str]:
    """Substitute every ``{name}`` a token contains, not only a whole token.

    Substituting whole tokens only made the contract unable to express any
    spelling that pairs a placeholder with something else in one argument --
    ``--input ID=path`` is the CLI's own documented form for a multi-track
    command, and it was passed through literally, so the case read as covered
    while the file was never found. An unknown name is left as written, so a
    literal brace in an argument still reaches the CLI unchanged.
    """
    return [_PLACEHOLDER.sub(lambda m: paths.get(m.group(1), m.group(0)), token) for token in argv]
