"""Tests for the model renderer's library-staleness warning.

Every number the harness reports comes out of one dylib, the render carries no
mark of which, and the default is a build directory nothing keeps current — so
a whole bank-wide sweep can measure the previous generation of a voice with
each reading looking entirely ordinary. These pin the one line that says so.

    rye run --pyproject bindings/python/pyproject.toml \
        python -m pytest tools/voicematch/test_render_model.py
"""

from __future__ import annotations

import os
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import render_model  # noqa: E402

HOUR = 3600.0


def _stamp(when: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(when))


@pytest.fixture(autouse=True)
def _fresh_process(monkeypatch):
    """Undo the once-per-process latch so each case starts unwarned."""
    monkeypatch.setattr(render_model, "_staleness_checked", False)


def _library(tmp_path: Path, *, built_at: float) -> str:
    lib = tmp_path / "libsonare.dylib"
    lib.write_bytes(b"")
    os.utime(lib, (built_at, built_at))
    return str(lib)


def test_a_library_newer_than_the_sources_says_nothing(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(render_model, "_newest_source_mtime", lambda: 1000.0)
    render_model.warn_if_stale(_library(tmp_path, built_at=1000.0 + HOUR))
    assert capsys.readouterr().err == ""


def test_a_library_older_than_a_source_names_both_times_and_the_refresh(
    tmp_path, monkeypatch, capsys
):
    monkeypatch.setattr(render_model, "_newest_source_mtime", lambda: 1000.0 + HOUR)
    render_model.warn_if_stale(_library(tmp_path, built_at=1000.0))
    err = capsys.readouterr().err
    assert "measures the older library" in err
    assert render_model.REFRESH_HINT in err
    # Both stamps, so the reader sees how far apart they are rather than only
    # that they differ.
    assert _stamp(1000.0) in err
    assert _stamp(1000.0 + HOUR) in err


def test_the_warning_is_said_once_per_process(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(render_model, "_newest_source_mtime", lambda: 1000.0 + HOUR)
    lib = _library(tmp_path, built_at=1000.0)
    render_model.warn_if_stale(lib)
    capsys.readouterr()
    render_model.warn_if_stale(lib)
    assert capsys.readouterr().err == ""


def test_no_resolved_library_is_not_a_staleness_finding(monkeypatch, capsys):
    def _unreachable() -> float:  # pragma: no cover - asserts it is never called
        raise AssertionError("the source tree must not be walked for an empty path")

    monkeypatch.setattr(render_model, "_newest_source_mtime", _unreachable)
    render_model.warn_if_stale("")
    assert capsys.readouterr().err == ""


def test_a_library_that_is_not_there_is_not_a_staleness_finding(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(render_model, "_newest_source_mtime", lambda: 1000.0 + HOUR)
    render_model.warn_if_stale(str(tmp_path / "absent.dylib"))
    assert capsys.readouterr().err == ""
