"""CLI tests for ``repair --stages``: exactly the named stages, at library defaults."""

from __future__ import annotations

import json
import math
from pathlib import Path

import pytest

from ._analyzer_helpers import _run_cli, _write_test_wav


def _identity_fit():
    """A fit that leaves the samples alone: the writer under test is the plain quantizer."""
    from libsonare._cli_output import _OutputFit

    return _OutputFit()


_SAMPLE_RATE = 22050
# The plateau sits below full scale and the rebuilt peak with it, so the
# full-scale fit never acts and the comparison with declip is exact.
_PLATEAU = 0.5
_DETECT_AT_DB = 20.0 * math.log10(0.47)


def _clipped_source(tmp_path: Path) -> Path:
    source = tmp_path / "clipped.wav"
    samples = [
        max(-_PLATEAU, min(_PLATEAU, 0.6 * math.sin(2.0 * math.pi * 220.0 * i / _SAMPLE_RATE)))
        for i in range(4410)
    ]
    _write_test_wav(str(source), samples, _SAMPLE_RATE)
    return source


def test_repair_stages_declip_writes_what_the_declip_entry_writes(tmp_path) -> None:
    from libsonare import mastering_repair_declip
    from libsonare._cli_output import _write_channel_output
    from libsonare.audio import Audio

    source = _clipped_source(tmp_path)
    via_declip = tmp_path / "declip.wav"
    via_stages = tmp_path / "stages.wav"

    with Audio.from_file(str(source)) as audio:
        mono = mastering_repair_declip(
            audio.data, sample_rate=audio.sample_rate, clip_threshold_db=_DETECT_AT_DB
        )
        _write_channel_output(
            str(via_declip), [mono.samples], audio.sample_rate, fit=_identity_fit()
        )
    repaired = _run_cli(
        [
            "repair",
            str(source),
            "--stages",
            "declip",
            "--params",
            f"repair.declip.clipThresholdDb={_DETECT_AT_DB}",
            "-o",
            str(via_stages),
            "--json",
        ],
    )
    assert repaired.returncode == 0, repaired.stderr

    payload = json.loads(repaired.stdout)
    assert payload["mode"] == "stages"
    assert payload["stages"] == ["repair.declip"]
    assert payload["output_gain_db"] == 0.0
    # The control: the repair changed the file, so equality is not two copies.
    assert via_stages.read_bytes() != source.read_bytes()
    assert via_stages.read_bytes() == via_declip.read_bytes()


def test_repair_stages_takes_a_stage_name_in_any_letter_case(tmp_path) -> None:
    source = _clipped_source(tmp_path)
    result = _run_cli(
        [
            "repair",
            str(source),
            "--stages",
            "DeHum, Declip",
            "-o",
            str(tmp_path / "o.wav"),
            "--json",
        ],
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["stages"] == ["repair.declip", "repair.dehum"]


@pytest.mark.parametrize(
    ("extra", "message"),
    [
        (["--stages", "declip,reverb"], "--stages: unknown stage 'reverb' (expected declip, "),
        (["--stages", "declip,Declip"], "--stages names declip twice"),
        (["--stages", "declip,,dehum"], "--stages has an empty entry"),
        (["--stages", ""], "--stages has an empty entry"),
        (["--stages", "declip", "--preset", "speech"], "--stages cannot be combined with --preset"),
        (["--stages", "declip", "--explain"], "--explain cannot be combined with --stages"),
        (["--params", "eq.enabled=1"], "--params eq.enabled must be a repair.* field"),
        (
            ["--stages", "dehum", "--params", "repair.declip.lpcOrder=24"],
            "--params repair.declip.lpcOrder names a stage --stages does not run",
        ),
    ],
)
def test_repair_stages_refusals(tmp_path, extra: list[str], message: str) -> None:
    source = _clipped_source(tmp_path)
    result = _run_cli(["repair", str(source), "-o", str(tmp_path / "o.wav"), *extra])
    # Decided from the command line, so a usage error before the input is read.
    assert result.returncode == 2, result.stderr
    assert message in result.stderr


def test_repair_stages_is_refused_beside_detect(tmp_path) -> None:
    source = _clipped_source(tmp_path)
    result = _run_cli(["repair", str(source), "--detect", "--stages", "declip"])
    assert result.returncode == 2, result.stderr
    assert "--stages cannot be combined with --detect" in result.stderr
