"""Fixture materialization and argv placeholder resolution for a contract run."""

from __future__ import annotations

import copy
import json
import math
import re
import struct
import wave
from pathlib import Path
from typing import Any


def _write_wav(
    path: Path,
    fixture: dict[str, Any],
    *,
    channels: int = 1,
    tags: dict[str, str] | None = None,
) -> None:
    sample_rate = int(fixture["sample_rate"])
    frames = int(fixture["frames"])
    frequencies = [
        float(value) for value in fixture.get("frequencies_hz", [fixture["frequency_hz"]])
    ]
    # Each partial takes an equal share of the amplitude, so a chord peaks where the tone does.
    amplitude = float(fixture["amplitude"]) / len(frequencies)
    samples = bytearray()
    for index in range(frames):
        value = sum(
            amplitude * math.sin(2.0 * math.pi * frequency * index / sample_rate)
            for frequency in frequencies
        )
        sample = round(max(-1.0, min(1.0, value)) * 32767.0)
        if channels == 2:
            samples.extend(struct.pack("<hh", sample, -sample))
        else:
            samples.extend(struct.pack("<" + "h" * channels, *([sample] * channels)))
    with wave.open(str(path), "wb") as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(sample_rate)
        output.writeframes(bytes(samples))
    if tags is not None:
        _append_riff_info_tags(path, tags)


def _append_riff_info_tags(path: Path, tags: dict[str, str]) -> None:
    """Append the fixture's artist and title as padded RIFF LIST/INFO values."""
    info = bytearray(b"INFO")
    for key, tag_id in (("artist", b"IART"), ("title", b"INAM")):
        value = tags[key].encode("utf-8") + b"\0"
        info.extend(tag_id + struct.pack("<I", len(value)) + value)
        if len(value) % 2:
            info.append(0)
    list_chunk = b"LIST" + struct.pack("<I", len(info)) + bytes(info)
    if len(info) % 2:
        list_chunk += b"\0"
    wav = bytearray(path.read_bytes())
    riff_size = struct.unpack_from("<I", wav, 4)[0]
    wav[4:8] = struct.pack("<I", riff_size + len(list_chunk))
    path.write_bytes(wav + list_chunk)


def _write_impulse_wav(path: Path, fixture: dict[str, Any]) -> None:
    """Write one full-scale sample followed by silence, at the tone's rate and length."""
    frames = int(fixture["frames"])
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(int(fixture["sample_rate"]))
        output.writeframes(struct.pack("<h", 32767) + bytes(2 * (frames - 1)))


def _write_float_wav(path: Path, fixture: dict[str, Any]) -> None:
    """Write the tone as 32-bit IEEE float, the one PCM image that holds a sample past full scale."""
    sample_rate = int(fixture["sample_rate"])
    frames = int(fixture["frames"])
    frequency = float(fixture["frequency_hz"])
    amplitude = float(fixture["amplitude"])
    data = b"".join(
        struct.pack("<f", amplitude * math.sin(2.0 * math.pi * frequency * index / sample_rate))
        for index in range(frames)
    )
    fmt = struct.pack("<HHIIHH", 3, 1, sample_rate, sample_rate * 4, 4, 32)
    body = (
        b"WAVE"
        + b"fmt "
        + struct.pack("<I", len(fmt))
        + fmt
        + b"fact"
        + struct.pack("<II", 4, frames)
        + b"data"
        + struct.pack("<I", len(data))
        + data
    )
    path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)


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
    stereo_path = directory / "contract-stereo.wav"
    _write_wav(stereo_path, fixtures["audio"], channels=2)
    paths["audio_stereo"] = str(stereo_path)
    # The same tone at a second length, for the commands that take two recordings
    # of one part. Written from the audio fixture's own descriptor so the pair
    # cannot drift apart in rate or frequency, which is what an alignment needs.
    take_path = directory / "contract-take.wav"
    _write_wav(take_path, {**fixtures["audio"], "frames": fixtures["audio"]["take_frames"]})
    paths["audio_take"] = str(take_path)
    # The same tone at another rate, for the alignment whose take and reference
    # were recorded at different rates.
    other_rate = fixtures["audio"]["other_rate_take"]
    other_rate_path = directory / "contract-take-other-rate.wav"
    _write_wav(
        other_rate_path,
        {
            **fixtures["audio"],
            "sample_rate": other_rate["sample_rate"],
            "frames": other_rate["frames"],
        },
    )
    paths["audio_take_other_rate"] = str(other_rate_path)
    stereo_other_rate_path = directory / "contract-stereo-take-other-rate.wav"
    _write_wav(
        stereo_other_rate_path,
        {
            **fixtures["audio"],
            "sample_rate": other_rate["sample_rate"],
            "frames": other_rate["frames"],
        },
        channels=2,
    )
    paths["audio_stereo_take_other_rate"] = str(stereo_other_rate_path)
    overwide_path = directory / "contract-overwide.wav"
    _write_wav(overwide_path, fixtures["audio"], channels=6)
    paths["audio_overwide"] = str(overwide_path)
    tagged_path = directory / "contract-tagged.wav"
    _write_wav(tagged_path, fixtures["audio"], tags=fixtures["audio"]["tags"])
    paths["audio_tagged"] = str(tagged_path)
    # A dominant seventh at the tone's rate and length, for the cases whose flag
    # only shows on a chord with more than three pitch classes.
    chord_path = directory / "contract-chord.wav"
    _write_wav(
        chord_path,
        {**fixtures["audio"], "frequencies_hz": fixtures["audio"]["chord_frequencies_hz"]},
    )
    paths["audio_chord"] = str(chord_path)
    # The tone past full scale, for the writers that lower a whole output set by one
    # gain instead of clipping it; a 16-bit fixture cannot carry such a sample.
    hot_path = directory / "contract-hot.wav"
    _write_float_wav(
        hot_path, {**fixtures["audio"], "amplitude": fixtures["audio"]["hot_amplitude"]}
    )
    paths["audio_hot"] = str(hot_path)
    # Silence and a lone impulse at the tone's rate and length: the inputs on which
    # a measurement is not taken (absent) or reaches its infinite end (null), so the
    # two front-ends are compared on exactly the values a text printer can get wrong.
    silent_path = directory / "contract-silent.wav"
    _write_wav(silent_path, {**fixtures["audio"], "amplitude": 0.0})
    paths["audio_silent"] = str(silent_path)
    impulse_path = directory / "contract-impulse.wav"
    _write_impulse_wav(impulse_path, fixtures["audio"])
    paths["audio_impulse"] = str(impulse_path)
    for name, text in fixtures["projects"].items():
        project_path = directory / f"project_{name}.json"
        project_path.write_text(text, encoding="utf-8")
        paths[f"project_{name}"] = str(project_path)
    for name, value in fixtures["presets"].items():
        preset_path = directory / f"preset_{name}.json"
        preset_path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")
        paths[f"preset_{name}"] = str(preset_path)
    # The custom preset with only its retune grain changed, one document per size.
    for size in fixtures["preset_grain_sizes"]:
        preset = copy.deepcopy(fixtures["presets"]["custom"])
        preset["id"] = f"contract-grain-{size}"
        preset["dsp"]["retune"]["grainSizeSamples"] = size
        preset_path = directory / f"preset_grain_{size}.json"
        preset_path.write_text(json.dumps(preset, separators=(",", ":")), encoding="utf-8")
        paths[f"preset_grain_{size}"] = str(preset_path)
    for name, value in fixtures["chain_configs"].items():
        chain_config_path = directory / f"chain_config_{name}.json"
        chain_config_path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")
        paths[f"chain_config_{name}"] = str(chain_config_path)
    for name, value in fixtures["playback_configs"].items():
        playback_config_path = directory / f"playback_config_{name}.json"
        playback_config_path.write_text(json.dumps(value, separators=(",", ":")), encoding="utf-8")
        paths[f"playback_config_{name}"] = str(playback_config_path)
    for name, value in fixtures["melodies"].items():
        melody_path = directory / f"melody_{name}.mid"
        _write_smf(melody_path, value)
        paths[f"melody_{name}"] = str(melody_path)
    paths["project_warning_output"] = str(directory / "canonical_project.json")
    paths["mastering_report"] = str(directory / "mastering-report.json")
    paths["chain_config_module_off_output"] = str(directory / "chain-config-module-off.wav")
    paths["preset_missing"] = str(directory / "preset-does-not-exist.json")
    paths["melody_missing"] = str(directory / "melody-does-not-exist.mid")
    paths["output"] = str(directory / "rejected-output.wav")
    paths["split_takes_prefix"] = str(directory / "split-take-")
    # The WAV writers are the only place the two surfaces hold separate code for
    # the same bytes, and a sample with no PCM image is the one input where they
    # can disagree. The writers' guard is not against a non-finite the caller
    # supplied -- the decoder refuses that before any writer runs, so no fixture
    # can carry one -- but against a non-finite the library computed, which is
    # why these cases reach it through a band boost on the fixture's tone plus the
    # largest output gain a 32-bit float accepts. The sibling gain that still fits
    # is the control: without it a writer emitting silence for everything would
    # satisfy the other two.
    paths["eq_non_finite_output"] = str(directory / "eq-non-finite-output.wav")
    paths["eq_non_finite_output_24bit"] = str(directory / "eq-non-finite-output-24bit.wav")
    paths["eq_saturating_output"] = str(directory / "eq-saturating-output.wav")
    paths["rir_output"] = str(directory / "rir-output.wav")
    paths["voice_change_preset_output"] = str(directory / "voice-change-preset-output.wav")
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
    paths["align_takes_other_rate_output"] = str(directory / "aligned-takes-other-rate.json")
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
