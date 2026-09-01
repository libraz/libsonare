"""The last two claims: that they load, name real voices, and expire correctly."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import bank  # noqa: E402
import signoff  # noqa: E402


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
        Path(__file__).resolve().parents[2] / "tools" / "bank-versions.json")
    for slug, record in signoff.load().items():
        for claim in (record.structure, record.music):
            if claim is None:
                continue
            assert claim.provenance.bank_generation, f"{slug}: no bank generation recorded"
            assert claim.provenance.bank_generation <= generation, (
                f"{slug}: recorded against generation {claim.provenance.bank_generation}, "
                f"and the registry is only on {generation}")
            if claim.provenance.patch_version:
                assert claim.provenance.patch_version <= max(units.values() or [0])


# --------------------------------------------------------------------------- #
# An unreachable term is accepted with a reason, or it is open
# --------------------------------------------------------------------------- #

def test_an_accepted_term_needs_a_reason(tmp_path):
    path = _write(tmp_path, {"v": {"structure": {
        "unreachable": ["tail"], "accepted": {"tail": "  "}}}})
    with pytest.raises(ValueError, match="carry no reason"):
        signoff.load(path)


def test_a_term_the_diagnosis_never_reported_cannot_be_accepted(tmp_path):
    """Otherwise an accepted list drifts into an argument about a term nobody measured."""
    path = _write(tmp_path, {"v": {"structure": {
        "unreachable": ["tail"], "accepted": {"harm": "not this one"}}}})
    with pytest.raises(ValueError, match="not in `unreachable`"):
        signoff.load(path)


def test_an_unreachable_term_with_no_reason_is_open(tmp_path):
    path = _write(tmp_path, {"v": {"structure": {
        "unreachable": ["tail", "harm"], "accepted": {"harm": "the reference's room"}}}})
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


def test_the_generation_alone_moving_makes_it_unverified():
    """A shared unit moved and nothing can say whether it reaches this voice."""
    p = signoff.Provenance(bank_generation=19, patch_version=2)
    assert p.state(20, 2) == signoff.UNVERIFIED


def test_a_kit_has_no_patch_version_and_still_expires():
    """Its voices are its drum notes, so only the generation can date it."""
    p = signoff.Provenance(bank_generation=19)
    assert p.state(19, 0) == signoff.CURRENT
    assert p.state(20, 0) == signoff.UNVERIFIED


# --------------------------------------------------------------------------- #
# What the last step needs
# --------------------------------------------------------------------------- #

def _axes(structure=None, music=None):
    return signoff.axis(structure, 19, 1), signoff.axis(music, 19, 1)


def test_settled_needs_both_claims():
    prov = signoff.Provenance(bank_generation=19, patch_version=1)
    s, m = _axes(signoff.Structure(prov, accepted={}), None)
    assert not signoff.settled(s, m)
    s, m = _axes(signoff.Structure(prov, accepted={}), signoff.Music(prov))
    assert signoff.settled(s, m)


def test_an_open_term_blocks_settled():
    """Recording a diagnosis that still has one raises nothing, and should not."""
    prov = signoff.Provenance(bank_generation=19, patch_version=1)
    s, m = _axes(signoff.Structure(prov, unreachable=("tail",), accepted={}),
                 signoff.Music(prov))
    assert not signoff.settled(s, m)


def test_an_expired_claim_blocks_settled():
    old = signoff.Provenance(bank_generation=18, patch_version=1)
    now = signoff.Provenance(bank_generation=19, patch_version=1)
    s = signoff.axis(signoff.Structure(old, accepted={}), 19, 1)
    m = signoff.axis(signoff.Music(now), 19, 1)
    assert s["state"] == signoff.UNVERIFIED
    assert not signoff.settled(s, m)
