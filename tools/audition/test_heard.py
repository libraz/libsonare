"""Reading the listening log back.

The failure worth catching is a summary that reads well and is about a render
nobody can hear any more. A note is dated and the voice under it moves, so the
worst verdict a voice carries and the worst verdict still standing are different
facts -- and the first is the one that sends somebody to fix a voice as it was
three weeks ago.
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import heard


def _log(root: Path, set_id: str, entries: list[dict]) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / f"{set_id}.jsonl").write_text(
        "".join(json.dumps(e, ensure_ascii=False) + "\n" for e in entries),
        encoding="utf-8")


def _note(at: str, grade: str, tag: str = "", text: str = "", hit: dict | None = None) -> dict:
    return {"at": at, "grade": grade, "tag": tag, "text": text, "lang": "en",
            "conditions": {"set": "p040-violin", "take": "single-long",
                           "take_label": "Single note", "version": "model",
                           "playhead": 1.5, "hit": hit}}


def _hit(n: int, *notes: int) -> dict:
    return {"n": n, "start": 0.3, "into": 0.2,
            "notes": [{"note": note, "velocity": 100} for note in notes]}


def _bank(root: Path, units: dict[str, list[str]]) -> None:
    """One entry per unit, each carrying the dates it moved on."""
    heard.BANK_VERSIONS = root / "bank-versions.json"
    heard.BANK_VERSIONS.write_text(json.dumps({"units": {
        name: {"kind": "patch", "version": len(dates),
               "history": [{"date": d, "version": i + 1} for i, d in enumerate(dates)]}
        for name, dates in units.items()
    }}), encoding="utf-8")


def _audition(root: Path, set_id: str, patch: str, kit: bool = False) -> None:
    heard.AUDITION_ROOT = root
    (root / set_id).mkdir(parents=True, exist_ok=True)
    voice = {"program": 40, "patch": patch}
    if kit:
        voice = {"program": 0, "kit": True}
    (root / set_id / "manifest.json").write_text(
        json.dumps({"voice": voice}), encoding="utf-8")


def _with_scratch(fn):
    def wrapped() -> None:
        saved = (heard.FEEDBACK_ROOT, heard.AUDITION_ROOT, heard.BANK_VERSIONS)
        try:
            fn()
        finally:
            (heard.FEEDBACK_ROOT, heard.AUDITION_ROOT, heard.BANK_VERSIONS) = saved
    wrapped.__name__ = fn.__name__
    wrapped.__doc__ = fn.__doc__
    return wrapped


@_with_scratch
def test_a_note_taken_before_the_voice_moved_is_marked() -> None:
    """The date on a note is load-bearing.

    A voice bumps its version when its values move, so a note older than that
    bump describes a render that is not in the tree. Nothing here claims the
    bump answered it -- only that it cannot be acted on without listening again.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": ["2026-08-15", "2026-09-11"]})
        _log(heard.FEEDBACK_ROOT, "p040-violin", [
            _note("2026-08-01T10:00:00+00:00", "wrong-instrument"),
            _note("2026-09-19T10:00:00+00:00", "acceptable"),
        ])
        voice = heard.collect([], "")[0]
        assert voice["last_moved"] == "2026-09-11", voice
        by_date = {n["at"][:10]: n for n in voice["notes"]}
        assert by_date["2026-08-01"]["predates_last_move"], voice
        assert not by_date["2026-09-19"]["predates_last_move"], voice


@_with_scratch
def test_the_headline_verdict_is_the_worst_one_still_standing() -> None:
    """Not the worst one ever recorded.

    `wrong-instrument` outranks `acceptable`, so a voice fixed since would go on
    being reported as a different instrument for as long as the old note sits in
    the file -- and the summary line is the only part of this anybody reads
    before deciding where to go next.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": ["2026-09-11"]})
        _log(heard.FEEDBACK_ROOT, "p040-violin", [
            _note("2026-08-01T10:00:00+00:00", "wrong-instrument"),
            _note("2026-09-19T10:00:00+00:00", "acceptable"),
        ])
        voice = heard.collect([], "")[0]
        assert voice["worst"] == "acceptable", voice
        assert not voice["worst_predates_last_move"], voice

        # With nothing said since the move there is no standing verdict, so the
        # old one is reported rather than dropped -- and marked, because the
        # two cases look identical on the line otherwise.
        _log(heard.FEEDBACK_ROOT, "p040-violin",
             [_note("2026-08-01T10:00:00+00:00", "wrong-instrument")])
        stale = heard.collect([], "")[0]
        assert stale["worst"] == "wrong-instrument", stale
        assert stale["worst_predates_last_move"], stale


@_with_scratch
def test_a_kit_note_is_about_the_drum_it_was_struck_on() -> None:
    """One part, forty-odd instruments, and the unit of work is the drum note.

    A kit has no patch unit at all -- `bank-versions` versions `d000`-`d127`
    separately -- so a verdict attributed to "the kit" is attributable to
    nothing. The page already records which strike it was and which notes that
    strike held; this is that attribution being read.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "kit000-standard-kit", "", kit=True)
        _bank(root, {"d038": ["2026-09-11"], "d042": ["2026-05-01"]})
        _log(heard.FEEDBACK_ROOT, "kit000-standard-kit", [
            # The snare has moved since this was said; the hi-hat has not.
            _note("2026-08-01T10:00:00+00:00", "wrong-instrument", hit=_hit(1, 38)),
            _note("2026-08-01T10:00:00+00:00", "acceptable", hit=_hit(2, 42)),
        ])
        voice = heard.collect([], "")[0]
        assert voice["kit"] and voice["patch"] == "", voice
        by_unit = {n["units"][0]: n for n in voice["notes"]}
        assert by_unit["d038"]["predates_last_move"], by_unit["d038"]
        assert not by_unit["d042"]["predates_last_move"], by_unit["d042"]
        # And the one still standing is what the headline reports, which on a
        # kit is the whole difference between "this kit is wrong" and "the
        # snare was, and has been touched since".
        assert voice["worst"] == "acceptable", voice
        assert "Acoustic Snare 38" in by_unit["d038"]["where"], by_unit["d038"]


@_with_scratch
def test_a_fill_is_stale_as_soon_as_any_drum_under_it_moves() -> None:
    """One strike can hold several notes, and a flam holds two of one.

    A note taken on a six-tom fill is about all six, so the newest of their
    moves is what dates it -- taking the oldest would keep reporting a fill as
    current after five of its six had been re-voiced.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "kit000-standard-kit", "", kit=True)
        _bank(root, {"d043": ["2026-01-01"], "d041": ["2026-09-11"]})
        _log(heard.FEEDBACK_ROOT, "kit000-standard-kit",
             [_note("2026-08-01T10:00:00+00:00", "acceptable", hit=_hit(1, 43, 41))])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["units"] == ["d043", "d041"], note
        assert note["last_moved"] == "2026-09-11", note
        assert note["predates_last_move"], note


@_with_scratch
def test_a_voice_nothing_can_be_dated_against_is_never_marked() -> None:
    """A page built without a tuning build names no patch.

    An unmarked note has to mean "nothing known about when this voice moved",
    never "current" -- the opposite reading would quietly promote every note on
    every unbuilt page to standing.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p007-clavi", "")
        _bank(root, {"violin": ["2026-09-11"]})
        _log(heard.FEEDBACK_ROOT, "p007-clavi",
             [_note("2026-01-01T10:00:00+00:00", "broken")])
        voice = heard.collect([], "")[0]
        assert voice["patch"] == "", voice
        assert voice["notes"][0]["units"] == [], voice
        assert not voice["notes"][0]["predates_last_move"], voice


@_with_scratch
def test_a_log_survives_a_line_that_is_not_one() -> None:
    """What a crash mid-append leaves, and what a hand-edit leaves."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": ["2026-09-11"]})
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_note("2026-09-19T10:00:00+00:00", "ok")])
        with (heard.FEEDBACK_ROOT / "p040-violin.jsonl").open("a", encoding="utf-8") as fh:
            fh.write('{"grade": "brok')
        assert len(heard.collect([], "")[0]["notes"]) == 1


@_with_scratch
def test_what_was_sounding_is_carried_through_to_the_line() -> None:
    """A note whose subject cannot be identified is worse than no note.

    The page attaches the take, the version, which strike and where in it; all
    of it has to survive to the reader, or acting on a note means guessing
    which of nine takes it was about.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": ["2026-09-11"]})
        entry = _note("2026-09-19T10:00:00+00:00", "acceptable", "tone/dark")
        entry["conditions"].update({"take_label": "Legato — a scale", "version": "gm041",
                                    "hit": _hit(3, 67), "playhead": 2.25})
        _log(heard.FEEDBACK_ROOT, "p040-violin", [entry])
        line = heard.collect([], "")[0]["notes"][0]["where"]
        for part in ("Legato — a scale", "gm041", "hit 3", "2.25s"):
            assert part in line, (part, line)

        # Blind mode withholds the version on the page, and a line that named
        # it anyway would report what the listener was not told.
        entry["conditions"].update({"blind": True, "version": None})
        _log(heard.FEEDBACK_ROOT, "p040-violin", [entry])
        blind = heard.collect([], "")[0]["notes"][0]["where"]
        assert "blind" in blind and "gm041" not in blind, blind


def _run_all() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok   {t.__name__}")
        # Any exception, not just a failed assertion: a test that reaches for a
        # key a file stopped carrying raises, and catching only AssertionError
        # ends the whole run with no line saying which test it was.
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"FAIL {t.__name__}: {type(e).__name__}: {e}"
                  if not isinstance(e, AssertionError) else f"FAIL {t.__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
