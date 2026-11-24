from __future__ import annotations

import json
import sys
import wave
from pathlib import Path
from runpy import run_path
from types import SimpleNamespace

import libsonare
import numpy as np
import render_model


def test_corpus_renders_a_selected_program_with_the_bank_rig(tmp_path, monkeypatch):
    rendered_frames = []

    class Project:
        def import_smf(self, data):
            assert data.startswith(b"MThd")

        def bounce(self, *, sf2_instruments, total_frames, num_channels):
            [binding] = sf2_instruments
            assert binding == libsonare.ProjectSf2InstrumentBinding(
                instrument=libsonare.Sf2InstrumentConfig()
            )
            rendered_frames.append(total_frames)
            return libsonare.ProjectBounceResult(
                channels=np.zeros((num_channels, total_frames), dtype=np.float32),
                sample_rate=48000,
            )

        def sound_font_manifest(self):
            return [SimpleNamespace(program=27, backend="synth")]

        def close(self):
            pass

    script = Path(__file__).with_name("render_corpus.py")
    monkeypatch.setattr(libsonare, "Project", Project)
    monkeypatch.setattr(render_model, "ensure_lib_path", lambda: None)
    monkeypatch.setattr(sys, "argv", [str(script), "--out", str(tmp_path), "--programs", "27"])
    assert run_path(str(script))["main"]() == 0

    out_dir = tmp_path / "model"
    with wave.open(str(out_dir / "027_clean-electric.wav"), "rb") as wav:
        assert [wav.getnframes()] == rendered_frames
        assert wav.getframerate() == 48000
        assert wav.getnchannels() == 2
    metrics = json.loads((out_dir / "metrics.json").read_text())
    assert set(metrics) == {"027_clean-electric"}
