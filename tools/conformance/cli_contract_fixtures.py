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
                        amplitude
                        * math.sin(2.0 * math.pi * frequency * index / sample_rate),
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


def _write_fixtures(directory: Path, manifest: dict[str, Any]) -> dict[str, str]:
    fixtures = manifest["fixtures"]
    paths: dict[str, str] = {}
    audio_path = directory / "contract.wav"
    _write_wav(audio_path, fixtures["audio"])
    paths["audio"] = str(audio_path)
    for name, text in fixtures["projects"].items():
        project_path = directory / f"project_{name}.json"
        project_path.write_text(text, encoding="utf-8")
        paths[f"project_{name}"] = str(project_path)
    for name, value in fixtures["presets"].items():
        preset_path = directory / f"preset_{name}.json"
        preset_path.write_text(
            json.dumps(value, separators=(",", ":")), encoding="utf-8"
        )
        paths[f"preset_{name}"] = str(preset_path)
    paths["project_warning_output"] = str(directory / "canonical_project.json")
    paths["mastering_report"] = str(directory / "mastering-report.json")
    paths["preset_missing"] = str(directory / "preset-does-not-exist.json")
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
    paths["eq_non_finite_output_24bit"] = str(
        directory / "eq-non-finite-output-24bit.wav"
    )
    paths["eq_saturating_output"] = str(directory / "eq-saturating-output.wav")
    paths["rir_output"] = str(directory / "rir-output.wav")
    paths["render_output"] = str(directory / "render-output.wav")
    paths["resample_output"] = str(directory / "resample-output.wav")
    paths["morph_output"] = str(directory / "morph-output.wav")
    paths["bounce_output"] = str(directory / "bounce-output.wav")
    paths["new_project_output"] = str(directory / "new-project.json")
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
