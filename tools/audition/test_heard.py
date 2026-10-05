"""Reading the listening log back.

The failure worth catching is a summary that reads well and is about a render
nobody can hear any more. A note is dated and the voice under it moves, so the
worst verdict a voice carries and the worst verdict still standing are different
facts -- and the first is the one that sends somebody to fix a voice as it was
three weeks ago.
"""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import heard


def _log(root: Path, set_id: str, entries: list[dict]) -> None:
    root.mkdir(parents=True, exist_ok=True)
    (root / f"{set_id}.jsonl").write_text(
        "".join(json.dumps(e, ensure_ascii=False) + "\n" for e in entries), encoding="utf-8"
    )


def _note(at: str, grade: str, tag: str = "", text: str = "", hit: dict | None = None) -> dict:
    return {
        "at": at,
        "grade": grade,
        "tag": tag,
        "text": text,
        "lang": "en",
        "conditions": {
            "set": "p040-violin",
            "take": "single-long",
            "take_label": "Single note",
            "version": "model",
            "playhead": 1.5,
            "hit": hit,
        },
    }


def _hit(n: int, *notes: int) -> dict:
    return {
        "n": n,
        "start": 0.3,
        "into": 0.2,
        "notes": [{"note": note, "velocity": 100} for note in notes],
    }


def _bank(
    root: Path,
    units: dict[str, dict[str, int]],
    kinds: dict[str, str] | None = None,
    generation: int | None = None,
) -> str:
    """One entry per unit, each date it moved on paired with the generation
    the bump landed at.

    Named explicitly rather than derived from position: a test that has to
    point at a specific generation needs the fixture to say which number that
    is, not compute one a reader has to re-derive to check the assertion.
    """
    heard.BANK_VERSIONS = root / "bank-versions.json"
    gens = [g for bumps in units.values() for g in bumps.values()]
    heard.BANK_VERSIONS.write_text(
        json.dumps(
            {
                "bank_generation": generation if generation is not None else max(gens, default=0),
                "units": {
                    name: {
                        "kind": (kinds or {}).get(name, "patch"),
                        "version": len(bumps),
                        "history": [
                            {"date": d, "version": i + 1, "generation": g}
                            for i, (d, g) in enumerate(sorted(bumps.items()))
                        ],
                    }
                    for name, bumps in units.items()
                },
            }
        ),
        encoding="utf-8",
    )
    return hashlib.sha256(heard.BANK_VERSIONS.read_bytes()).hexdigest()


def _audition(
    root: Path, set_id: str, patch: str, kit: bool = False, sources: dict | None = None
) -> None:
    heard.AUDITION_ROOT = root
    (root / set_id).mkdir(parents=True, exist_ok=True)
    voice = {"program": 40, "patch": patch}
    if kit:
        voice = {"program": 0, "kit": True}
    # One take, as every set the server serves has.
    manifest = {"voice": voice, "items": [{"id": "single-long", "tracks": {"model": "model.wav"}}]}
    if sources:
        manifest["sources"] = sources
    (root / set_id / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")


_SOURCES = {
    "model": {"role": "model"},
    "bow-light": {"role": "model"},
    "gm041": {"role": "reference"},
}


#: What the reference capture of every v2 page here says about its boundary.
_REFERENCE = {"id": "violin", "rig": "none", "room": "none", "rig_evidence": "verified"}


def _page(
    root: Path,
    set_id: str,
    digest: str | None,
    *,
    patch: str = "violin",
    program: int = 40,
    kit: bool = False,
    comparisons: list[dict] | None = None,
    path: bool = True,
) -> None:
    """A v2 manifest: one take, evidence per source, the page's comparisons."""
    heard.AUDITION_ROOT = root
    (root / set_id).mkdir(parents=True, exist_ok=True)
    voice = {"program": 0, "kit": True} if kit else {"program": program, "patch": patch}
    voice["capture"] = "violin"
    sources = {
        "model": {"role": "model", "scope": "product"},
        "model-di": {"role": "model", "scope": "instrument"},
        "bow-light": {"role": "model", "scope": "product"},
        "gm041": {"role": "reference"},
    }
    record = {"schema": 1, "bank_registry_digest": digest, "complete": True, "events": []}
    evidence = {
        key: {
            "request_id": f"req-{key}",
            "build_id": "build-1",
            "asset_id": f"asset-{key}",
            "path": record if path else None,
        }
        for key in sources
    }
    if comparisons is None:
        comparisons = [_comparison("gm_gs_product", "product", ["model", "bow-light"])]
    manifest = {
        "schema_version": 2,
        "voice": voice,
        "sources": sources,
        "set_generation": "gen-1",
        "comparisons": comparisons,
        "items": [{"id": "single-long", "evidence": evidence}],
    }
    (root / set_id / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")


def _comparison(cid: str, scope: str, models: list[str], status: str = "matched") -> dict:
    return {
        "id": cid,
        "scope": scope,
        "model_sources": models,
        "oracle_sources": ["gm041"],
        "status": status,
        "may_sign_off": status == "matched",
        "reasons": [],
    }


def _v2(
    at: str,
    grade: str,
    judged: str = "model",
    scope: str = "product",
    cid: str = "gm_gs_product",
    hit: dict | None = None,
) -> dict:
    """A note as the page writes it now: what was judged, and which recording."""
    entry = _note(at, grade, hit=hit)
    entry["conditions"]["version"] = judged
    entry["schema_version"] = 2
    entry["evaluation"] = {
        "comparison_id": cid,
        "scope": scope,
        "judged_source": judged,
        "oracle_source": "gm041",
        "comparison_status": "matched",
        "blind": False,
    }
    entry["evidence"] = {
        "set_generation": "gen-1",
        "take": "single-long",
        "request_id": f"req-{judged}",
        "asset_id": f"asset-{judged}",
        "build_id": "build-1",
        "completeness": "complete",
    }
    return entry


def _with_scratch(fn):
    def wrapped() -> None:
        saved = (heard.FEEDBACK_ROOT, heard.AUDITION_ROOT, heard.BANK_VERSIONS)
        stubs = ("reference_raw", "capture_of_timbre", "registry_at")
        saved_stubs = {name: getattr(heard.claims, name) for name in stubs}
        heard.claims.reference_raw = lambda capture: dict(_REFERENCE)
        heard.claims.capture_of_timbre = lambda timbre, prefer="": "violin"
        try:
            fn()
        finally:
            (heard.FEEDBACK_ROOT, heard.AUDITION_ROOT, heard.BANK_VERSIONS) = saved
            for name, value in saved_stubs.items():
                setattr(heard.claims, name, value)

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
        _bank(root, {"violin": {"2026-08-15": 1, "2026-09-11": 2}})
        _log(
            heard.FEEDBACK_ROOT,
            "p040-violin",
            [
                _note("2026-08-01T10:00:00+00:00", "wrong-instrument"),
                _note("2026-09-19T10:00:00+00:00", "acceptable"),
            ],
        )
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
        _bank(root, {"violin": {"2026-09-11": 1}})
        _log(
            heard.FEEDBACK_ROOT,
            "p040-violin",
            [
                _note("2026-08-01T10:00:00+00:00", "wrong-instrument"),
                _note("2026-09-19T10:00:00+00:00", "acceptable"),
            ],
        )
        voice = heard.collect([], "")[0]
        assert voice["worst"] == "acceptable", voice
        assert not voice["worst_predates_last_move"], voice

        # With nothing said since the move there is no standing verdict, so the
        # old one is reported rather than dropped -- and marked, because the
        # two cases look identical on the line otherwise.
        _log(
            heard.FEEDBACK_ROOT,
            "p040-violin",
            [_note("2026-08-01T10:00:00+00:00", "wrong-instrument")],
        )
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
        _bank(root, {"d038": {"2026-09-11": 2}, "d042": {"2026-05-01": 1}})
        _log(
            heard.FEEDBACK_ROOT,
            "kit000-standard-kit",
            [
                # The snare has moved since this was said; the hi-hat has not.
                _note("2026-08-01T10:00:00+00:00", "wrong-instrument", hit=_hit(1, 38)),
                _note("2026-08-01T10:00:00+00:00", "acceptable", hit=_hit(2, 42)),
            ],
        )
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
        _bank(root, {"d043": {"2026-01-01": 1}, "d041": {"2026-09-11": 2}})
        _log(
            heard.FEEDBACK_ROOT,
            "kit000-standard-kit",
            [_note("2026-08-01T10:00:00+00:00", "acceptable", hit=_hit(1, 43, 41))],
        )
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
        _bank(root, {"violin": {"2026-09-11": 1}})
        _log(heard.FEEDBACK_ROOT, "p007-clavi", [_note("2026-01-01T10:00:00+00:00", "broken")])
        voice = heard.collect([], "")[0]
        assert voice["patch"] == "", voice
        assert voice["notes"][0]["units"] == [], voice
        assert not voice["notes"][0]["predates_last_move"], voice


@_with_scratch
def test_the_versions_put_forward_to_keep_are_counted_apart_from_the_verdicts() -> None:
    """A voice carrying recorded candidates asks which of them should ship.

    A list of notes does not answer that however carefully each one is read, and
    a preference is not a verdict: the best of a set can still be short of the
    reference, which is the state a bank of candidates is usually in. Whether
    the names were visible rides with the count, because that is what decides
    what the count is worth.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p019-church-organ", "church_organ")
        _bank(root, {"church_organ": {"2026-09-11": 1}})

        def prefer(version: str, take: str, blind: bool = False) -> dict:
            entry = _note("2026-09-19T10:00:00+00:00", "", "prefer")
            entry["conditions"].update({"version": version, "take": take, "blind": blind})
            return entry

        _log(
            heard.FEEDBACK_ROOT,
            "p019-church-organ",
            [
                prefer("hall", "single-long"),
                prefer("hall", "music"),
                prefer("chiff-strong", "tongued", blind=True),
                # A verdict on the same voice, which is a different statement and
                # must not be counted as a vote for anything.
                _note("2026-09-19T11:00:00+00:00", "acceptable", "tone/dark"),
            ],
        )
        voice = heard.collect([], "")[0]
        kept = {p["version"]: p for p in voice["preferred"]}
        assert [p["version"] for p in voice["preferred"]] == ["hall", "chiff-strong"], voice
        assert kept["hall"]["n"] == 2 and kept["hall"]["sighted"] == 2, kept
        assert kept["chiff-strong"]["sighted"] == 0, kept
        assert kept["hall"]["takes"] == ["single-long", "music"], kept
        # The verdict stays a verdict: a preference carries no grade and must
        # not become the voice's headline.
        assert voice["worst"] == "acceptable", voice


@_with_scratch
def test_a_log_survives_a_line_that_is_not_one() -> None:
    """What a crash mid-append leaves, and what a hand-edit leaves."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": {"2026-09-11": 1}})
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
        _bank(root, {"violin": {"2026-09-11": 1}})
        entry = _note("2026-09-19T10:00:00+00:00", "acceptable", "tone/dark")
        entry["conditions"].update(
            {
                "take_label": "Legato — a scale",
                "version": "gm041",
                "hit": _hit(3, 67),
                "playhead": 2.25,
            }
        )
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


@_with_scratch
def test_a_clean_verdict_signs_off_with_both_provenance_numbers_resolved() -> None:
    """The common case: one current `ok` on `model`, its recording's ids carried."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"violin": {"2026-08-15": 1, "2026-09-11": 2}})
        _page(root / "audition", "p040-violin", digest)
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_v2("2026-09-19T10:00:00+00:00", "ok")])
        blocks = heard.signoff("p040-violin")
        assert set(blocks) == {"music"}, blocks
        block = blocks["music"]
        assert block["provenance"] == {
            "date": "2026-09-19",
            "bank_generation": 2,
            "patch_version": 2,
        }, block
        assert block["take"] == "single-long", block
        ev = block["evidence"]
        ids = (ev["comparison_id"], ev["request_id"], ev["asset_id"], ev["build_id"])
        assert ids == ("gm_gs_product", "req-model", "asset-model", "build-1"), ev
        assert ev["bank_registry_digest"] == digest and ev["units"] == {"violin": 2}, ev
        assert ev["boundary"]["status"] == "matched", ev
        assert ev["boundary"]["page_status"] == "matched", ev


@_with_scratch
def test_acceptable_signs_off_the_same_way_as_ok() -> None:
    """`acceptable` means the instrument too, and is not a lesser case."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"violin": {"2026-09-11": 1}})
        _page(root / "audition", "p040-violin", digest)
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_v2("2026-09-19T10:00:00+00:00", "acceptable")])
        assert heard.signoff("p040-violin")["music"]["take"] == "single-long"


@_with_scratch
def test_no_notes_at_all_refuses() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": {"2026-09-11": 1}})
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "no notes" in str(e), e


@_with_scratch
def test_wrong_instrument_or_broken_refuses_naming_the_verdict_and_its_date() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": {"2026-09-11": 1}})
        _log(
            heard.FEEDBACK_ROOT,
            "p040-violin",
            [_note("2026-09-19T10:00:00+00:00", "wrong-instrument")],
        )
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "wrong-instrument" in str(e) and "2026-09-19" in str(e), e


@_with_scratch
def test_a_preference_tag_alone_refuses() -> None:
    """A `prefer` note carries no grade and is not a verdict."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": {"2026-09-11": 1}})
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_note("2026-09-19T10:00:00+00:00", "", "prefer")])
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "preference" in str(e), e


@_with_scratch
def test_a_note_predating_the_last_bump_refuses() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin")
        _bank(root, {"violin": {"2026-08-15": 1, "2026-09-11": 2}})
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_note("2026-08-01T10:00:00+00:00", "ok")])
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "2026-08-01" in str(e) and "2026-09-11" in str(e), e


@_with_scratch
def test_a_kit_note_resolves_its_version_from_its_drum_unit() -> None:
    """A kit has no patch unit -- the version comes from the note it was struck on."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"d038": {"2026-09-11": 1}})
        _page(root / "audition", "kit000-standard-kit", digest, kit=True)
        entry = _v2("2026-09-19T10:00:00+00:00", "ok", hit=_hit(1, 38))
        _log(heard.FEEDBACK_ROOT, "kit000-standard-kit", [entry])
        block = heard.signoff("kit000-standard-kit")["music"]
        assert block["provenance"]["bank_generation"] == 1, block
        assert block["provenance"]["patch_version"] == 1, block
        assert block["evidence"]["units"] == {"d038": 1}, block


@_with_scratch
def test_bank_generation_is_the_recordings_registry_not_todays() -> None:
    """A unit unrelated to this voice bumping later must not inflate the record.

    The record's `bank_generation` is a watermark against which a later shared
    move reads as `unverified`; stamping today's generation would set it too
    high. The recording's own registry, found by its digest, supplies it.
    """
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        then_digest = _bank(root, {"violin": {"2026-08-15": 40}})
        then = json.loads(heard.BANK_VERSIONS.read_text())
        _bank(root, {"violin": {"2026-08-15": 40}, "reed": {"2026-09-20": 70}})
        heard.claims.registry_at = lambda d, path: then if d == then_digest else None
        _page(root / "audition", "p040-violin", then_digest)
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_v2("2026-09-19T10:00:00+00:00", "ok")])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["freshness"] == "current", note
        block = heard.signoff("p040-violin")["music"]
        assert block["provenance"]["bank_generation"] == 40, block


@_with_scratch
def test_a_note_written_with_the_reference_sounding_is_about_the_model_it_names() -> None:
    """The page records what was selected; a listener often writes with the
    reference still up. `against` names the library version the note is about."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin", sources=_SOURCES)
        _bank(root, {"violin": {"2026-09-11": 1}})
        entry = _note("2026-09-19T10:00:00+00:00", "acceptable")
        entry["conditions"].update({"version": "gm041", "against": "model"})
        _log(heard.FEEDBACK_ROOT, "p040-violin", [entry])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["judged"] == "model", note
        assert "gm041 sounding, about model" in note["where"], note["where"]
        # A v1 note says nothing about the recording, so it does not sign off.
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "unverified" in str(e) and "v1 note" in str(e), e


@_with_scratch
def test_a_note_taken_on_a_comparison_is_about_neither_the_model_nor_the_reference() -> None:
    """A comparison is a capture offered beside the reference, not a candidate
    and not the target `against` resolves to -- a note taken while it sounds
    must not be counted as being about either."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        sources = dict(_SOURCES, **{"kit-a": {"role": "comparison"}})
        _audition(root / "audition", "p040-violin", "violin", sources=sources)
        _bank(root, {"violin": {"2026-09-11": 1}})
        entry = _note("2026-09-19T10:00:00+00:00", "ok")
        entry["conditions"]["version"] = "kit-a"
        _log(heard.FEEDBACK_ROOT, "p040-violin", [entry])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["judged"] == "kit-a", note
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "kit-a" in str(e), e


@_with_scratch
def test_compared_against_is_surfaced_and_flagged() -> None:
    """Judged against the policy reference reads clean; a comparison is a real
    answer and flagged as one; nothing recorded and no field at all are both
    "unknown" -- neither is guessed into the other."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        sources = dict(_SOURCES, **{"kit-a": {"role": "comparison"}})
        _audition(root / "audition", "p040-violin", "violin", sources=sources)
        _bank(root, {"violin": {"2026-09-11": 1}})

        against_reference = _note("2026-09-19T10:00:00+00:00", "ok")
        against_reference["conditions"]["compared_against"] = {
            "role": "reference",
            "version": "gm041",
            "label": "gm041",
        }
        against_comparison = _note("2026-09-19T10:01:00+00:00", "ok")
        against_comparison["conditions"]["compared_against"] = {
            "role": "comparison",
            "version": "kit-a",
            "label": "kit-a",
        }
        nothing_played = _note("2026-09-19T10:02:00+00:00", "ok")
        nothing_played["conditions"]["compared_against"] = None
        legacy = _note("2026-09-19T10:03:00+00:00", "ok")  # predates the field entirely

        _log(
            heard.FEEDBACK_ROOT,
            "p040-violin",
            [against_reference, against_comparison, nothing_played, legacy],
        )
        notes = {n["at"]: n for n in heard.collect([], "")[0]["notes"]}
        assert notes["2026-09-19T10:00:00+00:00"]["oracle_flag"] == "", notes
        assert notes["2026-09-19T10:00:00+00:00"]["compared_against"]["version"] == "gm041"
        assert notes["2026-09-19T10:01:00+00:00"]["oracle_flag"] == "comparison", notes
        assert notes["2026-09-19T10:02:00+00:00"]["oracle_flag"] == "unknown", notes
        assert notes["2026-09-19T10:02:00+00:00"]["compared_against"] is None
        assert notes["2026-09-19T10:03:00+00:00"]["oracle_flag"] == "unknown", notes


@_with_scratch
def test_a_reference_sounding_note_with_no_subject_recorded_does_not_sign_off() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin", sources=_SOURCES)
        _bank(root, {"violin": {"2026-09-11": 1}})
        entry = _note("2026-09-19T10:00:00+00:00", "ok")
        entry["conditions"]["version"] = "gm041"
        _log(heard.FEEDBACK_ROOT, "p040-violin", [entry])
        note = heard.collect([], "")[0]["notes"][0]
        assert "model version not recorded" in note["where"], note["where"]
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "unrecorded" in str(e), e


@_with_scratch
def test_a_verdict_on_a_candidate_is_not_a_signoff_of_the_shipped_voice() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _audition(root / "audition", "p040-violin", "violin", sources=_SOURCES)
        _bank(root, {"violin": {"2026-09-11": 1}})
        entry = _note("2026-09-19T10:00:00+00:00", "ok")
        entry["conditions"]["version"] = "bow-light"
        _log(heard.FEEDBACK_ROOT, "p040-violin", [entry])
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "bow-light" in str(e), e


@_with_scratch
def test_a_direct_path_verdict_is_not_a_signoff_of_the_shipped_voice() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        sources = dict(_SOURCES, **{"model-di": {"role": "model"}})
        _audition(root / "audition", "p040-violin", "violin", sources=sources)
        _bank(root, {"violin": {"2026-09-11": 1}})
        entry = _note("2026-09-19T10:00:00+00:00", "ok")
        entry["conditions"]["version"] = "model-di"
        _log(heard.FEEDBACK_ROOT, "p040-violin", [entry])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["judged"] == "model-di", note
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "model-di" in str(e) and "direct-path" in str(e), e


@_with_scratch
def test_a_signoff_keeps_the_judged_source_and_its_comparison_counterpart() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"violin": {"2026-09-11": 1}})
        _page(root / "audition", "p040-violin", digest)
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_v2("2026-09-19T10:00:00+00:00", "ok")])
        ev = heard.signoff("p040-violin")["music"]["evidence"]
        assert ev["judged"] == "model", ev
        assert ev["counterpart"] == {"source": "gm041", "capture": "violin"}, ev
        assert ev["set"] == "p040-violin" and ev["set_generation"] == "gen-1", ev


@_with_scratch
def test_a_v1_note_is_unverified_and_never_signs_off() -> None:
    """Posted today about a WAV nobody can identify: no version is back-filled."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"violin": {"2026-09-11": 1}})
        _page(root / "audition", "p040-violin", digest)
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_note("2026-10-05T10:00:00+00:00", "ok")])
        voice = heard.collect([], "")[0]
        assert voice["notes"][0]["freshness"] == "unverified", voice
        assert voice["worst"] == "ok" and voice["worst_freshness"] == "unverified", voice
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "v1 note" in str(e), e


@_with_scratch
def test_the_voices_own_unit_moving_after_the_recording_makes_it_stale() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        then_digest = _bank(root, {"violin": {"2026-09-11": 1}})
        then = json.loads(heard.BANK_VERSIONS.read_text())
        # Posted after the bump, about a recording made before it.
        _bank(root, {"violin": {"2026-09-11": 1, "2026-09-20": 2}})
        heard.claims.registry_at = lambda d, path: then if d == then_digest else None
        _page(root / "audition", "p040-violin", then_digest)
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_v2("2026-09-25T10:00:00+00:00", "ok")])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["freshness"] == "stale", note
        assert "violin moved from v1 to v2" in note["freshness_reasons"][0], note
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "stale" in str(e), e


@_with_scratch
def test_a_shared_unit_moving_alone_leaves_the_note_unverified() -> None:
    """The existing policy: a shared move cannot be attributed, so it is not `stale`."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        kinds = {"bowed_string_voice": "shared"}
        units = {"violin": {"2026-09-11": 10}, "bowed_string_voice": {"2026-09-01": 5}}
        then_digest = _bank(root, units, kinds)
        then = json.loads(heard.BANK_VERSIONS.read_text())
        _bank(root, dict(units, bowed_string_voice={"2026-09-01": 5, "2026-09-20": 12}), kinds)
        heard.claims.registry_at = lambda d, path: then if d == then_digest else None
        _page(root / "audition", "p040-violin", then_digest)
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_v2("2026-09-25T10:00:00+00:00", "ok")])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["freshness"] == "unverified", note
        assert "shared unit moved at generation 12" in note["freshness_reasons"][0], note


@_with_scratch
def test_an_old_library_without_a_bank_digest_is_unverified() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        _bank(root, {"violin": {"2026-09-11": 1}})
        _page(root / "audition", "p040-violin", None)
        _log(heard.FEEDBACK_ROOT, "p040-violin", [_v2("2026-09-25T10:00:00+00:00", "ok")])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["freshness"] == "unverified", note
        assert "no bank registry digest" in note["freshness_reasons"][0], note
        # And a library that wrote no path record at all.
        _page(root / "audition", "p040-violin", None, path=False)
        note = heard.collect([], "")[0]["notes"][0]
        assert note["freshness"] == "unverified", note
        assert "not a tuning build" in note["freshness_reasons"][0], note


@_with_scratch
def test_a_re_rendered_recording_is_stale() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"violin": {"2026-09-11": 1}})
        _page(root / "audition", "p040-violin", digest)
        entry = _v2("2026-09-25T10:00:00+00:00", "ok")
        entry["evidence"]["asset_id"] = "asset-of-an-older-render"
        _log(heard.FEEDBACK_ROOT, "p040-violin", [entry])
        note = heard.collect([], "")[0]["notes"][0]
        assert note["freshness"] == "stale", note


@_with_scratch
def test_a_di_ok_is_an_instrument_claim_and_never_the_music_block() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"electric_guitar": {"2026-09-11": 1}})
        comparisons = [
            _comparison("instrument_di", "instrument", ["model-di"]),
            _comparison("gm_gs_product", "product", ["model"], status="context_only"),
        ]
        _page(
            root / "audition",
            "p027-electric-guitar-clean",
            digest,
            patch="electric_guitar",
            program=27,
            comparisons=comparisons,
        )
        entry = _v2("2026-09-25T10:00:00+00:00", "ok", "model-di", "instrument", "instrument_di")
        _log(heard.FEEDBACK_ROOT, "p027-electric-guitar-clean", [entry])
        blocks = heard.signoff("p027-electric-guitar-clean")
        assert set(blocks) == {"instrument"}, blocks
        assert blocks["instrument"]["rig_evidence"] == "verified", blocks
        assert blocks["instrument"]["evidence"]["scope"] == "instrument", blocks


@_with_scratch
def test_a_di_verdict_never_overrides_the_product_headline() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"electric_guitar": {"2026-09-11": 1}})
        comparisons = [
            _comparison("instrument_di", "instrument", ["model-di"]),
            _comparison("gm_gs_product", "product", ["model"]),
        ]
        _page(
            root / "audition",
            "p027-electric-guitar-clean",
            digest,
            patch="electric_guitar",
            program=27,
            comparisons=comparisons,
        )
        di = _v2("2026-09-25T10:00:00+00:00", "broken", "model-di", "instrument", "instrument_di")
        product = _v2("2026-09-25T10:01:00+00:00", "acceptable")
        _log(heard.FEEDBACK_ROOT, "p027-electric-guitar-clean", [di, product])
        voice = heard.collect([], "")[0]
        assert voice["worst"] == "acceptable", voice
        by_id = {p["comparison_id"]: p for p in voice["populations"]}
        assert by_id["instrument_di"]["worst"] == "broken", by_id
        assert by_id["gm_gs_product"]["worst"] == "acceptable", by_id


@_with_scratch
def test_a_candidate_with_overrides_is_never_the_default_signoff() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.FEEDBACK_ROOT = root / "feedback"
        digest = _bank(root, {"violin": {"2026-09-11": 1}})
        _page(root / "audition", "p040-violin", digest)
        _log(
            heard.FEEDBACK_ROOT,
            "p040-violin",
            [_v2("2026-09-25T10:00:00+00:00", "ok", judged="bow-light")],
        )
        voice = heard.collect([], "")[0]
        assert voice["notes"][0]["kind"] == "exploratory", voice
        assert voice["worst"] == "" and voice["exploratory"] == 1, voice
        try:
            heard.signoff("p040-violin")
            raise AssertionError("expected a refusal")
        except ValueError as e:
            assert "bow-light is a candidate" in str(e), e


def _v1_page_with_v2_note(root: Path, at: str) -> dict:
    """A v1 page (no comparisons) and a v2 note posted on it; its digest note."""
    heard.FEEDBACK_ROOT = root / "feedback"
    _bank(root, {"violin": {"2026-09-20": 2}})
    _audition(root / "audition", "p040-violin", "violin", sources=_SOURCES)
    note = _v2(at, "ok", cid="", scope="")
    note["evaluation"].update(comparison_id=None, scope=None, comparison_status=None)
    _log(heard.FEEDBACK_ROOT, "p040-violin", [note])
    return heard.collect([], "")[0]["notes"][0]


@_with_scratch
def test_a_v2_note_on_a_page_without_comparisons_is_still_a_verdict() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        note = _v1_page_with_v2_note(Path(tmp), "2026-09-25T10:00:00+00:00")
        assert note["scope"] == "product" and note["kind"] == "verdict", note
        assert note["freshness"] == "unverified", note


@_with_scratch
def test_a_v2_note_on_a_v1_page_gets_the_date_based_stale_check() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        note = _v1_page_with_v2_note(Path(tmp), "2026-09-10T10:00:00+00:00")
        assert note["freshness"] == "stale", note
        assert "last moved on 2026-09-20" in note["freshness_reasons"][0], note


@_with_scratch
def test_a_set_is_found_by_the_id_the_server_gives_it() -> None:
    """A leaf named `audition` takes its parent's name; a duplicate name gets `-2`."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        heard.AUDITION_ROOT = root / "audition"
        for rel, program in (("audition/dup", 1), ("dup/audition", 2), ("harp/audition", 3)):
            (root / rel).mkdir(parents=True)
            (root / rel / "manifest.json").write_text(
                json.dumps({"voice": {"program": program}, "items": [{"id": "t"}]}),
                encoding="utf-8",
            )
        assert heard.voice_of("harp") == {"program": 3}
        assert heard.voice_of("dup") == {"program": 1}
        assert heard.voice_of("dup-2") == {"program": 2}
        assert heard.manifest_of("audition") == {}


def _blind_or_preference(root: Path, note: dict) -> dict:
    heard.FEEDBACK_ROOT = root / "feedback"
    digest = _bank(root, {"violin": {"2026-09-11": 1}})
    _page(root / "audition", "p040-violin", digest)
    _log(heard.FEEDBACK_ROOT, "p040-violin", [note])
    return heard.collect([], "")[0]["notes"][0]


@_with_scratch
def test_a_blind_run_is_unverified_not_stale() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        blind = _v2("2026-09-25T10:00:00+00:00", "")
        blind["evaluation"].update(judged_source=None, oracle_source=None, blind=True)
        note = _blind_or_preference(Path(tmp), blind)
        assert note["freshness"] == "unverified", note
        assert "blind" in note["freshness_reasons"][0], note


@_with_scratch
def test_a_preference_for_a_reference_key_is_not_stale_for_being_one() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        pref = _v2("2026-09-25T10:00:00+00:00", "", judged="gm041")
        pref.update(tag="prefer")
        note = _blind_or_preference(Path(tmp), pref)
        assert note["freshness"] != "stale", note


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
            print(
                f"FAIL {t.__name__}: {type(e).__name__}: {e}"
                if not isinstance(e, AssertionError)
                else f"FAIL {t.__name__}: {e}"
            )
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(_run_all())
