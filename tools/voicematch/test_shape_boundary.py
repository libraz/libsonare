"""The shape tool renders the model from the capture's own boundary and stimulus.

`build` turns a capture and its corpus into the `RenderRequest` the model side
renders from, refuses a fit against a reference that is not at the instrument's
boundary before anything renders, and keys its caches on the request. The render
itself is stubbed at the subprocess, which is where the request leaves the
process.

    python -m pytest tools/voicematch/test_shape_boundary.py
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shape import __main__ as shape_main
from shape import render as shape_render

CAPTURES = Path(__file__).resolve().parent / "capture"


def _corpus(tmp_path: Path, capture: str) -> Path:
    cap = json.loads((CAPTURES / f"{capture}.json").read_text())
    timbre = cap["timbres"][0]["id"]
    renders = [
        {
            "timbre": timbre,
            "note": n,
            "velocity": v,
            "path": f"{timbre}/n{n:03d}_v{v:03d}.wav",
            "seconds": 2.0,
        }
        for n in cap["notes"][:2]
        for v in cap["velocities"][:1]
    ]
    root = tmp_path / capture
    root.mkdir()
    (root / "manifest.json").write_text(json.dumps({**cap, "renders": renders}))
    return root


def _args(capture: str, corpus: Path, tmp_path: Path) -> argparse.Namespace:
    return argparse.Namespace(
        capture=capture,
        corpus=str(corpus),
        timbre="",
        notes="",
        velocities="",
        lib="",
        cache=str(tmp_path / "cache"),
        no_bed=True,
    )


@pytest.fixture
def worker_calls(monkeypatch, tmp_path):
    """Stub the render subprocess; collect what it was handed on stdin."""
    calls: list[dict] = []

    def fake_run(cmd, *, input, **_kw):
        calls.append(json.loads(input))
        pairs = [tuple(p) for p in json.loads(cmd[3])]
        np.savez(cmd[4], **{f"{n}_{v}": np.zeros(8, dtype=np.float32) for n, v in pairs})
        return subprocess.CompletedProcess(cmd, 0, "", "")

    monkeypatch.setattr(shape_render.subprocess, "run", fake_run)
    return calls


def _build(tmp_path, capture, *, fit):
    corpus = _corpus(tmp_path, capture)
    return shape_main.build(_args(capture, corpus, tmp_path), fit=fit)


def test_di_guitar_renders_with_the_rig_cleared_at_the_capture_boundary(tmp_path, worker_calls):
    sigs = _build(tmp_path, "electric_guitar_di", fit=True)[2]
    sigs([(60, 100)])
    request = worker_calls[0]["request"]
    assert request["program"] == 27
    assert request["rig"] is False
    assert request["sample_rate"] == 48000
    assert request["sends"] == [0, 0, 0]
    assert worker_calls[0]["preroll_s"] == 0.5


def test_a_baked_guitar_is_refused_for_a_fit_before_anything_renders(tmp_path, worker_calls):
    with pytest.raises(SystemExit, match="carries a rig"):
        _build(tmp_path, "overdriven_guitar", fit=True)
    assert worker_calls == []


def test_an_unclassified_guitar_is_refused_for_a_fit(tmp_path, worker_calls):
    with pytest.raises(SystemExit, match="nothing says whether"):
        _build(tmp_path, "jazz_guitar", fit=True)
    assert worker_calls == []


def test_a_diagnostic_run_against_a_baked_guitar_proceeds_and_says_so(
    tmp_path, worker_calls, capsys
):
    sigs = _build(tmp_path, "overdriven_guitar", fit=False)[2]
    assert "diagnostic run" in capsys.readouterr().err
    sigs([(60, 100)])
    assert worker_calls[0]["request"]["rig"] is True


def test_a_variation_bank_capture_renders_at_its_bank_and_rate(tmp_path, worker_calls):
    sigs = _build(tmp_path, "mandolin", fit=True)[2]
    sigs([(60, 100)])
    request = worker_calls[0]["request"]
    assert (request["program"], request["bank"], request["sample_rate"]) == (25, 16, 44100)
    assert worker_calls[0]["preroll_s"] == 0.0


def test_the_cache_key_follows_bank_rate_and_rig(tmp_path, worker_calls):
    sigs = _build(tmp_path, "mandolin", fit=True)[2]
    pairs = [(60, 100)]
    base = sigs._key(pairs, "", False)
    for change in ({"bank": 8}, {"sample_rate": 48000}, {"rig": not sigs.request.rig}):
        other = dataclasses.replace(sigs, request=dataclasses.replace(sigs.request, **change))
        assert other._key(pairs, "", False) != base, change


def test_candidate_overrides_are_recorded_in_the_worker_request(tmp_path, worker_calls):
    sigs = _build(tmp_path, "mandolin", fit=True)[2]
    override = "Mandolin.FilterCutoffHz=3200"
    sigs._render([(60, 100)], override, False, tmp_path / "candidate.npz")
    assert worker_calls[0]["request"]["overrides"] == override
