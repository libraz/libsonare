"""The spec guard: that it reads both spec shapes and fails on a name nothing has."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_specs  # noqa: E402


def _spec(tmp_path: Path, payload, name: str = "spec.json") -> Path:
    path = tmp_path / name
    path.write_text(json.dumps(payload))
    return path


def _catalogue(*keys: str):
    """Enough of a Catalogue for the resolution: the keys the library reported."""
    return SimpleNamespace(defaults={k: 0.0 for k in keys})


def test_both_spec_shapes_are_read(tmp_path):
    """A bare list and a `knobs` object are both shipped."""
    as_list = _spec(tmp_path, [{"tunable": "a.b"}], "list.json")
    as_object = _spec(tmp_path, {"_": "note", "knobs": [{"tunable": "a.b"}]}, "object.json")
    assert check_specs.spec_knobs(as_list) == ["a.b"]
    assert check_specs.spec_knobs(as_object) == ["a.b"]


def test_a_source_edited_knob_names_no_tunable(tmp_path):
    """`file`/`pattern` knobs rewrite source and have nothing to resolve."""
    path = _spec(tmp_path, {"knobs": [{"file": "x.cpp", "pattern": "kY = ([0-9.]+)f"}]})
    assert check_specs.spec_knobs(path) == []


def test_a_knob_the_library_reports_passes(tmp_path):
    path = _spec(tmp_path, {"knobs": [{"tunable": "harpsichord.harpsichord.pluck_8a"}]})
    assert check_specs.missing([path], _catalogue("harpsichord.harpsichord.pluck_8a")) == {}


def test_a_knob_nothing_has_is_named(tmp_path):
    """The failure this exists for: a spec outliving the mechanism it swept."""
    path = _spec(tmp_path, {"knobs": [
        {"tunable": "harpsichord_voice.kContactWide"},
        {"tunable": "harpsichord.harpsichord.pluck_8a"},
    ]})
    dead = check_specs.missing([path], _catalogue("harpsichord.harpsichord.pluck_8a"))
    assert dead == {"spec.json": ["harpsichord_voice.kContactWide"]}
