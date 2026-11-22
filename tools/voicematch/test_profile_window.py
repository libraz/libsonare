"""The per-note window of a module one-shot capture, and what a row's end says it is.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_profile_window.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module

from capture import SOURCE_HOLD_MS, is_module_one_shot
from corpus import corpus_pattern, load_corpus
from metrics import _db, _rms_envelope
from profile_window import (
    MARK_FADE_END,
    MARK_TRUNCATED,
    RULE_RECORDED,
    RULE_SOUND_END,
    WINDOW_MARGIN_MS,
    row_window,
)
from wavio import write_wav

SR = 22050
NOTE = 60
F0 = 261.63


def _held(seconds: float, *, decay: float, floor: float = 0.0, fade_s: float = 0.0) -> np.ndarray:
    """A decaying note over a flat noise bed, optionally faded out over its last `fade_s`."""
    rng = np.random.default_rng(3)
    t = np.arange(int(seconds * SR)) / SR
    y = sum(np.sin(2 * np.pi * F0 * k * t) / k for k in range(1, 6)) * np.exp(-decay * t)
    y = 0.1 * y + floor * rng.standard_normal(t.size)
    if fade_s:
        n = int(fade_s * SR)
        y[-n:] *= np.linspace(1.0, 0.0, n)
    return y


def _window(y: np.ndarray, clear_end_s: float | None = None) -> dict:
    t, env = _rms_envelope(y, SR)
    partials = [F0 * k for k in range(1, 9)]
    return row_window(
        y, SR, int(len(y) / SR * 1000.0), partials, t, np.asarray(_db(env)), clear_end_s
    )


def test_a_module_one_shot_is_decided_from_the_definition():
    base = {"source_class": "module", "gate_ms": 680, "timbres": [{"id": "t0"}]}
    assert is_module_one_shot(base)
    assert not is_module_one_shot({**base, "gate_ms": SOURCE_HOLD_MS})
    assert not is_module_one_shot({**base, "source_class": "library"})
    assert not is_module_one_shot({**base, "timbres": [{"id": "k", "channel": 10}]})


def test_a_row_still_sounding_at_its_end_keeps_its_recorded_length_and_says_truncated():
    window = _window(_held(1.0, decay=1.0, floor=1e-5))
    assert window["rule"] == RULE_RECORDED
    assert window["end_ms"] == window["recorded_ms"]
    assert MARK_TRUNCATED in window["marks"]
    assert "sound_end_ms" not in window


def test_a_row_that_reaches_its_floor_ends_a_margin_after_its_last_clear_frame():
    y = _held(2.0, decay=14.0, floor=1e-3)
    window = _window(y, clear_end_s=0.6)
    assert window["rule"] == RULE_SOUND_END
    assert window["sound_end_ms"] == 600
    assert window["end_ms"] == 600 + WINDOW_MARGIN_MS
    assert MARK_TRUNCATED not in window.get("marks", {})


def test_a_floor_is_never_read_past_the_recorded_length():
    window = _window(_held(0.7, decay=14.0, floor=1e-3), clear_end_s=0.65)
    assert window["rule"] == RULE_RECORDED
    assert window["end_ms"] == window["recorded_ms"]


def test_an_editor_fade_at_the_file_end_is_marked():
    plain = _window(_held(1.0, decay=0.5, floor=1e-5))
    faded = _window(_held(1.0, decay=0.5, floor=1e-5, fade_s=0.03))
    assert MARK_FADE_END not in plain.get("marks", {})
    assert MARK_FADE_END in faded["marks"]
    assert MARK_TRUNCATED in faded["marks"]


def _corpus(tmp_path: Path, lengths_ms: list[float], gate_ms: int, source_class: str = "module"):
    """One timbre, one decaying note per length, extracted-sample style (no preroll)."""
    root = tmp_path / "corpus"
    (root / "t0").mkdir(parents=True)
    renders = []
    for i, ms in enumerate(lengths_ms):
        note = NOTE + 3 * i
        t = np.arange(int(ms / 1000.0 * SR)) / SR
        f0 = 261.63 * 2 ** ((note - 60) / 12)
        y = (0.4 * np.sin(2 * np.pi * f0 * t) * np.exp(-2.0 * t)).astype(np.float32)
        rel = f"t0/n{note:03d}_v100.wav"
        write_wav(root / rel, np.stack([y, y], axis=1), SR)
        renders.append(
            {
                "id": f"t0/n{note:03d}_v100",
                "timbre": "t0",
                "note": note,
                "velocity": 100,
                "path": rel,
                "seconds": len(t) / SR,
                "window_end_ms": int(len(t) / SR * 1000.0),
            }
        )
    manifest = {
        "id": "x",
        "source": {"kind": "sf2", "class": source_class},
        "params": [],
        "sample_rate": SR,
        "gate_ms": gate_ms,
        "tail": "0s",
        "preroll_ms": 0,
        "notes": [r["note"] for r in renders],
        "velocities": [100],
        "timbres": [{"id": "t0"}],
        "renders": renders,
    }
    (root / "manifest.json").write_text(json.dumps(manifest))
    config = tmp_path / "x.json"
    config.write_text(json.dumps({"id": "x", "label": "x", "timbres": [{"id": "t0"}]}))
    cfg = {
        "id": "x",
        "program": 0,
        "source_class": source_class,
        "gate_ms": gate_ms,
        "timbres": [{"id": "t0"}],
        "_path": str(config),
    }
    return root, cfg


def _measured(tmp_path: Path, name: str, root: Path, cfg: dict) -> list[dict]:
    out = tmp_path / f"{name}.json"
    assert profile_module.measure(cfg, root, out) == 0
    return json.loads(out.read_text())["rows"]


def test_each_row_of_a_one_shot_is_read_to_its_own_window_end(tmp_path):
    root, cfg = _corpus(tmp_path, [1500, 1100, 800], gate_ms=800)
    rows = _measured(tmp_path, "oneshot", root, cfg)
    assert [r["window_end_ms"] for r in rows] == [1500, 1100, 800]
    assert all(r["window_rule"] == RULE_RECORDED for r in rows)
    assert [r["window_recorded_ms"] for r in rows] == [1500, 1100, 800]


def test_a_row_ending_at_the_old_gate_measures_bit_identically_to_the_single_gate(tmp_path):
    # 800.4 ms is what a gate cut to the shortest cell leaves over: the gate is the whole-ms floor.
    root, cfg = _corpus(tmp_path, [1500, 800.4], gate_ms=800)
    per_note = _measured(tmp_path, "oneshot", root, cfg)
    single = _measured(tmp_path, "single", root, {**cfg, "source_class": "library"})
    last_new, last_old = per_note[-1], single[-1]
    assert last_new["window_end_ms"] == 800
    for key in sorted(set(last_old) | set(last_new)):
        if key.startswith("window"):
            continue
        assert last_new.get(key) == last_old.get(key), key
    assert "window_end_ms" not in last_old


def test_a_capture_that_is_not_a_one_shot_keeps_its_single_gate(tmp_path):
    root, cfg = _corpus(tmp_path, [1500, 800], gate_ms=800)
    rows = _measured(tmp_path, "single", root, {**cfg, "source_class": "library"})
    assert all("window_end_ms" not in r for r in rows)
    assert all(r["decay_span_s"] <= 0.8 for r in rows)


def test_a_gate_the_cached_manifest_disagrees_on_is_refused_naming_both(tmp_path, capsys):
    root, cfg = _corpus(tmp_path, [1500, 800], gate_ms=800)
    refused = profile_module.measure({**cfg, "gate_ms": 1140}, root, tmp_path / "p.json")
    err = capsys.readouterr().err
    assert refused == 2
    assert "gate_ms 800" in err and "gate_ms 1140" in err
    assert not (tmp_path / "p.json").exists()


def test_the_model_is_held_to_each_rows_own_window_end():
    row = {"window_end_ms": 1234}
    assert profile_module.row_gate_s(row, 0.5) == pytest.approx(1.234)
    assert profile_module.row_gate_s({}, 0.5) == 0.5
    assert profile_module.row_gate_s(None, 0.5) == 0.5


def test_the_probe_holds_each_slot_for_its_own_window_end(tmp_path):
    root, _cfg = _corpus(tmp_path, [1500, 1100, 800], gate_ms=800)
    pattern = corpus_pattern(load_corpus(root))
    assert [n.dur for n in pattern.notes] == pytest.approx([1.5, 1.1, 0.8])
