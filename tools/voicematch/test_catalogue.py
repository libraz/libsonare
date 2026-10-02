from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import catalogue


def _source_root(tmp_path: Path, text: str, name: str = "voice.cpp") -> Path:
    source_root = tmp_path / "src"
    source_root.mkdir()
    (source_root / name).write_text(text)
    return tmp_path


def test_scan_tunables_keeps_explicit_scope_and_literal_spans(tmp_path, monkeypatch):
    root = _source_root(
        tmp_path,
        'SONARE_TUNABLE_SCOPED("piano_voice", kGain, 1.25f)\nSONARE_TUNABLE(kLocal, -2.5e-1f)\n',
        "piano_voice_calibration.cpp",
    )
    monkeypatch.setattr(catalogue, "REPO_ROOT", root)

    found = catalogue.scan_tunables()

    scoped = found["piano_voice.kGain"]
    assert scoped.value == 1.25
    source = scoped.file.read_text()
    assert source[scoped.span_start : scoped.span_end] == "1.25"
    rewritten = source[: scoped.span_start] + "2.0" + source[scoped.span_end :]
    assert "kGain, 2.0f" in rewritten

    ordinary = found["piano_voice_calibration.kLocal"]
    assert ordinary.value == -0.25
    assert ordinary.file == scoped.file


def test_scan_tunables_rejects_duplicate_explicit_keys(tmp_path, monkeypatch):
    root = _source_root(
        tmp_path,
        'SONARE_TUNABLE_SCOPED("piano_voice", kGain, 1.25f)\n'
        'SONARE_TUNABLE_SCOPED("piano_voice", kGain, 2.0f)\n',
        "piano_voice_calibration.cpp",
    )
    monkeypatch.setattr(catalogue, "REPO_ROOT", root)

    with pytest.raises(ValueError, match=r"piano_voice\.kGain is declared twice"):
        catalogue.scan_tunables()


def test_scan_tunables_unifies_ordinary_and_explicit_scopes(tmp_path, monkeypatch):
    root = _source_root(
        tmp_path,
        'SONARE_TUNABLE(kGain, 1.25f)\nSONARE_TUNABLE_SCOPED("voice", kGain, 2.0f)\n',
        "voice.cpp",
    )
    monkeypatch.setattr(catalogue, "REPO_ROOT", root)

    with pytest.raises(ValueError, match=r"voice\.kGain is declared twice"):
        catalogue.scan_tunables()
