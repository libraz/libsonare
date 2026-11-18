"""CLI tests for ``declip``: how the clip threshold reaches the library."""

from __future__ import annotations

import json
import subprocess
import sys

import pytest

import libsonare
from libsonare import _cli_repair, cli


def _declip(monkeypatch: pytest.MonkeyPatch, *extra: str) -> dict[str, object]:
    calls: dict[str, object] = {}
    monkeypatch.setattr(
        _cli_repair, "_load_channels_or_downmix", lambda _path: ([[0.0] * 64], 22050)
    )

    def fake_declip(samples: object, *, sample_rate: int, **knobs: object) -> object:
        calls.update(knobs)
        return type("Result", (), {"samples": [0.0] * 64})()

    monkeypatch.setattr(libsonare, "mastering_repair_declip", fake_declip)
    args = cli._build_parser().parse_args(["declip", "take.wav", "--json", *extra])
    assert _cli_repair.cmd_declip(args) == 0
    return calls


def test_declip_threshold_is_given_in_dbfs(monkeypatch, capsys) -> None:
    calls = _declip(monkeypatch, "--clip-threshold-db", "-1.5")
    assert calls["clip_threshold_db"] == -1.5
    assert json.loads(capsys.readouterr().out)["clip_threshold_db"] == -1.5


def test_declip_threshold_left_out_is_the_librarys_default(monkeypatch, capsys) -> None:
    calls = _declip(monkeypatch)
    assert "clip_threshold_db" not in calls
    assert "clip_threshold_db" not in json.loads(capsys.readouterr().out)


def test_declip_linear_threshold_flag_is_refused() -> None:
    result = subprocess.run(
        [sys.executable, "-m", "libsonare.cli", "declip", "take.wav", "--clip-threshold", "0.98"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 2
    assert "--clip-threshold" in result.stderr
