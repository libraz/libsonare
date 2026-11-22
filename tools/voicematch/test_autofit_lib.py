"""Fitting against a prebuilt library: nothing is built, a library that ignores
overrides is refused, and a stale library is reported rather than trusted.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_autofit_lib.py -q
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit
import catalogue as catalogue_module
from _repo import shared_library_name
from catalogue import Catalogue


def _boom(*a, **k):
    raise AssertionError("a prebuilt library must not be configured or built")


def _lib(tmp_path: Path) -> Path:
    lib = tmp_path / shared_library_name()
    lib.write_bytes(b"not a real library")
    return lib


def _parse(*argv: str):
    return autofit.build_parser().parse_args(list(argv))


def _no_build(monkeypatch):
    monkeypatch.setattr(autofit, "configure_build", _boom)
    monkeypatch.setattr(autofit, "build_shared", _boom)
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", [" M src/x.cpp"]))
    monkeypatch.setattr(autofit, "src_files", list)


def test_dump_knobs_with_lib_never_builds_and_ignores_a_dirty_src(tmp_path, monkeypatch, capsys):
    _no_build(monkeypatch)
    lib = _lib(tmp_path)
    seen = {}

    def fake_dump(program, pattern, lib_path, **kw):
        seen["lib"] = lib_path
        return Catalogue(
            defaults={"p0.gain": 1.0},
            programs={(0, 0): "p0"},
            bounds={},
            modes={},
            rigs={},
        )

    monkeypatch.setattr(autofit, "dump_catalogue", fake_dump)
    args = _parse("--spec", "auto", "--program", "0", "--dump-knobs", "--lib", str(lib))
    assert autofit.run(args, []) == 0
    assert seen["lib"] == str(lib.resolve())
    assert "p0.gain" in capsys.readouterr().out


def test_a_library_that_writes_no_dump_is_refused(tmp_path, monkeypatch):
    _no_build(monkeypatch)
    lib = _lib(tmp_path)

    class _Done:
        returncode = 0
        stderr = ""

    # The render succeeds and leaves SONARE_TUNING_DUMP untouched, as a library
    # built without BUILD_TUNING does.
    monkeypatch.setattr(catalogue_module.subprocess, "run", lambda *a, **k: _Done())
    args = _parse("--spec", "auto", "--program", "0", "--dump-knobs", "--lib", str(lib))
    with pytest.raises(RuntimeError, match="no knob dump"):
        autofit.run(args, [])


def test_a_library_whose_dump_has_no_keys_is_refused(tmp_path, monkeypatch):
    _no_build(monkeypatch)
    lib = _lib(tmp_path)
    monkeypatch.setattr(
        autofit,
        "dump_catalogue",
        lambda *a, **k: Catalogue(defaults={}, programs={}, bounds={}, modes={}, rigs={}),
    )
    args = _parse("--spec", "auto", "--program", "0", "--dump-knobs", "--lib", str(lib))
    with pytest.raises(RuntimeError, match="ignores SONARE_TUNING_OVERRIDES"):
        autofit.run(args, [])


def test_source_knobs_are_refused_under_lib_naming_them(tmp_path, monkeypatch):
    _no_build(monkeypatch)
    lib = _lib(tmp_path)
    spec = tmp_path / "spec.json"
    spec.write_text(
        json.dumps(
            [
                {"tunable": "a.b", "min": 0, "max": 1},
                {"file": "src/midi/synth/foo.cpp", "pattern": "x = (1.0)", "min": 0, "max": 2},
            ]
        )
    )
    args = _parse("--spec", str(spec), "--program", "0", "--lib", str(lib), "--dry-run")
    with pytest.raises(ValueError, match=r"src/midi/synth/foo\.cpp"):
        autofit.run(args, [])


def test_a_fit_against_a_prebuilt_library_never_writes_back(tmp_path, monkeypatch):
    _no_build(monkeypatch)
    lib = _lib(tmp_path)
    args = _parse("--spec", "auto", "--program", "0", "--lib", str(lib))
    with pytest.raises(ValueError, match="read-only"):
        autofit.run(args, [])


def test_the_report_names_a_source_newer_than_the_library(tmp_path):
    lib = _lib(tmp_path)
    old, new = tmp_path / "old.cpp", tmp_path / "new.cpp"
    old.write_text("a")
    new.write_text("b")
    base = lib.stat().st_mtime
    os.utime(old, (base - 100, base - 100))
    os.utime(new, (base + 100, base + 100))
    record = autofit.library_record(lib, [old, new, tmp_path / "gone.cpp"])
    assert record["newer_sources"] == [str(new)]
    assert record["path"] == str(lib)
    assert len(record["sha256"]) == 64
    assert record["mtime"].endswith("+00:00")


def test_the_library_record_is_folded_into_out(tmp_path):
    out = tmp_path / "out.json"
    out.write_text("{}")
    lib = {"path": "p", "mtime": "t", "sha256": "h", "newer_sources": ["src/a.cpp"]}
    autofit._fold_tree_provenance(str(out), "abc", [], lib)
    assert json.loads(out.read_text())["library"] == lib


def test_the_dylib_of_a_prebuilt_path_is_the_path(tmp_path):
    lib = _lib(tmp_path)
    assert autofit.library_path(lib) == lib
