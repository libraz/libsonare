"""A module kit's model is read on the window its reference rows were read on, or refused.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_kit_window.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module

KIT = {
    "id": "kit_module",
    "source_class": "module",
    "program": 0,
    "gate_ms": 37,
    "preroll_ms": 0,
    "tail": "0s",
    "sample_rate": 44100,
    "timbres": [{"id": "k", "channel": 10}],
}


def _kit_profile() -> dict:
    return {
        "capture": {**KIT, "notes": [28, 29], "velocities": [100]},
        "rows": [{"timbre": "k", "note": n, "velocity": 100} for n in (28, 29)],
    }


def _kit_corpus(tmp_path: Path, seconds: dict[int, float]) -> Path:
    root = tmp_path / "corpus"
    root.mkdir()
    renders = [
        {
            "timbre": "k",
            "note": n,
            "velocity": 100,
            "path": f"{n}.wav",
            **({"seconds": s} if s else {}),
        }
        for n, s in seconds.items()
    ]
    manifest = {
        "gate_ms": 37,
        "preroll_ms": 0,
        "timbres": [{"id": "k"}],
        "renders": renders,
    }
    (root / "manifest.json").write_text(json.dumps(manifest))
    return root


class _Rendered(Exception):
    """Raised by the stubbed render: the comparison got past every refusal."""


def _compare(cfg: dict, corpus: Path | None, monkeypatch) -> int:
    def render(*_a, **_k):
        raise _Rendered

    monkeypatch.setattr(profile_module, "render_model", render)
    try:
        return profile_module.compare_percussion(
            cfg,
            _kit_profile(),
            timbre="k",
            notes_filter=set(),
            gate_path="",
            write_gate="",
            margin=1.25,
            corpus_dir=corpus,
        )
    except _Rendered:
        return -1


def test_a_module_kit_slot_with_no_recorded_length_is_refused_by_name(
    tmp_path, monkeypatch, capsys
):
    corpus = _kit_corpus(tmp_path, {28: 0.124, 29: None})
    assert _compare(KIT, corpus, monkeypatch) == 2
    err = capsys.readouterr().err
    assert "kit_module" in err and "n29/v100" in err and "n28/v100" not in err


def test_a_module_kit_with_no_corpus_is_refused_rather_than_read_on_its_gate(
    tmp_path, monkeypatch, capsys
):
    assert _compare(KIT, None, monkeypatch) == 2
    assert "n28/v100, n29/v100" in capsys.readouterr().err


def test_a_module_kit_whose_slots_are_recorded_reaches_the_render(tmp_path, monkeypatch):
    corpus = _kit_corpus(tmp_path, {28: 0.124, 29: 0.186})
    assert _compare(KIT, corpus, monkeypatch) == -1


def test_a_library_kit_keeps_its_declared_window(tmp_path, monkeypatch):
    assert _compare({**KIT, "source_class": "library"}, None, monkeypatch) == -1
