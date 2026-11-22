"""A bound remembers the reader version it was set under, and a run may hold one untightened.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_reader_version.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module

import profile_gate
import profile_measure
from profile_gate import unison_hold, write_gate_file


@pytest.fixture(autouse=True)
def _build(monkeypatch):
    monkeypatch.setattr(profile_gate, "_model_build_state", lambda: {"built_utc": "build-1"})
    monkeypatch.setattr(profile_measure, "READER_VERSIONS", {"balance": 2})


def _balance(value, decay=5.0):
    return {
        "balance": {"median": value, "abs_median": value, "p90": value, "n": 10},
        "decay": {"median": decay, "abs_median": decay, "p90": decay, "n": 10},
    }


def _gate(tmp_path: Path, value: float, reader: int | None, **notes) -> Path:
    """A gate whose balance bounds were set under @p reader (None: before readers were versioned)."""
    path = tmp_path / "g.json"
    write_gate_file(_balance(value), path, "ref", 1.0)
    doc = json.loads(path.read_text())
    for prov in doc["bounds"]["balance"]["provenance"].values():
        prov.pop("set_reader", None)
        if reader is not None:
            prov["set_reader"] = reader
    path.write_text(json.dumps({**doc, **notes}))
    return path


def _prov(path: Path, dim: str = "balance", stat: str = "median") -> dict:
    return json.loads(path.read_text())["bounds"][dim]["provenance"][stat]


def _bound(path: Path, dim: str = "balance", stat: str = "median") -> float:
    return json.loads(path.read_text())["bounds"][dim][stat]


def test_a_bound_another_reader_set_that_the_new_reading_exceeds_is_rewindowed(tmp_path, capsys):
    gate = _gate(tmp_path, 3.0, reader=1)
    capsys.readouterr()
    write_gate_file(_balance(4.0), gate, "ref", 1.0)
    prov = _prov(gate)
    assert _bound(gate) == 4.0
    assert prov["outcome"] == "rewindowed" and prov["set_reader"] == 2
    assert prov["rewindowed_from"] == {"bound": 3.0, "reader": 1}
    out = capsys.readouterr().out
    assert (
        "rewindowed balance.median bound 4 (model residual 4 under reader 2; was 3 under reader 1)"
        in out
    )
    notes = profile_gate.provenance_notes(json.loads(gate.read_text()))
    assert any("balance.median: re-recorded for a reader change" in n for n in notes)


def test_a_bound_another_reader_set_that_still_holds_ratchets_and_keeps_its_reader(tmp_path):
    gate = _gate(tmp_path, 3.0, reader=1)
    write_gate_file(_balance(3.0), gate, "ref", 1.0)
    prov = _prov(gate)
    # Kept, not rewound: it still limits the new reading, and it records it was set under 1.
    assert _bound(gate) == 3.0 and prov["outcome"] == "kept" and prov["set_reader"] == 1
    assert "rewindowed_from" not in prov
    write_gate_file(_balance(2.0), gate, "ref", 1.0)
    prov = _prov(gate)
    assert _bound(gate) == 2.0 and prov["outcome"] == "tightened" and prov["set_reader"] == 2


def test_a_file_with_no_reader_version_reads_as_the_old_reader(tmp_path):
    gate = _gate(tmp_path, 3.0, reader=None)
    write_gate_file(_balance(3.5), gate, "ref", 1.0)
    assert _prov(gate)["rewindowed_from"] == {"bound": 3.0, "reader": 1}
    # A dimension no reader version lists is not versioned and ratchets as before.
    assert _prov(gate, "decay")["outcome"] == "kept" and "set_reader" not in _prov(gate, "decay")


def test_a_bound_under_the_same_reader_still_ratchets_tighten_only(tmp_path):
    gate = _gate(tmp_path, 3.0, reader=2)
    write_gate_file(_balance(4.0), gate, "ref", 1.0)
    prov = _prov(gate)
    assert _bound(gate) == 3.0 and prov["outcome"] == "exceeded" and prov["set_reader"] == 2


def test_a_check_judges_a_bound_another_reader_set_and_says_so_when_it_fails(tmp_path, capsys):
    gate = _gate(tmp_path, 3.0, reader=1)
    assert profile_module.check_gate(_balance(1.0), gate, "ref") == 0
    capsys.readouterr()
    assert profile_module.check_gate(_balance(4.0), gate, "ref") == 1
    assert "over its bound 3.00 (set under reader 1, read by 2)" in capsys.readouterr().out


def test_a_held_dimension_keeps_its_bound_where_a_run_would_tighten_it(tmp_path, capsys):
    hold = {"balance": "h6 straddles the presence margin"}
    gate = _gate(tmp_path, 3.0, reader=2, _no_tighten=hold)
    capsys.readouterr()
    write_gate_file(_balance(2.0, decay=4.0), gate, "ref", 1.0)
    doc = json.loads(gate.read_text())
    assert _bound(gate) == 3.0 and _prov(gate)["outcome"] == "kept"
    assert _bound(gate, "decay") == 4.0
    assert doc["_no_tighten"] == hold
    assert "balance.median 2 -> 3 (held untightened: h6 straddles the presence margin)" in (
        capsys.readouterr().out
    )


def test_a_held_bound_still_moves_when_the_new_reader_exceeds_it(tmp_path):
    gate = _gate(tmp_path, 3.0, reader=1, _no_tighten={"balance": "on the presence cliff"})
    write_gate_file(_balance(4.0), gate, "ref", 1.0)
    assert _bound(gate) == 4.0 and _prov(gate)["outcome"] == "rewindowed"


def _rows(n, ensemble, width):
    return [
        {"unwarp": {"state": "ensemble" if i < ensemble else "single_source", "width_ratio": width}}
        for i in range(n)
    ]


def test_a_detuned_unison_by_measurement_holds_balance_and_a_clean_voice_does_not():
    member = unison_hold(_rows(9, 5, 2.18), _rows(9, 0, 1.0))
    assert member.startswith("measured: detuned unison on the reference (5 of 9 rows ensemble")
    assert "-8.8 to -13.6 dB" in member
    assert "on the model" in unison_hold(_rows(9, 1, 1.0), _rows(9, 2, 3.25))
    # Ensemble without broadened lines, or broadened lines on too few ensemble rows: no hold.
    assert unison_hold(_rows(9, 9, 1.17), _rows(9, 0, 1.0)) is None
    assert unison_hold(_rows(35, 5, 6.2), []) is None


def test_a_computed_hold_is_regenerated_and_a_hand_written_one_carried(tmp_path):
    gate = _gate(tmp_path, 3.0, reader=2, _no_tighten={"body": "hand-written"})
    write_gate_file(_balance(2.0), gate, "ref", 1.0, held={"balance": "measured: unison"})
    doc = json.loads(gate.read_text())
    assert doc["_no_tighten"] == {"body": "hand-written", "balance": "measured: unison"}
    assert _bound(gate) == 3.0
    write_gate_file(_balance(2.0), gate, "ref", 1.0)
    doc = json.loads(gate.read_text())
    assert doc["_no_tighten"] == {"body": "hand-written"} and _bound(gate) == 2.0


def test_a_bound_already_failing_under_the_old_reader_stays_failing_rather_than_rewinding(tmp_path):
    gate = _gate(tmp_path, 3.0, reader=1)
    write_gate_file(_balance(4.0), gate, "ref", 1.0)  # rewound: the new reader's own number
    doc = json.loads(gate.read_text())
    for prov in doc["bounds"]["balance"]["provenance"].values():
        prov.update(outcome="exceeded", set_reader=1)
    doc["bounds"]["balance"].update(median=3.0, abs_median=3.0, p90=3.0)
    gate.write_text(json.dumps(doc))
    write_gate_file(_balance(4.2), gate, "ref", 1.0)
    prov = _prov(gate)
    # The voice failed it under reader 1 already: the reader did not make it fail, so no rewind.
    assert _bound(gate) == 3.0 and prov["outcome"] == "exceeded" and prov["set_reader"] == 1
