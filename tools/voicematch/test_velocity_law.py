"""The reference's level-to-velocity law: a source's mapping table or a per-note dynamic.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_velocity_law.py -q
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))

import gate_audit
from profile_gate import write_summary_json
from profile_summary import (
    VELOCITY_LAW_FIELD,
    format_velocity_law,
    summarize,
    velocity_law,
    velocity_law_reading,
)

GRID = (32, 56, 80, 104, 127)
NOTES = (40, 52, 64, 76)


def rows(level, notes=NOTES, velocities=GRID, timbre="t"):
    return [
        {"timbre": timbre, "note": n, "velocity": v, VELOCITY_LAW_FIELD: level(n, v)}
        for n in notes
        for v in velocities
    ]


def power(k: float):
    """Gain proportional to velocity^k, under a per-note offset the law must ignore."""
    return lambda n, v: -40.0 + 0.3 * (n - 40) + 20.0 * k * math.log10(v / 127)


def test_a_power_law_on_every_note_reads_as_a_mapping_with_its_exponent():
    law = velocity_law(rows(power(1.5)))
    assert law["class"] == "mapping"
    assert law["k"] == pytest.approx(1.5, abs=1e-3)
    assert law["k_iqr"] == pytest.approx(0.0, abs=1e-3)
    assert [p["velocities"] for p in law["pairs"]] == [[32, 56], [56, 80], [80, 104], [104, 127]]
    assert not any(p["flat"] for p in law["pairs"])
    assert law["spread_db"] == pytest.approx(0.0, abs=1e-3)
    assert (
        format_velocity_law(law)
        == "velocity law (held_peak_dbfs): mapping k=1.50, spread 0.00 dB (IQR k 0.00)"
    )


def test_a_note_dependent_law_varies_by_note():
    law = velocity_law(rows(lambda n, v: power(0.5 + (n - 40) / 24)(n, v)))
    assert law["class"] == "varies by note"
    assert law["spread_db"] > 0.25
    assert format_velocity_law(law) == (
        f"velocity law (held_peak_dbfs): varies by note, spread {law['spread_db']:.2f} dB"
    )


def test_a_top_pair_with_no_step_reads_flat():
    law = velocity_law(rows(lambda n, v: power(1.5)(n, min(v, 104))))
    flat = [p["velocities"] for p in law["pairs"] if p["flat"]]
    assert flat == [[104, 127]]
    # The flat pair stays out of the exponent rather than pulling it toward zero.
    assert law["k"] == pytest.approx(1.5, abs=1e-3)
    assert format_velocity_law(law) == (
        "velocity law (held_peak_dbfs): mapping k=1.50, flat at v104-v127, spread 0.00 dB (IQR k 0.00)"
    )


def test_one_velocity_or_too_few_notes_gives_no_reading():
    assert velocity_law(rows(power(1.5), velocities=(80,))) is None
    assert velocity_law(rows(power(1.5), notes=(40, 52))) is None
    assert format_velocity_law(None) == "velocity law (held_peak_dbfs): no reading"


def test_the_summary_records_the_law_beside_the_velocity_response():
    summary = summarize(rows(power(1.5)))
    assert summary["t"]["velocity_law"]["class"] == "mapping"


def test_a_profile_without_the_field_is_computed_from_its_rows_and_says_so():
    profile = {"summary": {"t": {"velocity_response": {}}}, "rows": rows(power(1.5))}
    law, recorded = velocity_law_reading(profile, "t")
    assert not recorded
    assert law["k"] == pytest.approx(1.5, abs=1e-3)
    assert format_velocity_law(law, recorded) == (
        "velocity law (held_peak_dbfs): mapping k=1.50, spread 0.00 dB (IQR k 0.00) (computed, not recorded)"
    )
    # A recorded field is read as recorded, even where the rows would say otherwise.
    profile["summary"]["t"]["velocity_law"] = None
    assert velocity_law_reading(profile, "t") == (None, True)


def test_compare_json_and_the_gate_audit_carry_the_law(tmp_path):
    law = velocity_law(rows(power(1.5)))
    out = tmp_path / "compare.json"
    write_summary_json(out, {}, "", "t", "", velocity_law={"reading": law, "recorded": False})
    body = json.loads(out.read_text())
    assert body["velocity_law"]["recorded"] is False
    entries = [{"key": "cap:vel_range", "stat": "median"}]
    got = gate_audit.format_velocity_laws({"cap": body["velocity_law"]}, entries)
    assert got == [
        "  cap: velocity law (held_peak_dbfs): mapping k=1.50, spread 0.00 dB (IQR k 0.00) (computed, not recorded)"
    ]
    # A capture whose vel_range the audit did not read prints nothing.
    assert gate_audit.format_velocity_laws({"cap": body["velocity_law"]}, []) == []


def test_a_committed_mapping_reference_reads_the_steps_measured_independently():
    """synth_strings_1 / gm051, read off `held_peak_dbfs` by an independent script:
    32->56 +8.2 dB k=1.68, 56->80 +5.8 k=1.86, 80->104 +4.4 k=1.93, 104->127 +3.4 k=1.95,
    every pair's across-note IQR at most 0.05 dB."""
    profile = json.loads((HERE / "reference" / "synth_strings_1.json").read_text())
    law, _ = velocity_law_reading(profile, "gm051")
    assert law["class"] == "mapping"
    expected = {
        (32, 56): (8.2, 1.68),
        (56, 80): (5.8, 1.86),
        (80, 104): (4.4, 1.93),
        (104, 127): (3.4, 1.95),
    }
    got = {tuple(p["velocities"]): (p["step_db"], p["k"]) for p in law["pairs"]}
    assert set(got) == set(expected)
    for pair, (step, k) in expected.items():
        assert got[pair][0] == pytest.approx(step, abs=0.05)
        assert got[pair][1] == pytest.approx(k, abs=0.005)
