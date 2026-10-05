"""The last two claims: that they load, name real voices, and expire correctly."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bank
import signoff


def _write(tmp_path: Path, payload: dict) -> Path:
    path = tmp_path / "signoff.json"
    path.write_text(json.dumps(payload))
    return path


# --------------------------------------------------------------------------- #
# The shipped file
# --------------------------------------------------------------------------- #


def test_the_shipped_file_loads():
    assert isinstance(signoff.load(), dict)


def test_every_recorded_voice_is_a_voice_the_bank_has():
    """A typo is silent in the wrong direction.

    The voice it was meant for goes on reporting its claims as unrecorded,
    which is the state the file exists to leave behind.
    """
    table = signoff.load()
    known = {v.slug for v in bank.voices(kits=sorted(bank.KIT_NAMES))}
    assert signoff.unknown_voices(table, known) == []


def test_every_recorded_provenance_names_a_generation_the_registry_has():
    """A claim dated against a bank generation that never existed dates nothing."""
    generation, units = signoff.bank_versions(
        Path(__file__).resolve().parents[2] / "tools" / "bank-versions.json"
    )
    for slug, record in signoff.load().items():
        for claim in (record.structure, record.music):
            if claim is None:
                continue
            assert claim.provenance.bank_generation, f"{slug}: no bank generation recorded"
            assert claim.provenance.bank_generation <= generation, (
                f"{slug}: recorded against generation {claim.provenance.bank_generation}, "
                f"and the registry is only on {generation}"
            )
            if claim.provenance.patch_version:
                assert claim.provenance.patch_version <= max(units.values() or [0])


# --------------------------------------------------------------------------- #
# An unreachable term is accepted with a reason, or it is open
# --------------------------------------------------------------------------- #


def test_an_accepted_term_needs_a_reason(tmp_path):
    path = _write(
        tmp_path, {"v": {"structure": {"unreachable": ["tail"], "accepted": {"tail": "  "}}}}
    )
    with pytest.raises(ValueError, match="carry no reason"):
        signoff.load(path)


def test_a_term_the_diagnosis_never_reported_cannot_be_accepted(tmp_path):
    """Otherwise an accepted list drifts into an argument about a term nobody measured."""
    path = _write(
        tmp_path,
        {"v": {"structure": {"unreachable": ["tail"], "accepted": {"harm": "not this one"}}}},
    )
    with pytest.raises(ValueError, match="not in `unreachable`"):
        signoff.load(path)


def test_an_unreachable_term_with_no_reason_is_open(tmp_path):
    path = _write(
        tmp_path,
        {
            "v": {
                "structure": {
                    "unreachable": ["tail", "harm"],
                    "accepted": {"harm": "the reference's room"},
                }
            }
        },
    )
    assert signoff.load(path)["v"].structure.open_terms == ["tail"]


# --------------------------------------------------------------------------- #
# Expiry
# --------------------------------------------------------------------------- #


def test_a_claim_taken_against_this_bank_is_current():
    p = signoff.Provenance(bank_generation=19, patch_version=2)
    assert p.state(19, 2) == signoff.CURRENT


def test_the_patch_moving_makes_it_stale():
    p = signoff.Provenance(bank_generation=19, patch_version=2)
    assert p.state(20, 3) == signoff.STALE


def test_a_shared_unit_moving_makes_it_unverified():
    """A shared unit moved and nothing can say whether it reaches this voice."""
    p = signoff.Provenance(bank_generation=19, patch_version=2)
    assert p.state(20, 2) == signoff.UNVERIFIED


def test_another_voices_patch_moving_leaves_it_current():
    """The generation runs ahead of the shared units, and only they date this.

    A patch bump moves the registry's generation, so holding a record against
    the bare number retires every voice in the bank whenever any one of the
    297 patch and drum units is touched -- and says a shared unit moved when
    none did.
    """
    p = signoff.Provenance(bank_generation=26, patch_version=2)
    assert p.state(26, 2) == signoff.CURRENT


def test_a_kit_has_no_patch_version_and_still_expires():
    """Its voices are its drum notes, so only the generation can date it."""
    p = signoff.Provenance(bank_generation=19)
    assert p.state(19, 0) == signoff.CURRENT
    assert p.state(20, 0) == signoff.UNVERIFIED


def test_a_kits_own_drum_note_moving_makes_it_stale_not_unverified():
    """Forty-three of its notes were fitted and it reported a shared unit.

    A kit is dated by the drum kinds because it has no patch unit, and that is
    the same claim a patch version makes: the voice itself changed. Folding the
    two generations together loses the distinction in the one direction that
    matters, since `unverified` reads as "something under this voice moved and
    nothing can attribute it" when the thing that moved was the voice.
    """
    p = signoff.Provenance(bank_generation=31)
    assert p.state(31, 0, 34) == signoff.STALE
    assert p.state(34, 0, 31) == signoff.UNVERIFIED
    assert p.state(31, 0, 31) == signoff.CURRENT


def test_moved_generation_reads_only_the_named_kinds(tmp_path):
    reg = tmp_path / "bank-versions.json"
    reg.write_text(
        json.dumps(
            {
                "bank_generation": 32,
                "units": {
                    "piano_voice": {
                        "kind": "shared",
                        "version": 2,
                        "history": [{"generation": 1}, {"generation": 26}],
                    },
                    "lead_voice": {
                        "kind": "patch",
                        "version": 3,
                        "history": [{"generation": 5}, {"generation": 32}],
                    },
                    "d035": {"kind": "drum", "version": 2, "history": [{"generation": 30}]},
                },
            }
        )
    )
    assert signoff.moved_generation(reg, {"shared"}) == 26
    # A kit has no patch unit of its own, so the drum kinds stand in for one.
    assert signoff.moved_generation(reg, {"drum"}) == 30
    assert signoff.moved_generation(reg, {"patch"}) == 32


def test_moved_generation_is_zero_without_a_registry(tmp_path):
    assert signoff.moved_generation(tmp_path / "absent.json", {"shared"}) == 0


# --------------------------------------------------------------------------- #
# What the last step needs
# --------------------------------------------------------------------------- #


def _axes(structure=None, music=None):
    return signoff.axis(structure, 19, 1), signoff.axis(music, 19, 1)


def test_settled_needs_both_claims(tmp_path, monkeypatch):
    prov = signoff.Provenance(bank_generation=19, patch_version=1)
    s, m = _axes(signoff.Structure(prov, accepted={}), None)
    assert not signoff.settled(s, m)
    reg = _registry(tmp_path, violin=1)
    _reference(monkeypatch)
    music = signoff.Music(prov, evidence=_evidence(reg))
    s = signoff.axis(signoff.Structure(prov, accepted={}), 19, 1)
    m = signoff.axis(music, 19, 1, registry=reg)
    assert signoff.settled(s, m), m


def test_an_open_term_blocks_settled():
    """Recording a diagnosis that still has one raises nothing, and should not."""
    prov = signoff.Provenance(bank_generation=19, patch_version=1)
    s, m = _axes(signoff.Structure(prov, unreachable=("tail",), accepted={}), signoff.Music(prov))
    assert not signoff.settled(s, m)


def test_an_expired_claim_blocks_settled():
    old = signoff.Provenance(bank_generation=18, patch_version=1)
    now = signoff.Provenance(bank_generation=19, patch_version=1)
    s = signoff.axis(signoff.Structure(old, accepted={}), 19, 1)
    m = signoff.axis(signoff.Music(now), 19, 1)
    assert s["state"] == signoff.UNVERIFIED
    assert not signoff.settled(s, m)


# --------------------------------------------------------------------------- #
# A musical claim is checked against the recording it rests on
# --------------------------------------------------------------------------- #

REFERENCE = {"id": "violin", "rig": "none", "room": "none"}


def _registry(tmp_path, *, violin=2, shared=10, generation=20, reed=1) -> Path:
    """A registry with one patch, an unrelated patch and one shared unit."""
    reg = tmp_path / "bank-versions.json"
    reg.write_text(
        json.dumps(
            {
                "bank_generation": generation,
                "units": {
                    "violin": {"kind": "patch", "version": violin, "history": [{"generation": 5}]},
                    "reed": {"kind": "patch", "version": reed, "history": [{"generation": 6}]},
                    "bowed_string_voice": {
                        "kind": "shared",
                        "version": 1,
                        "history": [{"generation": shared}],
                    },
                },
            }
        )
    )
    return reg


def _digest(reg: Path) -> str:
    return signoff.registry_digest(reg)


def _reference(monkeypatch, raw: dict | None = None) -> None:
    monkeypatch.setattr(signoff, "reference_raw", lambda capture: raw or REFERENCE)


def _evidence(reg: Path, **over) -> dict:
    out = {
        "comparison_id": "gm_gs_product",
        "scope": signoff.SCOPE_PRODUCT,
        "set": "p040-violin",
        "set_generation": "gen-1",
        "take": "single-long",
        "judged": "model",
        "program": 40,
        "request_id": "req",
        "asset_id": "asset",
        "build_id": "build",
        "bank_registry_digest": _digest(reg),
        "units": {"violin": 2},
        "counterpart": {"source": "gm041", "capture": "violin"},
        "boundary": {
            "capture": "violin",
            "rig": "none",
            "room": "none",
            "rig_evidence": "not_applicable",
            "product_rig": None,
            "page_status": "matched",
            "page_may_sign_off": True,
        },
    }
    out.update(over)
    return out


def _check(reg: Path, evidence: dict, generation: int = 20, scope=signoff.SCOPE_PRODUCT):
    prov = signoff.Provenance(date="2026-10-05", bank_generation=generation, patch_version=2)
    return signoff.claim_eligibility(evidence, prov, scope=scope, registry=reg)


def test_the_scope_names_are_boundarys():
    """Repeated so `heard.py` need not import numpy; held in step here."""
    import boundary

    assert signoff.SCOPE_PRODUCT == boundary.SCOPE_PRODUCT
    assert signoff.SCOPE_INSTRUMENT == boundary.SCOPE_INSTRUMENT
    assert signoff.STATUS_MATCHED == boundary.STATUS_MATCHED


def test_a_hand_dated_music_claim_is_unverified_not_current(tmp_path):
    """A date is not a recording: nothing says which render was heard."""
    reg = _registry(tmp_path)
    prov = signoff.Provenance(bank_generation=19, patch_version=1)
    m = signoff.axis(signoff.Music(prov), 19, 1, registry=reg)
    assert m["state"] == signoff.UNVERIFIED, m
    assert "without recording evidence" in m["reasons"][0], m
    # A date that already says stale or unverified reads exactly as before.
    stale = signoff.axis(signoff.Music(prov), 19, 2, registry=reg)
    assert stale == {"state": signoff.STALE, "date": "", "take": ""}, stale


def test_no_shipped_music_claim_without_evidence_reads_current():
    root = Path(__file__).resolve().parents[2] / "tools" / "bank-versions.json"
    _generation, units = signoff.bank_versions(root)
    shared = signoff.moved_generation(root, {"shared"})
    for slug, record in signoff.load().items():
        if record.music is None or record.music.evidence is not None:
            continue
        m = signoff.axis(record.music, shared, units.get(slug, 0), registry=root)
        assert m["state"] != signoff.CURRENT, (slug, m)


def test_load_carries_a_claims_evidence(tmp_path):
    reg = _registry(tmp_path)
    path = _write(
        tmp_path,
        {"v": {"music": {"provenance": {"bank_generation": 20}, "evidence": _evidence(reg)}}},
    )
    assert signoff.load(path)["v"].music.evidence["request_id"] == "req"


def test_an_evidenced_claim_against_the_current_registry_is_current(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    _reference(monkeypatch)
    assert _check(reg, _evidence(reg)) == (signoff.CURRENT, [])


def test_a_claim_missing_evidence_is_unverified(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    _reference(monkeypatch)
    state, why = _check(reg, _evidence(reg, asset_id=None, build_id=""))
    assert state == signoff.UNVERIFIED and "asset_id, build_id" in why[0], why


def test_a_library_without_a_bank_digest_is_unverified(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    state, why, _ = signoff.bank_state(None, ["violin"], reg)
    assert state == signoff.UNVERIFIED and "no bank registry digest" in why[0], why


def test_the_voices_own_unit_moving_makes_it_stale(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    evidence = _evidence(reg)
    _registry(tmp_path, violin=3, generation=21)
    _reference(monkeypatch)
    state, why = _check(reg, evidence)
    assert state == signoff.STALE and "violin moved from v2 to v3" in why[0], why


def test_a_shared_unit_moving_alone_makes_it_unverified(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    evidence = _evidence(reg)
    _registry(tmp_path, shared=21, generation=21)
    _reference(monkeypatch)
    state, why = _check(reg, evidence)
    assert state == signoff.UNVERIFIED and "shared unit moved at generation 21" in why[0], why


def test_another_patch_moving_leaves_it_current(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    evidence = _evidence(reg)
    _registry(tmp_path, reed=2, generation=21)
    _reference(monkeypatch)
    assert _check(reg, evidence) == (signoff.CURRENT, [])


def test_an_unknown_registry_is_found_through_history_or_unverified(tmp_path, monkeypatch):
    """A digest with no recorded unit versions is resolved by `registry_at`."""
    old = _registry(tmp_path)
    digest, then = _digest(old), json.loads(old.read_text())
    reg = _registry(tmp_path, violin=3, generation=21)
    monkeypatch.setattr(signoff, "registry_at", lambda d, path: then if d == digest else None)
    state, why, recorded = signoff.bank_state(digest, ["violin"], reg)
    assert state == signoff.STALE and recorded == {"bank_generation": 20, "units": {"violin": 2}}
    state, why, _ = signoff.bank_state("0" * 64, ["violin"], reg)
    assert state == signoff.UNVERIFIED and "neither the current one" in why[0], why


def test_registry_at_finds_a_committed_registry():
    root = Path(__file__).resolve().parents[2] / "tools" / "bank-versions.json"
    blob = subprocess.run(
        ["git", "-C", str(root.parents[1]), "show", "HEAD:tools/bank-versions.json"],
        capture_output=True,
        check=True,
    ).stdout
    found = signoff.registry_at(hashlib.sha256(blob).hexdigest(), root)
    assert found is not None and found["bank_generation"] == json.loads(blob)["bank_generation"]


def test_the_reference_boundary_moving_makes_it_stale(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    _reference(monkeypatch, dict(REFERENCE, room="present"))
    state, why = _check(reg, _evidence(reg))
    assert state == signoff.STALE and "room none -> present" in " ".join(why), why


def test_a_comparison_the_page_did_not_find_signable_is_unverified(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    _reference(monkeypatch)
    boundary = dict(_evidence(reg)["boundary"], page_status="context_only")
    state, why = _check(reg, _evidence(reg, boundary=boundary))
    assert state == signoff.UNVERIFIED and "context_only" in " ".join(why), why


def test_a_di_or_candidate_judgement_cannot_stand_as_music(tmp_path, monkeypatch):
    reg = _registry(tmp_path)
    _reference(monkeypatch)
    di = _evidence(reg, scope=signoff.SCOPE_INSTRUMENT, judged="model-di")
    state, why = _check(reg, di)
    assert state == signoff.UNVERIFIED and "instrument judgement" in why[0], why
    state, why = _check(reg, _evidence(reg, judged="bow-light"))
    assert state == signoff.UNVERIFIED and "bow-light" in why[0], why


def test_a_di_reference_against_an_unestablished_product_rig_is_refused(tmp_path, monkeypatch):
    """A rig-capable voice's direct recording cannot sign off a product path that may carry a rig."""
    reg = _registry(tmp_path)
    _reference(monkeypatch, dict(REFERENCE, rig_evidence="verified"))
    boundary = dict(_evidence(reg)["boundary"], rig_evidence="verified")
    state, why = _check(reg, _evidence(reg, program=27, boundary=boundary))
    assert state == signoff.UNVERIFIED and "context_only" in " ".join(why), why
