"""Tests for the default-is-the-identity check.

The rendering half needs two builds of the library and cannot run here, so what
is tested is the part that decides -- which is where the check can go wrong in
the direction that matters. A check of this kind fails safe only if it treats
"nothing moved anywhere" as a failure, and that is the branch a hand reading of
the output routinely gets backwards.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import identity  # noqa: E402


def _hashes(mapping, default="same"):
    """A stand-in for `render_hash` keyed by (library, note, overrides)."""
    def fake(lib, note, program, channel, overrides=""):
        return mapping.get((lib, note, overrides), default)
    return fake


def test_a_drum_note_and_a_program_are_addressed_differently():
    note, program, channel, ov = identity.parse_reach("36:d036.percussion.plate_gain=1.0")
    assert (note, program, channel) == (36, 0, identity.PERCUSSION_CHANNEL)
    assert ov == "d036.percussion.plate_gain=1.0"

    note, program, channel, ov = identity.parse_reach("p40:violin.bowed_string.bow_force=2")
    assert (program, channel) == (40, 0)
    assert ov == "violin.bowed_string.bow_force=2"

    with pytest.raises(ValueError):
        identity.parse_reach("36")


def test_identical_renders_are_not_a_pass_when_no_override_reached_them(monkeypatch, capsys):
    # Every render hashes the same, including the one the override was meant to
    # change: a library that ignores overrides, a key that does not resolve and
    # a branch that is never taken all look exactly like this.
    monkeypatch.setattr(identity, "render_hash", _hashes({}))
    rc = identity.main(["--base", "b.dylib", "--head", "h.dylib", "--drums", "36",
                        "--reach", "36:d036.percussion.plate_gain=1.0"])
    assert rc == 1
    assert "vacuous" in capsys.readouterr().out


def test_a_default_that_changed_another_voice_fails(monkeypatch, capsys):
    moved = _hashes({("h.dylib", 38, ""): "moved",
                     ("h.dylib", 36, "d036.percussion.plate_gain=1.0"): "on"})
    monkeypatch.setattr(identity, "render_hash", moved)
    rc = identity.main(["--base", "b.dylib", "--head", "h.dylib", "--drums", "36,38",
                        "--reach", "36:d036.percussion.plate_gain=1.0"])
    assert rc == 1
    out = capsys.readouterr().out
    assert "drum 38" in out.split("the identity for")[1]


def test_both_halves_together_are_the_pass(monkeypatch, capsys):
    monkeypatch.setattr(identity, "render_hash", _hashes(
        {("h.dylib", 36, "d036.percussion.plate_gain=1.0"): "on"}))
    rc = identity.main(["--base", "b.dylib", "--head", "h.dylib", "--drums", "36,38",
                        "--programs", "0", "--reach",
                        "36:d036.percussion.plate_gain=1.0"])
    assert rc == 0
    assert "3 voices bit-identical" in capsys.readouterr().out
