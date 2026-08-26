"""The bank index: that every voice is addressable and resolves to something."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bank  # noqa: E402
from phrases import TAKE_SETS  # noqa: E402


def test_the_whole_bank_is_addressable():
    """Every program listed, and no two sharing a directory name.

    A collision is silent and destructive: the second voice's renders overwrite
    the first's inside a directory whose manifest still describes the first.
    """
    entries = bank.voices(kits=sorted(bank.KIT_NAMES))
    assert len(entries) == 128 + len(bank.KIT_NAMES)
    slugs = [v.slug for v in entries]
    assert len(slugs) == len(set(slugs))
    for slug in slugs:
        assert slug == slug.lower()
        assert all(c.isalnum() or c == "-" for c in slug), slug


def test_a_bank_variation_gets_its_own_slug():
    """A variation is a separate patch, so it is a separate page."""
    capital, variation = bank.Voice(program=19), bank.Voice(program=19, bank=8)
    assert capital.slug != variation.slug
    assert "b008" in variation.slug


def test_every_voice_names_a_phrase_set_that_exists():
    for voice in bank.voices(kits=sorted(bank.KIT_NAMES)):
        assert voice.take_set in TAKE_SETS, voice.label


def test_a_kit_and_a_piano_share_program_zero_and_nothing_else():
    """The number says nothing about which of the two it selects."""
    piano = bank.Voice(program=0)
    kit = bank.Voice(program=0, kit=True)
    assert piano.slug != kit.slug
    assert piano.take_set != kit.take_set
    assert kit.group == bank.DRUM_GROUP


def test_a_capture_is_matched_to_its_own_voice_and_not_another():
    """Program 0 has two captures — the grand and the kit — and one number."""
    pool = bank.captures()
    if not pool:
        pytest.skip("no capture definitions in this checkout")
    melodic = bank.capture_for(0, pool=pool)
    kit = bank.capture_for(0, kit=True, pool=pool)
    if melodic is not None:
        assert not melodic.drums
    if kit is not None:
        assert kit.drums
    if melodic is not None and kit is not None:
        assert melodic.id != kit.id


def test_a_captured_voice_keeps_the_captures_phrase_set():
    """The reference archive is keyed by take id.

    Given a different set from the one its references were rendered against, a
    captured voice finds none of them and drops to model-only without a word.
    """
    for capture in bank.captures():
        voice = bank.Voice(program=capture.program, bank=capture.bank,
                           kit=capture.drums, capture=capture)
        assert voice.take_set == capture.take_set


def test_an_uncaptured_voice_is_a_voice_all_the_same():
    """The index is the bank; a reference is an attachment to an entry."""
    voice = bank.Voice(program=96)
    assert voice.capture is None
    assert voice.take_set in TAKE_SETS
    assert voice.slug and voice.title
    assert "capture" not in voice.describe()


def test_describe_says_which_voice_answered():
    described = bank.Voice(program=40, patch="violin").describe()
    assert described["program"] == 40
    assert described["patch"] == "violin"
    assert described["tone_class"] == "sustained"


@pytest.mark.parametrize("spec,expected", [
    ("", []),
    ("40", [40]),
    ("0-3", [0, 1, 2, 3]),
    ("7,0-2,7", [0, 1, 2, 7]),
    (" 5 , 6 ", [5, 6]),
])
def test_parse_selection(spec, expected):
    assert bank.parse_selection(spec) == expected


def test_parse_selection_all_is_the_whole_bank():
    assert bank.parse_selection("all") == list(range(128))


@pytest.mark.parametrize("spec", ["128", "-1", "126-130"])
def test_parse_selection_refuses_what_is_not_a_program(spec):
    with pytest.raises(ValueError):
        bank.parse_selection(spec)


def test_the_index_describes_the_directory_rather_than_the_last_command(tmp_path):
    """A directory is filled a few programs at a time."""
    bank.write_index(tmp_path, [bank.Voice(program=40)])
    bank.write_index(tmp_path, [bank.Voice(program=6)])
    entries = json.loads(bank.index_path(tmp_path).read_text())
    assert [e["program"] for e in entries] == [6, 40]


def test_the_index_survives_a_damaged_one(tmp_path):
    bank.index_path(tmp_path).write_text("not json")
    bank.write_index(tmp_path, [bank.Voice(program=40)])
    assert json.loads(bank.index_path(tmp_path).read_text())[0]["program"] == 40
