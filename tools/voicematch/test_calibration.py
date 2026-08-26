"""Recorded calibration settings: that they load, and that they name real voices."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bank  # noqa: E402
import calibration  # noqa: E402


def _write(tmp_path: Path, payload: dict) -> Path:
    path = tmp_path / "calibrations.json"
    path.write_text(json.dumps(payload))
    return path


# --------------------------------------------------------------------------- #
# The shipped file
# --------------------------------------------------------------------------- #

def test_the_shipped_file_loads():
    """A syntax error here is found by a run that has already started rendering."""
    assert isinstance(calibration.load(), dict)


def test_every_recorded_voice_is_a_voice_the_bank_has():
    """A typo costs nothing at the time and everything later.

    The run finds no settings for the voice, renders the baseline alone, and
    produces a page indistinguishable from one for a voice nobody has recorded
    a candidate for.
    """
    table = calibration.load()
    known = {v.slug for v in bank.voices(kits=sorted(bank.KIT_NAMES))}
    assert calibration.unknown_voices(table, known) == []


def test_documentation_keys_are_not_voices():
    """The explanation lives beside the thing it explains, as the captures do."""
    raw = json.loads(calibration.DEFAULT_PATH.read_text())
    assert any(k.startswith(calibration.DOC_PREFIX) for k in raw), "no doc keys at all"
    assert not any(k.startswith(calibration.DOC_PREFIX) for k in calibration.load())


# --------------------------------------------------------------------------- #
# Loading
# --------------------------------------------------------------------------- #

def test_an_absent_file_is_an_empty_one():
    assert calibration.load(Path("/nonexistent/calibrations.json")) == {}


def test_a_voice_keeps_its_settings_in_the_order_written(tmp_path):
    """The page shows them in this order, so it is the order that was chosen."""
    path = _write(tmp_path, {"p040-violin": {"variants": [
        {"name": "b", "overrides": "x=1"},
        {"name": "a", "overrides": "x=2"},
    ]}})
    assert [v.name for v in calibration.load(path)["p040-violin"]] == ["b", "a"]


def test_a_voice_with_no_settings_is_not_in_the_table(tmp_path):
    path = _write(tmp_path, {"p040-violin": {"variants": []}})
    assert calibration.load(path) == {}


@pytest.mark.parametrize("name", ["", "model", "has space", "has/slash"])
def test_a_name_that_cannot_be_a_button_is_refused(tmp_path, name):
    """The name is the button, the file stem and the last segment of the address."""
    path = _write(tmp_path, {"p040-violin": {"variants": [
        {"name": name, "overrides": "x=1"}]}})
    with pytest.raises(ValueError):
        calibration.load(path)


def test_one_voice_naming_a_setting_twice_is_refused(tmp_path):
    path = _write(tmp_path, {"p040-violin": {"variants": [
        {"name": "warm", "overrides": "x=1"},
        {"name": "warm", "overrides": "x=2"},
    ]}})
    with pytest.raises(ValueError):
        calibration.load(path)


# --------------------------------------------------------------------------- #
# Resolution against a run
# --------------------------------------------------------------------------- #

def test_recorded_settings_come_before_the_runs_own(tmp_path):
    path = _write(tmp_path, {"p040-violin": {"variants": [
        {"name": "recorded", "overrides": "x=1"}]}})
    table = calibration.load(path)
    merged = calibration.for_voice(
        "p040-violin", table, calibration.parse_cli(["adhoc=y=2"]))
    assert [v.name for v in merged] == ["recorded", "adhoc"]


def test_a_voice_with_nothing_recorded_gets_the_runs_own(tmp_path):
    merged = calibration.for_voice("p073-flute", {}, calibration.parse_cli(["a=x=1"]))
    assert [v.name for v in merged] == ["a"]


def test_a_name_declared_twice_over_is_refused(tmp_path):
    """Whichever won, the note written about it would name the other just as well."""
    path = _write(tmp_path, {"p040-violin": {"variants": [
        {"name": "warm", "overrides": "x=1"}]}})
    table = calibration.load(path)
    with pytest.raises(ValueError, match="warm"):
        calibration.for_voice("p040-violin", table, calibration.parse_cli(["warm=x=2"]))


def test_settings_are_per_voice(tmp_path):
    """The whole point: one run, different candidates per voice."""
    path = _write(tmp_path, {
        "p040-violin": {"variants": [{"name": "bow", "overrides": "x=1"}]},
        "p073-flute": {"variants": [{"name": "jet", "overrides": "y=1"}]},
    })
    table = calibration.load(path)
    assert [v.name for v in calibration.for_voice("p040-violin", table, [])] == ["bow"]
    assert [v.name for v in calibration.for_voice("p073-flute", table, [])] == ["jet"]


# --------------------------------------------------------------------------- #
# The command line
# --------------------------------------------------------------------------- #

def test_parse_cli_keeps_the_overrides_untouched():
    """The library is the only thing that can say whether a key exists."""
    variant, = calibration.parse_cli(["a=piano_voice.kX=0.5,piano_voice.kY=2"])
    assert variant.overrides == "piano_voice.kX=0.5,piano_voice.kY=2"


def test_parse_cli_accepts_an_empty_override_set():
    """A second copy of the baseline is a legitimate thing to want on a page."""
    variant, = calibration.parse_cli(["control="])
    assert variant.overrides == ""


@pytest.mark.parametrize("spec", ["noequals", "=x=1", "model=x=1"])
def test_parse_cli_refuses_what_cannot_be_a_version(spec):
    with pytest.raises(ValueError):
        calibration.parse_cli([spec])


def test_the_detail_leads_with_why_rather_than_what():
    """The override string says what moved and never says what it was for."""
    variant = calibration.Variant("warm", "x=1", "the reference is 5 dB down at h7")
    assert variant.detail.startswith("the reference is 5 dB down at h7")
    assert "x=1" in variant.detail
    assert calibration.Variant("bare", "x=1").detail == "x=1"
    assert calibration.Variant("control", "").detail == "no overrides"
