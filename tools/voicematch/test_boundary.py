"""Tests for the comparison boundary.

The decision table is checked as invariants over every combination of its
inputs rather than by example, because the defect it exists to stop is a row
nobody thought to write down: a rigged or unanswered reference reaching a fit,
or a direct comparison reaching a product sign-off.

    python -m pytest tools/voicematch/test_boundary.py
"""

from __future__ import annotations

import dataclasses
import itertools
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from boundary import (
    BOUNDARY_INSTRUMENT,
    BOUNDARY_RIGGED,
    BOUNDARY_UNKNOWN,
    EVIDENCE_NOT_APPLICABLE,
    EVIDENCE_UNKNOWN,
    EVIDENCE_VALUES,
    SCOPE_INSTRUMENT,
    SCOPE_PRODUCT,
    SENDS_DRY,
    SENDS_POWER_ON,
    STATUS_CONTEXT_ONLY,
    STATUS_MATCHED,
    STATUS_UNAVAILABLE,
    STATUS_UNVERIFIED,
    Reference,
    RenderRequest,
    assess,
    canonical_digest,
    rig_evidence,
)
from capture import (
    RIG_BAKED,
    RIG_NONE,
    RIG_UNCLASSIFIED,
    RIG_VALUES,
    ROOM_NONE,
    ROOM_PRESENT,
    ROOM_VALUES,
    load_config,
    rig_capable,
)

CAPTURE_DIR = Path(__file__).resolve().parent / "capture"
MISSING = object()


def test_request_normalizes_boolean_flags_before_fingerprinting():
    for field in ("rig", "allow_rigged_oracle"):
        numeric = RenderRequest(program=27, seconds=1, **{field: 1})
        boolean = RenderRequest(program=27, seconds=1, **{field: True})
        assert numeric.fingerprint() == boolean.fingerprint()


#: One program on each side of `rig_capable`.
CAPABLE_PROGRAM, PLAIN_PROGRAM = 27, 0
SENDS_OVERRIDE = (40, 0, 0)


def _reference(rig, evidence, room) -> Reference:
    raw = {"id": "case", "room": room}
    if rig is not MISSING:
        raw["rig"] = rig
    if evidence is not MISSING:
        raw["rig_evidence"] = evidence
    return Reference.from_capture(raw)


ROWS = list(
    itertools.product(
        (*RIG_VALUES, MISSING),
        (*EVIDENCE_VALUES, MISSING),
        ROOM_VALUES,
        (SCOPE_INSTRUMENT, SCOPE_PRODUCT),
        (False, True),  # tuning overrides
        (SENDS_POWER_ON, SENDS_DRY, SENDS_OVERRIDE),
        (True, False),  # rig_capable
        (False, True),  # allow_rigged_oracle
        (True, False),  # request keeps the bank rig
        (True, False, None),  # product path carries a rig
    )
)


def _assess(row):
    rig, evidence, room, scope, tuned, sends, capable, allow, keep_rig, product_rig = row
    request = RenderRequest(
        program=CAPABLE_PROGRAM if capable else PLAIN_PROGRAM,
        seconds=2.0,
        smf=b"MThd",
        rig=keep_rig,
        overrides="clean_guitar.brightness=0.4" if tuned else "",
        sends=sends,
        allow_rigged_oracle=allow,
    )
    return assess(_reference(rig, evidence, room), scope, request, product_rig=product_rig)


@pytest.fixture(scope="module")
def table():
    return [(row, _assess(row)) for row in ROWS]


def test_the_grid_is_the_full_product(table):
    assert len(table) == 4 * 5 * 3 * 2 * 2 * 3 * 2 * 2 * 2 * 3
    assert CAPABLE_PROGRAM != PLAIN_PROGRAM
    assert rig_capable(CAPABLE_PROGRAM) and not rig_capable(PLAIN_PROGRAM)


def test_every_row_reaches_every_status_it_should(table):
    seen = {a.status for _, a in table}
    assert seen == {STATUS_MATCHED, STATUS_CONTEXT_ONLY, STATUS_UNVERIFIED}
    assert any(a.may_fit for _, a in table) and any(a.may_sign_off for _, a in table)


def test_a_rigged_or_unanswered_reference_never_fits_the_instrument(table):
    for row, a in table:
        rig, _, _, scope, _, _, capable, allow, *_ = row
        if capable and scope == SCOPE_INSTRUMENT and rig != RIG_NONE and not allow:
            assert not a.may_fit, row
        if rig == RIG_BAKED and not allow:
            assert not a.may_fit, row


def test_unknown_evidence_never_fits_or_signs_off_where_a_rig_is_possible(table):
    for row, a in table:
        _, evidence, _, _, _, _, capable, allow, *_ = row
        if capable and evidence in (EVIDENCE_UNKNOWN, MISSING):
            assert not a.may_sign_off, row
            assert a.may_fit == (allow and row[3] == SCOPE_INSTRUMENT), row


def test_allow_rigged_oracle_is_unverified_and_never_signs_off_or_adopts(table):
    for row, a in table:
        allow = row[7]
        assert (a.status == STATUS_UNVERIFIED) == allow, row
        if allow:
            assert not a.may_sign_off and not a.may_adopt, row
            assert a.may_fit == (row[3] == SCOPE_INSTRUMENT), row


def test_only_an_instrument_scope_fits(table):
    for row, a in table:
        if a.may_fit:
            assert row[3] == SCOPE_INSTRUMENT, row


def test_a_sign_off_comes_from_a_matched_untouched_request_of_its_own_scope(table):
    for row, a in table:
        _, _, _, scope, tuned, sends, capable, allow, keep_rig, _ = row
        if not a.may_sign_off:
            continue
        assert a.status == STATUS_MATCHED and not allow and not tuned, row
        assert sends != SENDS_OVERRIDE, row
        if scope == SCOPE_PRODUCT:
            # A direct render never signs off the product.
            assert keep_rig, row
        else:
            assert sends == SENDS_DRY, row
            assert not (capable and keep_rig), row


def test_overrides_never_sign_off(table):
    for row, a in table:
        tuned, sends = row[4], row[5]
        if tuned or sends == SENDS_OVERRIDE:
            assert not a.may_sign_off, row


def test_product_sends_follow_the_reference_room(table):
    for row, a in table:
        _, _, room, scope, _, sends, *_ = row
        if scope != SCOPE_PRODUCT or a.status != STATUS_MATCHED:
            continue
        assert sends == (SENDS_POWER_ON if room == ROOM_PRESENT else SENDS_DRY), row


def test_a_direct_reference_against_a_rigged_product_is_context(table):
    for row, a in table:
        rig, _, _, scope, _, _, capable, allow, _, product_rig = row
        if capable and scope == SCOPE_PRODUCT and rig == RIG_NONE and product_rig is not False:
            assert a.status == (STATUS_UNVERIFIED if allow else STATUS_CONTEXT_ONLY), row
            assert not a.may_sign_off, row


def test_a_family_without_a_rig_owes_no_evidence(table):
    by_rest: dict[tuple, set] = {}
    for row, a in table:
        if row[6]:
            continue
        assert a.evidence == EVIDENCE_NOT_APPLICABLE, row
        rest = row[:1] + row[2:]
        by_rest.setdefault(rest, set()).add((a.status, a.may_fit, a.may_sign_off, a.boundary))
    assert by_rest and all(len(outcomes) == 1 for outcomes in by_rest.values())


def test_a_family_without_a_rig_keeps_the_corpus_rule(table):
    # `corpus.check_rig`: baked refuses, anything else fits.
    for row, a in table:
        rig, _, _, scope, _, sends, capable, allow, *_ = row
        if capable or allow or scope != SCOPE_INSTRUMENT or sends != SENDS_DRY:
            continue
        assert a.may_fit == (rig != RIG_BAKED), row


def test_no_reference_is_unavailable():
    for program, scope, allow in itertools.product(
        (CAPABLE_PROGRAM, PLAIN_PROGRAM), (SCOPE_INSTRUMENT, SCOPE_PRODUCT), (False, True)
    ):
        a = assess(
            None, scope, RenderRequest(program=program, seconds=1.0, allow_rigged_oracle=allow)
        )
        assert a.status == STATUS_UNAVAILABLE
        assert not a.may_fit and not a.may_sign_off
        assert "musical judgement" in a.reason


def test_unknown_scope_is_refused():
    with pytest.raises(ValueError):
        assess(None, "both", RenderRequest(program=0, seconds=1.0))


# ---------------------------------------------------------------- real captures


def _capture(name: str) -> Reference:
    return Reference.from_capture(load_config(CAPTURE_DIR / f"{name}.json"))


def _di(program: int) -> RenderRequest:
    return RenderRequest(program=program, seconds=4.0, rig=False, sends=SENDS_DRY)


def _product(program: int, sends=SENDS_DRY) -> RenderRequest:
    return RenderRequest(program=program, seconds=4.0, rig=True, sends=sends)


def test_electric_guitar_di_is_eligible_for_instrument_di():
    ref = _capture("electric_guitar_di")
    assert ref.rig_evidence == "inferred"
    a = assess(ref, SCOPE_INSTRUMENT, _di(27))
    assert (a.boundary, a.status, a.may_fit, a.may_sign_off) == (
        BOUNDARY_INSTRUMENT,
        STATUS_MATCHED,
        True,
        True,
    )
    assert a.may_adopt
    # The amplified model against the direct reference is not that comparison.
    rigged = assess(ref, SCOPE_INSTRUMENT, _product(27))
    assert rigged.status == STATUS_CONTEXT_ONLY and not rigged.may_fit
    # Program 27 binds a rig, so its product path is not ranked against a DI.
    product = assess(ref, SCOPE_PRODUCT, _product(27), product_rig=True)
    assert product.status == STATUS_CONTEXT_ONLY and not product.may_sign_off


def test_bass_fingered_is_verified():
    ref = _capture("bass_fingered")
    assert ref.rig_evidence == "verified"
    a = assess(ref, SCOPE_INSTRUMENT, _di(33))
    assert a.status == STATUS_MATCHED and a.may_fit and a.may_sign_off
    # No bank rig on a bass: the product path is comparable when the rooms agree.
    product = assess(ref, SCOPE_PRODUCT, _product(33), product_rig=False)
    assert product.status == STATUS_MATCHED and product.may_sign_off


def test_overdriven_guitar_is_product_only():
    ref = _capture("overdriven_guitar")
    assert ref.rig == RIG_BAKED
    fit = assess(ref, SCOPE_INSTRUMENT, _di(29))
    assert fit.boundary == BOUNDARY_RIGGED and fit.status == STATUS_CONTEXT_ONLY
    assert not fit.may_fit and not fit.may_sign_off
    product = assess(ref, SCOPE_PRODUCT, _product(29))
    assert product.status == STATUS_MATCHED and product.may_sign_off and not product.may_fit
    forced = assess(ref, SCOPE_INSTRUMENT, dataclasses.replace(_di(29), allow_rigged_oracle=True))
    assert forced.status == STATUS_UNVERIFIED and forced.may_fit and not forced.may_adopt


def test_jazz_guitar_is_context_only():
    ref = _capture("jazz_guitar")
    assert ref.rig == RIG_UNCLASSIFIED and ref.rig_evidence == EVIDENCE_UNKNOWN
    for scope, request in ((SCOPE_INSTRUMENT, _di(26)), (SCOPE_PRODUCT, _product(26))):
        a = assess(ref, scope, request, product_rig=True)
        assert a.boundary == BOUNDARY_UNKNOWN and a.status == STATUS_CONTEXT_ONLY
        assert not a.may_fit and not a.may_sign_off


def test_every_capture_evidence_is_a_known_value_and_only_on_an_answered_rig():
    for path in sorted(CAPTURE_DIR.glob("*.json")):
        if path.name.endswith(".local.json"):
            continue
        raw = json.loads(path.read_text())
        if "rig_evidence" not in raw:
            continue
        assert raw["rig_evidence"] in EVIDENCE_VALUES, path.name
        assert raw.get("rig") in (RIG_NONE, RIG_BAKED), path.name
        assert rig_capable(raw.get("program", 0)), path.name


def test_absent_or_unrecognised_answers_read_as_unanswered():
    assert rig_evidence({}) == EVIDENCE_UNKNOWN
    assert rig_evidence({"rig_evidence": "measured"}) == EVIDENCE_UNKNOWN
    ref = Reference.from_capture({"rig": "amp", "room": "hall"})
    assert (ref.rig, ref.room) == (RIG_UNCLASSIFIED, "unclassified")
    assert Reference.from_capture({"room": ROOM_NONE}).room == ROOM_NONE


# ---------------------------------------------------------------- fingerprint


def _request(**kw) -> RenderRequest:
    base = {
        "program": 27,
        "seconds": 4.0,
        "smf": b"MThd\x00\x00\x00\x06",
        "bank": 0,
        "channel": 0,
        "preset": "",
        "rig": False,
        "overrides": "",
        "sends": SENDS_DRY,
        "allow_rigged_oracle": False,
        "sample_rate": 44100,
        "capture": "electric_guitar_di",
        "timbre": "di",
        "key_map": ((40, 40), (41, 41)),
        "keyswitch": 0,
    }
    base.update(kw)
    return RenderRequest(**base)


def test_canonical_digest_ignores_dict_order():
    a = {"b": 1, "a": [1.5, None, "x"], "c": {"y": True, "x": b"\x01"}}
    b = {"c": {"x": b"\x01", "y": True}, "a": [1.5, None, "x"], "b": 1}
    assert canonical_digest(a) == canonical_digest(b)
    assert canonical_digest({"a": 1}) != canonical_digest({"a": 1.0})


def test_canonical_digest_refuses_what_it_cannot_place():
    with pytest.raises(TypeError):
        canonical_digest({"a": {1, 2}})
    with pytest.raises(TypeError):
        canonical_digest({1: "a"})
    with pytest.raises(ValueError):
        canonical_digest({"a": float("nan")})


def test_fingerprint_is_stable_across_input_types():
    a = _request()
    b = _request(seconds=4, key_map=[[41, 41], [40, 40]], sends=[0, 0, 0], smf=bytearray(a.smf))
    assert a == b and a.fingerprint() == b.fingerprint()
    assert len(a.fingerprint()) == 64


#: One change per field; the coverage test holds this to the dataclass.
CHANGES = [
    {"program": 28},
    {"seconds": 4.5},
    {"smf": b"MThd\x00\x00\x00\x07"},
    {"bank": 8},
    {"channel": 9},
    {"preset": "clean_guitar"},
    {"rig": True},
    {"overrides": "clean_guitar.brightness=0.4"},
    {"sends": SENDS_POWER_ON},
    {"allow_rigged_oracle": True},
    {"sample_rate": 48000},
    {"capture": "overdriven_guitar"},
    {"timbre": "other"},
    {"key_map": ((40, 52),)},
    {"keyswitch": 24},
]


@pytest.mark.parametrize(
    "change",
    CHANGES,
    ids=lambda d: next(iter(d)),
)
def test_fingerprint_moves_with_every_field(change):
    assert _request(**change).fingerprint() != _request().fingerprint()


def test_every_field_is_covered_by_the_fingerprint_cases():
    changed = {next(iter(c)) for c in CHANGES}
    assert changed == {f.name for f in dataclasses.fields(RenderRequest)}


def test_smf_bytes_reach_the_digest_as_their_hash():
    import hashlib

    smf = b"MThd" + bytes(range(64))
    digest = hashlib.sha256(smf).hexdigest()
    assert canonical_digest({"smf": smf}) == canonical_digest({"smf": digest})
    assert _request(smf=smf).frames == round(4.0 * 44100)
