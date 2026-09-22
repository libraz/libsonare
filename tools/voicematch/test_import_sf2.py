"""Tests for the SoundFont corpus importer.

Every font here is built by `test_sf2._font`, so nothing needs a product. The
refusals matter more than the happy path: each one prevents something that would
otherwise produce a corpus every downstream metric computes happily over, and a
refusal nothing exercises is a comment.

The module dryness check is asserted as a PAIR — a dry corpus must pass and a wet
one must fail. Either alone stays green if the check stops measuring.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).parent))

import import_sf2
from corpus import load_corpus
from import_sf2 import ImportRefused, check_module_is_dry, import_corpus
from room import Room, synth_room_ir
from sf2 import UnplayableAsCorpus
from test_sf2 import _burst, _font

RATE = 44100


def _definition(tmp_path: Path, font: Path, preset: str, **overrides) -> Path:
    """A tracked capture definition plus its untracked overlay, on disk."""
    cfg = {
        "id": "probe",
        "label": "probe",
        "program": 80,
        "takes": "sustained",
        "notes": [60, 72],
        "velocities": [100],
        "gate_ms": 40,
        "preroll_ms": 0,
        "room": "none",
        "rig": "none",
        "source_class": "module",
        "timbres": [{"id": "t0", "label": "probe timbre"}],
    }
    cfg.update(overrides)
    path = tmp_path / "probe.json"
    path.write_text(json.dumps(cfg))
    path.with_suffix(".local.json").write_text(
        json.dumps(
            {
                "timbres": [{"id": "t0", "sf2": str(font), "preset": preset}],
            }
        )
    )
    return path


def _load(path: Path) -> dict:
    from capture import load_config

    return load_config(path)


@pytest.fixture
def font(tmp_path: Path) -> Path:
    return _font(
        tmp_path,
        zones=[(58, 62, 0), (70, 74, 1)],
        audio=[_burst(seconds=0.05, freq=261.6), _burst(seconds=0.05, freq=440.0)],
        pitches=[60, 72],
    )


def test_an_imported_corpus_loads_as_a_corpus(tmp_path: Path, font: Path):
    cfg = _load(_definition(tmp_path, font, "test"))
    out = tmp_path / "corpus"
    manifest = import_corpus(cfg, out)
    assert manifest["reach"] == {"requested": 2, "extracted": 2, "missing": {}}

    got = load_corpus(out)
    assert got.notes == (60, 72)
    assert got.velocities == (100,)
    assert got.sample_rate == RATE
    assert set(got.renders) == {(60, 100), (72, 100)}


def test_a_note_the_font_does_not_hold_is_reported_rather_than_dropped(tmp_path: Path, font: Path):
    """A grid asking for notes the font never recorded must say so by number.

    Silence here is the failure the reach field exists for: a screening run that
    found no divergence on a program has to be readable against how much of the
    grid was ever compared.
    """
    cfg = _load(_definition(tmp_path, font, "test", notes=[60, 66, 72, 84]))
    manifest = import_corpus(cfg, tmp_path / "corpus")
    assert manifest["reach"]["extracted"] == 2
    assert manifest["reach"]["requested"] == 4
    assert manifest["reach"]["missing"] == {"t0": [66, 84]}


def test_more_velocities_than_the_font_has_layers_is_refused(tmp_path: Path, font: Path):
    cfg = _load(_definition(tmp_path, font, "test", velocities=[32, 80, 127]))
    with pytest.raises(ImportRefused, match="same audio into every one of them"):
        import_corpus(cfg, tmp_path / "corpus")


def test_a_gate_longer_than_the_recordings_is_refused(tmp_path: Path, font: Path):
    """The bursts are 50 ms; this grid promises a 4 s analysis window."""
    cfg = _load(_definition(tmp_path, font, "test", gate_ms=4000))
    with pytest.raises(ImportRefused, match="shorter"):
        import_corpus(cfg, tmp_path / "corpus")


def test_a_nonzero_preroll_is_refused(tmp_path: Path, font: Path):
    cfg = _load(_definition(tmp_path, font, "test", preroll_ms=100))
    with pytest.raises(ImportRefused, match="preroll_ms: 0"):
        import_corpus(cfg, tmp_path / "corpus")


def test_a_font_that_needs_a_player_is_refused(tmp_path: Path):
    shaped = _font(
        tmp_path,
        zones=[(58, 62, 0), (70, 74, 1)],
        audio=[_burst(), _burst()],
        pitches=[60, 72],
        extra_igen=[(8, 6900)],  # a lowpass at ~1 kHz
        name="shaped",
    )
    cfg = _load(_definition(tmp_path, shaped, "shaped"))
    with pytest.raises(UnplayableAsCorpus, match="initialFilterFc"):
        import_corpus(cfg, tmp_path / "corpus")


def test_an_unknown_preset_name_is_refused_with_what_the_font_holds(tmp_path: Path, font: Path):
    cfg = _load(_definition(tmp_path, font, "Piano 2"))
    with pytest.raises(ImportRefused, match="no preset called"):
        import_corpus(cfg, tmp_path / "corpus")


def test_recordings_at_two_rates_are_refused_rather_than_resampled(tmp_path: Path):
    mixed = _font(
        tmp_path,
        zones=[(58, 62, 0), (70, 74, 1)],
        audio=[_burst(), _burst()],
        pitches=[60, 72],
        name="mixed",
    )
    # Rewrite one sample's rate in place: `_font` writes a single rate, and a
    # font mixing them is exactly what this refusal is for.
    raw = bytearray(mixed.read_bytes())
    at = raw.rindex(b"s1\x00")
    raw[at + 36 : at + 40] = (32000).to_bytes(4, "little")
    mixed.write_bytes(bytes(raw))
    cfg = _load(_definition(tmp_path, mixed, "mixed"))
    with pytest.raises(ImportRefused, match="One corpus"):
        import_corpus(cfg, tmp_path / "corpus")


# --------------------------------------------------------------------------
# `source_class: module` against the audio, both directions.


def _wet(dry: np.ndarray, rt60_s: float) -> np.ndarray:
    room = Room(rt60_s=rt60_s, hf_ratio=0.8, tail_db=12.0, predelay_ms=8.0)
    ir = synth_room_ir(room, RATE)
    ir = ir[:, 0] if ir.ndim > 1 else ir
    out = np.convolve(dry, ir)
    return out / max(float(np.max(np.abs(out))), 1e-9) * 0.7


@pytest.fixture
def module_cfg_and_corpus(tmp_path: Path):
    def build(audio: list[np.ndarray], gate_ms: int):
        f = _font(
            tmp_path, zones=[(58, 62, 0), (70, 74, 1)], audio=audio, pitches=[60, 72], name="mod"
        )
        cfg = _load(_definition(tmp_path, f, "mod", gate_ms=gate_ms))
        out = tmp_path / f"corpus{gate_ms}"
        return cfg, import_corpus(cfg, out), out

    return build


def test_a_dry_module_corpus_passes_the_dryness_check(module_cfg_and_corpus):
    dry = [_burst(seconds=1.0, freq=261.6), _burst(seconds=1.0, freq=440.0)]
    cfg, manifest, out = module_cfg_and_corpus(dry, 900)
    assert check_module_is_dry(cfg, manifest, out) == 0


def test_a_module_corpus_that_measures_a_space_fails(module_cfg_and_corpus, capsys):
    """The positive control for the check above.

    `capture.md` asserts `source_class: module` with `room: present` as a
    contradiction between two declarations. This is the measured half: the
    capture below declares `room: none` and its own audio says otherwise, which
    is the case a declaration cannot catch.
    """
    wet = [_wet(_burst(seconds=1.0, freq=261.6), 1.3), _wet(_burst(seconds=1.0, freq=440.0), 1.3)]
    cfg, manifest, out = module_cfg_and_corpus(wet, 900)
    assert cfg["room"] == "none", "the declaration must not be what fails it"
    assert check_module_is_dry(cfg, manifest, out) == 1
    assert "measures a space" in capsys.readouterr().err


def test_the_dryness_check_does_not_run_for_a_non_module_capture(
    tmp_path: Path, font: Path, monkeypatch
):
    """It is a module's own rule, so a library capture must not pay for it."""
    cfg = _load(_definition(tmp_path, font, "test", source_class="library"))
    out = tmp_path / "corpus"
    manifest = import_corpus(cfg, out)

    def explode(*_a, **_k):
        raise AssertionError("measure_rooms reached on a non-module capture")

    monkeypatch.setattr(import_sf2, "check_module_is_dry", check_module_is_dry)
    import profile as profile_mod

    monkeypatch.setattr(profile_mod, "measure_rooms", explode)
    assert check_module_is_dry(cfg, manifest, out) == 0
