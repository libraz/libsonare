"""End-to-end CLI coverage for ``gain``, ``fade``, ``filter`` and ``system-info``.

The payload key sets are the cross-front-end contract, so each is asserted
exactly; the validators are asserted by their native wording and the usage exit.
"""

from __future__ import annotations

import json
import math
import os
import struct
import subprocess
import sys
import wave
from pathlib import Path
from typing import Any

import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

SR = 22050
FRAMES = SR // 2
EXIT_USAGE = 2
EXIT_INVALID_PARAMETER = 3


def _run_cli(args: list[str]) -> subprocess.CompletedProcess[str]:
    src_dir = str(Path(__file__).parent.parent / "src")
    env = dict(os.environ)
    env["PYTHONPATH"] = src_dir + os.pathsep + env.get("PYTHONPATH", "")
    return subprocess.run(
        [sys.executable, "-m", "libsonare.cli", *args],
        capture_output=True,
        text=True,
        env=env,
    )


def _read_wav(path: Path) -> tuple[list[float], int]:
    with wave.open(str(path), "rb") as handle:
        rate = handle.getframerate()
        raw = handle.readframes(handle.getnframes())
    return [code / 32768.0 for code in struct.unpack(f"<{len(raw) // 2}h", raw)], rate


@pytest.fixture(scope="module")
def source(tmp_path_factory: pytest.TempPathFactory) -> Path:
    path = tmp_path_factory.mktemp("dsp") / "source.wav"
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(SR)
        samples = [0.4 * math.sin(2.0 * math.pi * 440.0 * n / SR) for n in range(FRAMES)]
        handle.writeframes(b"".join(struct.pack("<h", int(round(s * 32767.0))) for s in samples))
    return path


def _json(result: subprocess.CompletedProcess[str]) -> dict[str, Any]:
    assert result.returncode == 0, result.stderr
    payload: dict[str, Any] = json.loads(result.stdout)
    return payload


def test_gain_payload_and_samples(source: Path, tmp_path: Path) -> None:
    from libsonare import mastering_apply_processor

    out = tmp_path / "gain.wav"
    payload = _json(_run_cli(["gain", str(source), "-o", str(out), "--gain-db", "-3", "--json"]))
    assert set(payload) == {"output", "output_gain_db", "gain_db", "duration"}
    assert payload["gain_db"] == -3.0
    assert payload["duration"] == pytest.approx(FRAMES / SR)

    input_samples, rate = _read_wav(source)
    expected = mastering_apply_processor(
        "utility.gain", input_samples, sample_rate=rate, params={"levelDb": -3.0}
    ).samples
    written, _ = _read_wav(out)
    assert len(written) == len(expected)
    assert max(abs(a - float(b)) for a, b in zip(written, expected, strict=True)) < 2e-4


def test_fade_payload_reports_omitted_side_as_zero(source: Path, tmp_path: Path) -> None:
    out = tmp_path / "fade.wav"
    payload = _json(
        _run_cli(["fade", str(source), "-o", str(out), "--fade-in-sec", "0.1", "--json"])
    )
    assert set(payload) == {"output", "output_gain_db", "fade_in_sec", "fade_out_sec", "duration"}
    assert payload["fade_in_sec"] == pytest.approx(0.1)
    assert payload["fade_out_sec"] == 0.0
    written, _ = _read_wav(out)
    assert abs(written[0]) < 1e-3


@pytest.mark.parametrize(
    ("argv", "order", "cutoff", "center", "bandwidth"),
    [
        (["--type", "low-pass", "--cutoff-hz", "2000"], 2, 2000.0, None, None),
        (["--type", "high-pass", "--cutoff-hz", "500", "--order", "4"], 4, 500.0, None, None),
        (
            ["--type", "notch", "--center-hz", "1000", "--bandwidth-hz", "200"],
            2,
            None,
            1000.0,
            200.0,
        ),
    ],
)
def test_filter_payload(
    source: Path,
    tmp_path: Path,
    argv: list[str],
    order: int,
    cutoff: float | None,
    center: float | None,
    bandwidth: float | None,
) -> None:
    out = tmp_path / "filter.wav"
    payload = _json(_run_cli(["filter", str(source), "-o", str(out), *argv, "--json"]))
    # Only the frequencies the filter type reads are reported.
    frequencies = {
        key: value
        for key, value in (
            ("cutoff_hz", cutoff),
            ("center_hz", center),
            ("bandwidth_hz", bandwidth),
        )
        if value is not None
    }
    assert set(payload) == {
        "output",
        "output_gain_db",
        "type",
        "order",
        "zero_phase",
        "duration",
        *frequencies,
    }
    assert payload["type"] == argv[1]
    assert payload["order"] == order
    for key, value in frequencies.items():
        assert payload[key] == value
    assert payload["zero_phase"] is False


def test_filter_zero_phase_reported(source: Path, tmp_path: Path) -> None:
    out = tmp_path / "zp.wav"
    argv = ["filter", str(source), "-o", str(out), "--type", "low-pass", "--cutoff-hz", "2000"]
    assert _json(_run_cli([*argv, "--zero-phase", "--json"]))["zero_phase"] is True


def test_filter_cutoff_above_nyquist_is_a_library_refusal(source: Path, tmp_path: Path) -> None:
    result = _run_cli(
        [
            "filter",
            str(source),
            "-o",
            str(tmp_path / "x.wav"),
            "--type",
            "low-pass",
            "--cutoff-hz",
            "30000",
        ]
    )
    assert result.returncode == EXIT_INVALID_PARAMETER
    assert "cutoff_hz must be finite and in (0, 11025)" in result.stderr


@pytest.mark.parametrize(
    ("argv", "message"),
    [
        (["fade"], "give --fade-in-sec or --fade-out-sec"),
        (
            ["filter", "--type", "low-pass"],
            "--cutoff-hz is required when --type is high-pass or low-pass",
        ),
        (
            ["filter", "--type", "notch", "--bandwidth-hz", "100"],
            "--center-hz is required when --type is band-pass or notch",
        ),
        (
            ["filter", "--type", "notch", "--center-hz", "1000"],
            "--bandwidth-hz is required when --type is band-pass or notch",
        ),
        (
            ["filter", "--type", "high-pass", "--cutoff-hz", "100", "--center-hz", "100"],
            "--center-hz does not apply when --type is high-pass or low-pass",
        ),
        (
            [
                "filter",
                "--type",
                "band-pass",
                "--center-hz",
                "1000",
                "--bandwidth-hz",
                "100",
                "--cutoff-hz",
                "100",
            ],
            "--cutoff-hz does not apply when --type is band-pass or notch",
        ),
        (
            [
                "filter",
                "--type",
                "notch",
                "--center-hz",
                "1000",
                "--bandwidth-hz",
                "100",
                "--order",
                "4",
            ],
            "--order must be 2 when --type is band-pass or notch",
        ),
    ],
)
def test_validators_refuse_before_the_input_is_read(argv: list[str], message: str) -> None:
    unreadable = "/nonexistent/sonare/unreadable.wav"
    result = _run_cli([argv[0], unreadable, "-o", "/nonexistent/out.wav", *argv[1:]])
    assert result.returncode == EXIT_USAGE
    assert message in result.stderr


def test_system_info_json_has_the_three_sections() -> None:
    payload = _json(_run_cli(["system-info", "--json"]))
    assert {"cpu", "memory", "parallel"} <= set(payload)
    assert isinstance(payload["cpu"], dict)


def test_system_info_text_form() -> None:
    result = _run_cli(["system-info"])
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    assert lines[0] == "System Information"
    assert lines[1].startswith("  CPU Cores: ")
    assert lines[2].startswith("  Memory: ")
    assert "Parallel Configuration" in lines
