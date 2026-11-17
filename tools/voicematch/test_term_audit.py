"""Every loss term, read for what a model that loses a reading costs against one that is merely off.

The table is derived by running the terms: each numeric field of a base row set is shifted and
blanked in turn, the terms it moves are found, and a term's absent cost is compared with the
dearest present shift of the same field. `term_table` returns it, one row per `LOSS_TERMS` entry.
"""

from __future__ import annotations

import copy

import pytest
import test_own_term_readers as readers
from loss import loss_terms, percussion_terms
from loss_dimensions import LOSS_TERMS
from loss_weights import TERM_COUNT_KEYS

#: A row's identity, which pairs the two sides; it is not a reading either side can lose.
IDENTITY = ("note", "velocity")
SHIFTS = (1.0, 3.0, 10.0, 30.0, 100.0, 300.0, 1000.0)
TARGETS = (0, 1, 2)
#: Terms the base rows cannot reach, and why.
NOT_REACHED = {
    "mss": "a whole-timeline distance passed in, not a per-row reading",
}
#: Terms whose every reading is a field the analyzers always write, so no row can lack it.
REQUIRED = {
    "harm": "the ladder's absent cell is a -120 dB sentinel, not a missing value",
    "dyn": "reads the held ladder",
    "band": "reads the band profile every hit carries",
    "bandrms": "reads the band profile every hit carries",
    "tilt": "reads the band profile every hit carries",
    "ring": "reads the decay every hit carries",
}
#: Where an absent reading still costs less than a present one, and why.
EXCEPTIONS = {
    (
        "env",
        "attack_fine_ms",
    ): "read as `attack_ms` where the fine reading is missing, which charges",
}


def _shift(value, d: float, ramp: bool):
    if isinstance(value, bool) or value is None:
        return value
    if isinstance(value, (int, float)):
        return value + d
    if isinstance(value, list):
        n = max(len(value) - 1, 1)
        return [_shift(x, d * (i / n if ramp else 1.0), ramp) for i, x in enumerate(value)]
    if isinstance(value, dict):
        return {k: _shift(x, d, ramp) for k, x in value.items()}
    return value


def _numeric(value) -> bool:
    if isinstance(value, bool):
        return False
    if isinstance(value, (int, float)):
        return True
    if isinstance(value, list):
        return any(_numeric(x) for x in value)
    if isinstance(value, dict):
        return any(_numeric(x) for x in value.values())
    return False


#: Lists that hold only the modes found, so a list of None is not a way to lose a reading.
MEASURED_LISTS = ("modal_hz", "modal_db")


def _blanks(row: dict, field: str):
    value = row[field]
    yield {**row, field: None}
    yield {k: v for k, v in row.items() if k != field}
    if isinstance(value, list) and field not in MEASURED_LISTS:
        yield {**row, field: [None] * len(value)}


def _pitched_rows() -> list[dict]:
    row, _ = readers._pitched()
    rows = []
    for i, velocity in enumerate((40, 80, 120)):
        r = copy.deepcopy(row)
        r["velocity"] = velocity
        r["mono_peak_dbfs"] = row["mono_peak_dbfs"] + 8 * i
        r["held_peak_dbfs"] = row["held_peak_dbfs"] + 6 * i
        r["peak_dbfs"] = row["peak_dbfs"] + 6 * i
        r["harmonics_db"] = [row["harmonics_db"][0]] + [
            h + 3 * i if h > -120 else h for h in row["harmonics_db"][1:]
        ]
        r["attack_hf_db"] = [None if x is None else x + 4 * i for x in row["attack_hf_db"]]
        r["onset_overshoot_db"] = (row["onset_overshoot_db"] or 0.0) + i
        r["onset_settle_ms"] = (row["onset_settle_ms"] or 30.0) + 10 * i
        r["skeleton"]["tail_db_s"] = [-6.0 - 0.1 * k for k in range(12)]
        rows.append(r)
    return rows


def _kit_rows() -> list[dict]:
    (row, _) = readers._percussive(1.0, 1)
    rows = []
    for i, velocity in enumerate((40, 80, 120)):
        r = copy.deepcopy(row)
        r["velocity"] = velocity
        r["mono_peak_dbfs"] = row["mono_peak_dbfs"] + 8 * i
        r["peak_dbfs"] = row["peak_dbfs"] + 6 * i
        r["held_rms_dbfs"] = row["held_rms_dbfs"] + 6 * i
        rows.append(r)
    return rows


def _family_rows() -> tuple[list[dict], dict]:
    (row, _) = readers._percussive(1.0, 1)
    rows = []
    for i, note in enumerate((41, 43, 45)):
        r = copy.deepcopy(row)
        r.update(note=note, velocity=100, decay_ms=row["decay_ms"] * (1.0 + 0.3 * i))
        r["tone_f0_hz"] = 100.0 * (1.2**i)
        r["centroid_hz"] = row["centroid_hz"] * (1.0 + 0.2 * i)
        r["decay_capped"] = False
        rows.append(r)
    return rows, {"toms": [41, 43, 45]}


def _runs():
    kit_rows, groups = _family_rows()
    return (
        ("pitched", loss_terms, _pitched_rows()),
        ("kit", percussion_terms, _kit_rows()),
        ("family", lambda m, o: percussion_terms(m, o, groups=groups), kit_rows),
    )


def _cost(fn, model, oracle, term):
    """The term's value, `inf` where the evaluation is refused outright, None where a row is required."""
    try:
        out = fn(model, oracle)
    except (KeyError, TypeError):
        return None
    return float("inf") if out is None else out[term]


def term_table() -> dict[str, dict]:
    """One entry per term: `status`, `absent`, `present`, `count_key` and the fields inspected."""
    table = {
        t: {
            "status": "not reached",
            "absent": None,
            "present": None,
            "fields": [],
            "worst": [],
            "ref_charged": 0.0,
        }
        for t in LOSS_TERMS
    }
    for _name, fn, rows in _runs():
        base = fn(rows, rows)
        for field in rows[0]:
            if field in IDENTITY or not _numeric(rows[0][field]):
                continue
            for target in TARGETS:
                moved: dict[str, float] = {}
                for ramp in (False, True):
                    for d in SHIFTS:
                        shifted = [
                            {**r, field: _shift(r[field], d, ramp)} if i == target else r
                            for i, r in enumerate(rows)
                        ]
                        out = fn(shifted, rows)
                        for t in LOSS_TERMS:
                            if out is not None and abs(out[t] - base[t]) > 1e-9:
                                moved[t] = max(moved.get(t, 0.0), out[t] - base[t])
                for term, present in moved.items():
                    costs = [
                        _cost(
                            fn,
                            [blank if i == target else r for i, r in enumerate(rows)],
                            rows,
                            term,
                        )
                        for blank in _blanks(rows[target], field)
                    ]
                    live = [c - base[term] for c in costs if c is not None]
                    entry = table[term]
                    entry["fields"].append(field)
                    # The same field lost on the reference side, the model intact: a cell dropped.
                    for blank in _blanks(rows[target], field):
                        gone = _cost(
                            fn,
                            rows,
                            [blank if i == target else r for i, r in enumerate(rows)],
                            term,
                        )
                        if gone is not None:
                            entry["ref_charged"] = max(entry["ref_charged"], gone - base[term])
                    if not live:
                        entry["status"] = (
                            entry["status"] if entry["status"] != "not reached" else "required"
                        )
                        continue
                    absent = min(live)
                    inverted = absent < present - 1e-9
                    entry["absent"] = (
                        absent if entry["absent"] is None else min(entry["absent"], absent)
                    )
                    entry["present"] = (
                        present if entry["present"] is None else max(entry["present"], present)
                    )
                    if inverted:
                        entry["worst"].append(field)
                    entry["status"] = "inverted" if entry["worst"] else "capped"
    for t, entry in table.items():
        entry["count_key"] = TERM_COUNT_KEYS.get(t)
        entry["fields"] = sorted(set(entry["fields"]))
        entry["worst"] = sorted(set(entry["worst"]))
    return table


@pytest.fixture(scope="module")
def table():
    return term_table()


def test_every_term_is_accounted_for(table):
    assert set(table) == set(LOSS_TERMS)
    unreached = {t for t, e in table.items() if e["status"] == "not reached"}
    assert unreached == set(NOT_REACHED), unreached ^ set(NOT_REACHED)
    required = {t for t, e in table.items() if e["status"] == "required"}
    assert required == set(REQUIRED), required ^ set(REQUIRED)


def test_no_term_charges_an_absent_reading_less_than_a_present_one_but_the_named_exceptions(table):
    found = {(t, f) for t, e in table.items() for f in e["worst"]}
    assert found == set(EXCEPTIONS), sorted(found ^ set(EXCEPTIONS))


def test_a_reading_the_reference_lacks_is_dropped_without_charge(table):
    charged = {t: e["ref_charged"] for t, e in table.items() if e["ref_charged"] > 1e-9}
    assert not charged, charged


RELATIONS = (
    ("velspan", "pitched", "mono_peak_dbfs"),
    ("hfdyn", "pitched", "attack_hf_db"),
    ("onsetvel", "pitched", "onset_overshoot_db"),
    ("onsetvel", "pitched", "attack_fine_ms"),
    ("modedyn", "pitched", "modal_db"),
    ("modedyn", "pitched", "modal_hz"),
    ("kit", "family", "centroid_hz"),
    ("kit", "family", "tone_f0_hz"),
)


@pytest.mark.parametrize("term,run,field", RELATIONS)
def test_a_relation_the_model_went_blind_on_costs_no_less_than_any_present_delta(term, run, field):
    """The model loses one cell of a relation the reference holds; the reference stays intact."""
    _name, fn, rows = next(r for r in _runs() if r[0] == run)
    for target in TARGETS:
        absent = []
        for blank in _blanks(rows[target], field):
            cost = _cost(fn, [blank if i == target else r for i, r in enumerate(rows)], rows, term)
            if cost is not None:
                absent.append(cost)
        assert absent and min(absent) > 0.0, (field, target)
        for ramp in (False, True):
            for d in SHIFTS:
                moved = [
                    {**r, field: _shift(r[field], d, ramp)} if i == target else r
                    for i, r in enumerate(rows)
                ]
                assert min(absent) >= fn(moved, rows)[term] - 1e-9, (field, target, d)
