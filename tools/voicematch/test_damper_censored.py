"""The count of damper rows censored, printed beside `damper` and carried to the audit.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_damper_censored.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import gate_audit
from profile_gate import print_summary_table, write_summary_json


def _summary():
    row = {"median": 1.0, "abs_median": 1.0, "p90": 1.0, "n": 4}
    return {"damper": dict(row), "decay": dict(row)}


def test_the_compare_table_prints_the_censored_count_beside_damper_only(capsys):
    print_summary_table(_summary(), {}, 6, None, {"damper": 2})
    lines = capsys.readouterr().out.splitlines()
    assert any("damper" in ln and ln.endswith("[2 censored]") for ln in lines)
    assert not any("decay" in ln and "censored" in ln for ln in lines)


def test_a_clean_damper_still_prints_its_zero(capsys):
    print_summary_table(_summary(), {}, 6, None, {"damper": 0})
    assert "[0 censored]" in capsys.readouterr().out


def test_the_summary_json_records_the_count(tmp_path):
    out = tmp_path / "s.json"
    write_summary_json(out, _summary(), "", "t", "now", censored={"damper": 3})
    assert json.loads(out.read_text())["censored"] == {"damper": 3}


def test_the_audit_carries_the_count_onto_the_damper_reading_and_prints_it(tmp_path, monkeypatch):
    def fake_compare(voice, out, lib, overrides="", **_):
        readings = [
            {"key": k, "stat": "p90", "value": 3.0, "limit": 1.0, "over": True, "excess": 2.0}
            for k in ("damper", "decay")
        ]
        out.write_text(json.dumps({"gate": {"readings": readings}, "censored": {"damper": 4}}))

    monkeypatch.setattr(gate_audit, "compare", fake_compare)
    out = tmp_path / "post.json"
    gate_audit.compare_voice({"slug": "v", "capture": "c"}, out, Path("lib"))
    body = json.loads(out.read_text())
    by_key = {r["key"]: r for r in body["gate"]["readings"]}
    assert by_key["damper"]["censored"] == 4
    assert "censored" not in by_key["decay"]

    pre = {"gate": {"readings": [{**r, "value": 0.5} for r in body["gate"]["readings"]]}}
    text = "\n".join(gate_audit.format_flagged(gate_audit.audit(pre, body, None)))
    assert "damper p90: 0.5 -> 3 vs bound 1 (weighting unknown) [4 damper rows censored]" in text
    assert "decay p90: 0.5 -> 3 vs bound 1 (weighting unknown)\n" in text + "\n"
