"""The inputs an autofit test constructs: knobs, loss terms, the probe and
fit argument namespaces, and a miniature captured corpus on disk.

Each of these is reached from more than one of the autofit test modules;
a builder only one of them uses stays beside the tests that use it.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import json

from knobs import Knob
from loss import LOSS_TERMS
from wavio import write_wav


def _terms(**kwargs) -> dict[str, float]:
    out = {name: 0.0 for name in LOSS_TERMS}
    out.update(kwargs)
    return out


def _knob(label: str) -> Knob:
    return Knob(label=label, lo=0.0, hi=1.0, log=False, start_value=0.5, tunable=label)


def _probe_args(**kwargs) -> argparse.Namespace:
    """A Namespace carrying what the probe, the weights and the oracle routes read."""
    base = {
        "program": 0,
        "drum_note": None,
        "pattern": "sustain",
        "notes": "",
        "velocities": "",
        # Every weight is None, exactly as the parser leaves one that was not
        # given, so the instrument's class defaults are what these tests see.
        "w_harm": None,
        "w_cents": None,
        "w_tnr": None,
        "w_env": None,
        "w_init": None,
        "w_slope": None,
        "w_mss": None,
        "w_band": None,
        "w_bdecay": None,
        "n_harm": 10,
        "w_tail": None,
        "w_hf": None,
        "w_level": None,
        "w_crest": None,
        "w_lf": None,
        "w_stiff": None,
        "w_dyn": None,
        "w_modes": None,
        "w_mod": None,
        "w_kit": None,
        "corpus": "",
        "corpus_timbre": "",
        "spec": "auto",
        "oracle_wav": "",
        "au": "",
        "au_dry": False,
        "room": "auto",
        "validate_notes": "",
        "validate_velocities": "",
        "validate_oracle_wav": "",
    }
    base.update(kwargs)
    return argparse.Namespace(**base)


def _source_knob(tmp_path: Path, name: str, literal: str) -> tuple[Path, Knob]:
    """A file holding one numeric literal, and the source knob that points at it."""
    path = tmp_path / name
    head, tail = "constexpr float kValue = ", "f;\n"
    path.write_text(head + literal + tail)
    return path, Knob(
        label=name,
        lo=0.0,
        hi=10.0,
        log=False,
        start_value=float(literal),
        file=path,
        pattern=r"kValue = ([0-9.]+)f",
        span_start=len(head),
        span_end=len(head) + len(literal),
    )


def _fit_args(**kwargs) -> argparse.Namespace:
    # `no_cache` by default: the store lives under the scratch root a real run
    # shares, and a test that writes into it would seed a fit with terms no
    # library ever produced. The tests that DO exercise it point the root
    # somewhere of their own and turn it back on.
    base = {
        "raw_loss": False,
        "workers": 1,
        "cmake": "cmake",
        "jobs": 1,
        "n_harm": 10,
        "percussive": False,
        "no_cache": True,
    }
    base.update(kwargs)
    return _probe_args(**base)


def _write_corpus(
    root: Path,
    *,
    notes=(60, 72),
    velocities=(56, 120),
    gate_ms=8000,
    seconds=10.1,
    preroll_ms=100,
    dry=True,
    channel=1,
    groups=None,
    rig=None,
    room=None,
    note_map=None,
) -> Path:
    """A miniature capture: one short tone per slot, plus the manifest beside it.

    `seconds` is a number for a grid captured at one flat tail, or a note-keyed
    dict for one that recorded longer for some of its notes — which is what
    `tail_by_note` produces and what a single slot length cannot describe.
    """
    sr = 48000
    root.mkdir(parents=True, exist_ok=True)
    renders = []
    for note in notes:
        secs = seconds[note] if isinstance(seconds, dict) else seconds
        for vel in velocities:
            rel = f"t/n{note:03d}_v{vel:03d}.wav"
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            n = int(secs * sr)
            t = np.arange(n) / sr
            # Silence through the preroll, then a decaying tone at the note's own
            # pitch, so a misplaced onset shows up as a measurable shift.
            body = np.sin(2 * np.pi * 440.0 * 2 ** ((note - 69) / 12.0) * t)
            body *= np.exp(-t * 1.5) * (vel / 127.0)
            body[: int(preroll_ms / 1000.0 * sr)] = 0.0
            write_wav(path, np.stack([body, body], axis=1).astype(np.float32), sr)
            renders.append(
                {
                    "id": rel,
                    "timbre": "t",
                    "note": note,
                    "velocity": vel,
                    "path": rel,
                    "seconds": secs,
                }
            )
    header = {
        "id": "mini",
        "sample_rate": sr,
        "gate_ms": gate_ms,
        "tail": "2s",
        "preroll_ms": preroll_ms,
        "dry": dry,
        "timbres": [{"id": "t", "label": "mini timbre", "channel": channel}],
        "notes": list(notes),
        "velocities": list(velocities),
        "renders": renders,
    }
    if groups is not None:
        header["groups"] = groups
    # Left out entirely unless asked for, so a manifest that never answered the
    # rig question can be written — which is what every corpus captured before
    # the field looks like.
    if rig is not None:
        header["rig"] = rig
    # Same rule as the rig: absent is how every corpus captured before the field
    # looks, and it has to stay distinguishable from an explicit `none`.
    if room is not None:
        header["room"] = room
    if note_map is not None:
        header["note_map"] = note_map
    (root / "manifest.json").write_text(json.dumps(header))
    return root


def _bounded_knob(label, lo, hi, start) -> Knob:
    return Knob(label=label, lo=lo, hi=hi, log=False, start_value=start, tunable=label)
