"""The committed Bach excerpts, and the take built from one.

The excerpts are note data cut from a sibling corpus and committed, so these run
in a clone with nothing installed. What needs `SONARE_BACH_ROOT` is re-cutting
one, which `extract_excerpt.py --check` does and this does not.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import phrases  # noqa: E402
from patterns import registers_for_program  # noqa: E402
from toneclass import ToneClass  # noqa: E402

EXCERPTS = sorted(phrases.EXCERPT_DIR.glob("*.json"))


# --------------------------------------------------------------------------- #
# The committed files
# --------------------------------------------------------------------------- #

def test_every_class_but_noise_has_an_excerpt():
    """A class with none has no musical take, so every program in it is judged
    on isolated notes alone. Only `NOISE` is meant to be in that position."""
    for cls in ToneClass:
        ident = phrases.MUSIC_EXCERPTS[cls]
        if cls is ToneClass.NOISE:
            assert ident == ""
        else:
            assert ident, cls


def test_every_named_excerpt_is_committed():
    """A missing file makes the take vanish silently, which reads as a page that
    never had one."""
    for cls, ident in phrases.MUSIC_EXCERPTS.items():
        if ident:
            assert phrases.load_excerpt(ident), f"{cls}: {ident}"


@pytest.mark.parametrize("path", EXCERPTS, ids=lambda p: p.stem)
def test_an_excerpt_carries_its_provenance(path: Path):
    """Note data with no source is a passage nobody can re-cut or check."""
    data = json.loads(path.read_text())
    src = data["source"]
    assert src["bwv"] and src["work"] and src["roles"]
    assert len(src["beats"]) == 2 and src["beats"][1] > src["beats"][0]
    assert data["label"] and data["note"]


@pytest.mark.parametrize("path", EXCERPTS, ids=lambda p: p.stem)
def test_an_excerpt_is_about_ten_seconds(path: Path):
    """Long enough for a line to become one, short enough to hear twice."""
    data = json.loads(path.read_text())
    assert 6.0 <= data["seconds"] <= 20.0, data["seconds"]
    assert data["notes"]


@pytest.mark.parametrize("path", EXCERPTS, ids=lambda p: p.stem)
def test_an_excerpt_starts_where_every_other_take_does(path: Path):
    data = json.loads(path.read_text())
    assert min(n["start"] for n in data["notes"]) == pytest.approx(0.3, abs=1e-3)


@pytest.mark.parametrize("path", EXCERPTS, ids=lambda p: p.stem)
def test_every_note_is_playable(path: Path):
    data = json.loads(path.read_text())
    for n in data["notes"]:
        assert 0 <= n["pitch"] <= 127
        assert 1 <= n["velocity"] <= 127
        assert n["duration"] > 0


# --------------------------------------------------------------------------- #
# The take
# --------------------------------------------------------------------------- #

def test_the_musical_take_is_off_unless_asked_for():
    """It is not a measurement: nothing scores ten seconds of polyphony, and the
    take-measurement path reads a set as isolated notes against a reference."""
    assert not [t for t in phrases.build_takes("piano", 0) if t.id == "music"]
    assert [t for t in phrases.build_takes("piano", 0, music="") if t.id == "music"]


def test_a_kit_gets_no_musical_take():
    """A note number selects an instrument there, so a melodic line is 48 drums."""
    assert not [t for t in phrases.build_takes("drums", 0, music="") if t.id == "music"]


def test_a_capture_can_name_its_own_excerpt():
    """The organ needs a pedal part; the class default is one line."""
    got = phrases.music_take(19, excerpt="bwv639-chorale")
    assert got is not None
    assert got.label.startswith("Ich ruf zu dir")


def test_a_passage_already_in_the_compass_is_not_transposed():
    """Bach put a keyboard prelude where a keyboard is. Centring it on the
    program's middle register moves it down an octave, because the arpeggios
    reach above the middle and pull the median up with them."""
    assert phrases._octave_shift([60, 72, 77], (36, 60, 84)) == 0


def test_a_passage_outside_the_compass_moves_by_whole_octaves():
    """Anything but an octave puts the passage in a different key, which for a
    voice with fixed formants or a modal bank is a different instrument."""
    assert phrases._octave_shift([65, 74], (79, 91, 100)) == 2
    # Two octaves down puts 52-71 inside 28-52 exactly; one leaves it 7 over.
    assert phrases._octave_shift([52, 71], (28, 40, 52)) == -2


def test_the_take_lands_inside_the_program_it_was_built_for():
    """A glockenspiel asked to play a cello line is measuring a transposition."""
    for program in (0, 6, 9, 12, 24, 32, 40, 73):
        take = phrases.music_take(program)
        if take is None:
            continue
        lo, _, hi = registers_for_program(program)
        low = min(n.note for n in take.notes)
        high = max(n.note for n in take.notes)
        # Not a hard compass — the probe triple is three points, not a range —
        # so this only asks that the passage is not an octave clear of it.
        assert low > lo - 12, (program, low, lo)
        assert high < hi + 12, (program, high, hi)


def test_the_take_says_when_it_moved():
    """A page read weeks later cannot tell a transposed passage from a voice
    whose register is wrong."""
    moved = phrases.music_take(9)
    assert moved is not None and "octave" in moved.sub
    assert "octave" not in phrases.music_take(0).sub


def test_a_missing_excerpt_drops_the_take_rather_than_failing():
    """A page with one take fewer, not a run that stops."""
    assert phrases.music_take(0, excerpt="bwv0000-nothing") is None
