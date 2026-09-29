"""Tests for tools/gs/soundings_drift.py against a minimal fake archive."""

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "gs" / "soundings_drift.py"
UNIT = "roland-sc8850-01"
RECORD = "data/units/roland-sc8850-01/efx-orders/%s-the-byte-as-orders.json"
NAMED = "40 03 07"
BOUND = "40 03 06"
ITEM = "the phase byte holds past 5A"


def item_hash(text: str) -> str:
    return hashlib.sha1(text.encode("utf-8")).hexdigest()[:12]


def write(path: Path, value) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value if isinstance(value, str) else json.dumps(value))


class DriftTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        self.archive = root / "archive"
        self.bindings = root / "bindings"
        self.overlays = root / "overlays"
        self.inc = root / "models.inc"
        self.tables = root / "efx-tables.json"
        write(self.tables, {"map": []})
        self.inferences = self.archive / "inferences"
        self.model_path = self.inferences / "models" / "m.json"
        self.stage_path = self.inferences / UNIT / "stages" / "01-30.json"
        self.candidates = self.inferences / "candidates" / "whole-0130.json"
        self.model = {
            "model": {},
            "nodes": [
                {
                    "id": "a",
                    "kind": "gain",
                    "gain": {
                        "byte": "40 03 06",
                        "source": "document",
                        "rests_on": [RECORD % "01-30-06"],
                        "fitted_on": [],
                    },
                }
            ],
        }
        self.stage = {
            "type": "01 30",
            "p0": {"model": "inferences/models/m.json"},
            "p1": {"verdict": "stopped", "gates": {"gross": {"residual_over_span": 0.5}}},
        }
        self.claim = {
            "inference": {
                "state": "standing",
                "about": {"types": ["01 30"], "addresses": ["40 03 07"], "quantities": ["q"]},
            }
        }
        self.claim["rests_on"] = {"measurements": [{"file": RECORD % "01-30-03"}]}
        write(self.inferences / UNIT / "c.json", self.claim)
        write(self.candidates, {"what_no_candidate_here_predicts": [ITEM]})
        self.flush()
        designed = {"basis": "invented", "law": "d.x"}
        write(
            self.bindings / "01.json",
            [
                {
                    "type": "01 30",
                    "slot": 4,
                    "designed": {**designed, "replaced_when": {"claim_names": ["01 30", NAMED]}},
                },
                {
                    "type": "01 30",
                    "slot": 3,
                    "designed": {**designed, "replaced_when": {"model_binding": ["01 30", BOUND]}},
                },
                {
                    "type": "01 30",
                    "slot": 5,
                    "enables": {
                        "stages": [{"stage": "s"}],
                        "basis": "invented",
                        "replaced_when": {"stage_passed": "01 30"},
                    },
                },
            ],
        )
        write(
            self.overlays / "0130.json",
            {
                "type": "01 30",
                "covers": [
                    {
                        "covers": item_hash(ITEM),
                        "rationale": "r",
                        "replaced_when": {"covers": item_hash(ITEM)},
                    }
                ],
            },
        )

    def flush(self) -> None:
        """Write the model, stage and inc from the current in-memory state."""
        write(self.model_path, self.model)
        write(self.stage_path, self.stage)
        sha = hashlib.sha256(self.model_path.read_bytes()).hexdigest()
        self.recorded_sha = getattr(self, "recorded_sha", sha)
        row = f'{{0x0130, 0, 1, 0, 1, 1, 1, 1u, K, "{self.recorded_sha}", 0.5f, {{0}}, {{0}}}}'
        write(self.inc, f"    {row},\n")

    def run_tool(self) -> tuple[int, list[str]]:
        proc = subprocess.run(
            [
                sys.executable,
                str(TOOL),
                "--archive",
                str(self.archive),
                "--bindings",
                str(self.bindings),
                "--overlays",
                str(self.overlays),
                "--tables",
                str(self.tables),
                "--inc",
                str(self.inc),
            ],
            capture_output=True,
            text=True,
        )
        return proc.returncode, [ln for ln in proc.stdout.splitlines() if ln.strip()]

    def assert_reports(self, kind: str) -> None:
        code, lines = self.run_tool()
        self.assertEqual(code, 3, lines)
        self.assertTrue(any(ln.startswith(kind + ":") for ln in lines), lines)

    def test_unchanged_archive_reports_nothing(self) -> None:
        self.assertEqual(self.run_tool(), (0, []))

    def test_claim_added(self) -> None:
        self.claim["rests_on"]["measurements"].append({"file": RECORD % "01-30-07"})
        write(self.inferences / UNIT / "c.json", self.claim)
        self.assert_reports("claim_names")

    def add_record(self) -> None:
        self.claim["rests_on"]["measurements"].append({"file": RECORD % "01-30-07"})
        write(self.inferences / UNIT / "c.json", self.claim)

    def test_parked_claim_does_not_fire(self) -> None:
        self.claim["inference"]["state"] = "parked"
        self.add_record()
        self.assertEqual(self.run_tool(), (0, []))

    def test_quantityless_claim_does_not_fire(self) -> None:
        self.claim["inference"]["about"]["quantities"] = []
        self.add_record()
        self.assertEqual(self.run_tool(), (0, []))

    def test_table_reaches_only_when_mapped(self) -> None:
        row = {"type": "01 30", "slot": 6, "designed": {"basis": "carried", "law": "x"}}
        row["designed"]["replaced_when"] = {"table_reaches": ["01 30", "40 03 09"]}
        write(self.bindings / "extra.json", [row])
        self.assertEqual(self.run_tool(), (0, []))
        write(self.tables, {"map": [{"type": "01 30", "address": "40 03 09"}]})
        self.assert_reports("table_reaches")

    def test_claim_about_without_record_is_not_drift(self) -> None:
        _, lines = self.run_tool()
        self.assertFalse(any(ln.startswith("claim_names:") for ln in lines), lines)

    def test_binding_source_changed(self) -> None:
        gain = self.model["nodes"][0]["gain"]
        gain.update(source="law", fitted_on=[["x", 1]])
        self.flush()
        self.assert_reports("model_binding")

    def test_binding_without_fit_is_not_drift(self) -> None:
        self.model["nodes"][0]["gain"]["source"] = "law"
        self.flush()
        _, lines = self.run_tool()
        self.assertFalse(any(ln.startswith("model_binding:") for ln in lines), lines)

    def test_fit_resting_on_another_pair_is_not_drift(self) -> None:
        gain = self.model["nodes"][0]["gain"]
        gain.update(source="law", fitted_on=[["x", 1]], rests_on=[RECORD % "01-31-06"])
        self.flush()
        _, lines = self.run_tool()
        self.assertFalse(any(ln.startswith("model_binding:") for ln in lines), lines)

    def test_stage_passed(self) -> None:
        self.stage["p1"]["verdict"] = "passed"
        self.flush()
        self.assert_reports("stage_passed")

    def test_model_sha_changed(self) -> None:
        self.model["nodes"][0]["id"] = "renamed"
        self.flush()
        self.assert_reports("model_sha256")

    def test_gross_residual_changed(self) -> None:
        self.stage["p1"]["gates"]["gross"]["residual_over_span"] = 0.75
        self.flush()
        self.assert_reports("gross_residual")

    def test_covered_item_text_changed(self) -> None:
        write(self.candidates, {"what_no_candidate_here_predicts": [ITEM + " (revised)"]})
        self.assert_reports("covers")

    def test_missing_archive_stops(self) -> None:
        self.archive = self.archive / "absent"
        code, _ = self.run_tool()
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
