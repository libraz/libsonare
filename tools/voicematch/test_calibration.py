"""Recorded calibration settings: that they load, and that they name real voices."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bank
import calibration
from _repo import REPO_ROOT


def _write(tmp_path: Path, payload: dict) -> Path:
    path = tmp_path / "calibrations.json"
    path.write_text(json.dumps(payload))
    return path


def _variant(name: str, overrides: str = "x=1", **extra) -> dict:
    """A recorded setting carrying the fields every one is required to carry.

    Built here rather than spelled out in each fixture so a test says only what
    it is about. The loader refuses a setting without a title and a line in both
    languages, and a dozen fixtures restating that is a dozen places to edit the
    next time the schema grows.
    """
    return {
        "name": name,
        "overrides": overrides,
        "title": {"en": name, "ja": name},
        "desc": {"en": f"what {name} is for", "ja": f"{name} の狙い"},
        **extra,
    }


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


def test_every_patch_key_names_the_patch_of_the_voice_it_is_filed_under():
    """A patch prefix reaches that patch and no other, and says nothing when it misses.

    Where one patch is built by copying another — `overdriven` and `distortion`
    from `electric_guitar` — the copy is taken before either is tuned, so a key
    aimed at the source renders the copy byte-identical to the unmodified build.
    The page then shows a candidate that changes nothing, which reads as a knob
    with no effect rather than as a setting filed under the wrong voice.
    """
    status = json.loads((REPO_ROOT / "tools" / "voice-status.json").read_text())
    patches = {v["slug"]: v.get("patch") for v in status["voices"]}
    known = {p for p in patches.values() if p}
    wrong = []
    for slug, variants in calibration.load().items():
        for variant in variants:
            for assignment in variant.overrides.split(","):
                prefix = assignment.split("=", 1)[0].split(".", 1)[0]
                if prefix in known and prefix != patches.get(slug):
                    wrong.append(f"{slug}/{variant.name}: {prefix} voices another patch")
    assert wrong == []


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
    path = _write(
        tmp_path,
        {
            "p040-violin": {
                "variants": [
                    _variant("b"),
                    _variant("a", "x=2"),
                ]
            }
        },
    )
    assert [v.name for v in calibration.load(path)["p040-violin"]] == ["b", "a"]


def test_a_voice_with_no_settings_is_not_in_the_table(tmp_path):
    path = _write(tmp_path, {"p040-violin": {"variants": []}})
    assert calibration.load(path) == {}


@pytest.mark.parametrize("name", ["", "model", "has space", "has/slash"])
def test_a_name_that_cannot_be_a_button_is_refused(tmp_path, name):
    """The name is the button, the file stem and the last segment of the address."""
    path = _write(tmp_path, {"p040-violin": {"variants": [_variant(name)]}})
    with pytest.raises(ValueError):
        calibration.load(path)


@pytest.mark.parametrize("field", ["title", "desc"])
@pytest.mark.parametrize("broken", [None, {}, {"en": "only English"}, "a bare string"])
def test_a_setting_without_a_line_in_both_languages_is_refused(tmp_path, field, broken):
    """A setting worth recording is worth a sentence, in both languages.

    Refused where it is written rather than reported by a test somebody runs
    later, because the failure it prevents is silent: a button with no title
    falls back to its own key, and a key is an identifier that says which knob
    moved and never says what the setting is FOR. A page of nine of those is
    what this field exists to end.
    """
    item = _variant("warm")
    if broken is None:
        del item[field]
    else:
        item[field] = broken
    path = _write(tmp_path, {"p040-violin": {"variants": [item]}})
    with pytest.raises(ValueError, match=field):
        calibration.load(path)


def test_a_setting_the_shell_supplied_needs_no_line():
    """`--variant` cannot carry two languages and the person who typed it is the
    person listening, so it falls back to its own name and is not refused."""
    (variant,) = calibration.parse_cli(["adhoc=x=1"])
    assert variant.text("ja") == {"title": "adhoc", "desc": ""}


def test_a_line_falls_through_to_english_when_the_translation_is_absent():
    """A half-translated page is readable; a blank one is not. The registry is
    held to both languages, and everything else on the page falls through."""
    variant = calibration.Variant(
        "warm", "x=1", title={"en": "Warmer"}, desc={"en": "rounder, less edge"}
    )
    assert variant.text("ja")["title"] == "Warmer"
    assert variant.text("ja")["desc"] == "rounder, less edge"


def test_the_direct_version_is_marked_in_every_language():
    """A voice with a rig renders each candidate twice, and the two differ only
    in where the signal was taken from — so the button has to say which."""
    variant = calibration.Variant(
        "warm", "x=1", title={"en": "Warmer", "ja": "暖かく"}, desc={"en": "e", "ja": "j"}
    )
    plain = calibration.source_text(variant)
    direct = calibration.source_text(variant, direct=True)
    assert plain["title"] == {"en": "Warmer", "ja": "暖かく"}
    assert set(direct["title"]) == {"en", "ja"}
    assert all(direct["title"][k] != plain["title"][k] for k in plain["title"])
    assert direct["desc"] == plain["desc"]


def test_one_voice_naming_a_setting_twice_is_refused(tmp_path):
    path = _write(
        tmp_path,
        {
            "p040-violin": {
                "variants": [
                    _variant("warm"),
                    _variant("warm", "x=2"),
                ]
            }
        },
    )
    with pytest.raises(ValueError):
        calibration.load(path)


# --------------------------------------------------------------------------- #
# Resolution against a run
# --------------------------------------------------------------------------- #


def test_recorded_settings_come_before_the_runs_own(tmp_path):
    path = _write(tmp_path, {"p040-violin": {"variants": [_variant("recorded")]}})
    table = calibration.load(path)
    merged = calibration.for_voice("p040-violin", table, calibration.parse_cli(["adhoc=y=2"]))
    assert [v.name for v in merged] == ["recorded", "adhoc"]


def test_a_voice_with_nothing_recorded_gets_the_runs_own(tmp_path):
    merged = calibration.for_voice("p073-flute", {}, calibration.parse_cli(["a=x=1"]))
    assert [v.name for v in merged] == ["a"]


def test_a_name_declared_twice_over_is_refused(tmp_path):
    """Whichever won, the note written about it would name the other just as well."""
    path = _write(tmp_path, {"p040-violin": {"variants": [_variant("warm")]}})
    table = calibration.load(path)
    with pytest.raises(ValueError, match="warm"):
        calibration.for_voice("p040-violin", table, calibration.parse_cli(["warm=x=2"]))


def test_settings_are_per_voice(tmp_path):
    """The whole point: one run, different candidates per voice."""
    path = _write(
        tmp_path,
        {
            "p040-violin": {"variants": [_variant("bow")]},
            "p073-flute": {"variants": [_variant("jet", "y=1")]},
        },
    )
    table = calibration.load(path)
    assert [v.name for v in calibration.for_voice("p040-violin", table, [])] == ["bow"]
    assert [v.name for v in calibration.for_voice("p073-flute", table, [])] == ["jet"]


# --------------------------------------------------------------------------- #
# The command line
# --------------------------------------------------------------------------- #


def test_parse_cli_keeps_the_overrides_untouched():
    """The library is the only thing that can say whether a key exists."""
    (variant,) = calibration.parse_cli(["a=piano_voice.kX=0.5,piano_voice.kY=2"])
    assert variant.overrides == "piano_voice.kX=0.5,piano_voice.kY=2"


def test_parse_cli_accepts_an_empty_override_set():
    """A second copy of the baseline is a legitimate thing to want on a page."""
    (variant,) = calibration.parse_cli(["control="])
    assert variant.overrides == ""


@pytest.mark.parametrize("spec", ["noequals", "=x=1", "model=x=1"])
def test_parse_cli_refuses_what_cannot_be_a_version(spec):
    with pytest.raises(ValueError):
        calibration.parse_cli([spec])


def test_the_page_is_never_shown_the_override_string():
    """A question put in the parameter's own vocabulary gets the parameter's own
    answer back, so the knob names stay in this file and off the page."""
    variant = calibration.Variant(
        "warm", "piano.brightness=0.3", "the reference is 5 dB down at h7"
    )
    assert variant.detail == "the reference is 5 dB down at h7"
    assert "brightness" not in variant.detail
    assert calibration.Variant("bare", "x=1").detail == ""
    # A control with nothing overridden is not a knob name, and saying so is
    # what keeps it from reading as a setting whose line went missing.
    assert calibration.Variant("control", "").detail == "no overrides"


@pytest.mark.parametrize(
    ("given", "want"),
    [
        ("the felt is flat — fam0.piano.brightness=0.30", "the felt is flat"),
        ("two moved — a.b=1,c.d=2.5", "two moved"),
        ("a line with no override string", "a line with no override string"),
        # A setting with no note had the override string as its whole line, which
        # is what a rule written around the separator walks straight past.
        ("violin.bowed_string.bow_force=0.9", ""),
        ("a.b=1,c.d=2", ""),
        # A note is prose and prose has dashes in it. Only a tail that parses as
        # assignments is taken off, so a sentence ending in one survives.
        ("compared against 2 references — both dark", "compared against 2 references — both dark"),
        ("", ""),
    ],
)
def test_an_override_string_is_taken_off_a_page_rendered_before_this(given, want):
    """A hundred and eighty-odd manifests carry `note — a.b=1` in the field the
    banner reads, and re-rendering one to drop it is hours of audio."""
    assert calibration.strip_overrides(given) == want


def test_every_drum_note_a_recorded_setting_moves_is_struck_by_a_take():
    """Recording a candidate is only half of the mechanism — the other half is
    hearing it, and a drum note no take strikes cannot be heard at all. The page
    still builds, every version of every take renders, and all of them are the
    same audio, which looks exactly like a library built without the override
    layer.

    This holds the take set to the settings that exist, not to the whole kit:
    a note nothing is recorded against needs no take, and demanding one would
    turn a listening page into a roll call of 47 drums.
    """
    from make_audition import _DRUM_KEY
    from phrases import build_takes

    struck = {n.note for take in build_takes("drums", 0) for n in take.notes}
    unheard: dict[str, list[int]] = {}
    for slug, variants in calibration.load().items():
        if not slug.startswith("kit"):
            continue
        for variant in variants:
            named = {int(m.group(1)) for m in _DRUM_KEY.finditer(variant.overrides)}
            if named - struck:
                unheard[f"{slug}/{variant.name}"] = sorted(named - struck)
    assert not unheard
