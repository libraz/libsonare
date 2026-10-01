"""Regression checks for classic GS overlays against the current p0 graphs.

The fixtures keep only the nodes that an overlay adds, replaces, reads or anchors to.  Their ids and
the few fitted constants are copied from the archive p0 models named beside each
fixture; the test does not import or require the archive checkout.
"""

from __future__ import annotations

import copy
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OVERLAY_DIR = ROOT / "tools" / "gs" / "classic-overlays"


def _load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CLASSIC = _load_module("classic_models_for_overlay_test", ROOT / "tools/gs/classic_models.py")
COVERAGE = _load_module("gs_coverage_for_overlay_test", ROOT / "tools/gs/coverage.py")


# Provenance for this reduced fixture:
#   01 41: ../soundings/inferences/models/
#          0141-whole-raised-sine-tremolo-on-the-wet.json
#          (inferences/roland-sc8850-01/stages/01-41.json p0)
#   02 0C: ../soundings/inferences/models/
#          0300-whole-drive-then-equaliser.json
#          (03 00 is folded to canonical 02 0C by tools/gs/coverage.py)
#   11 04: ../soundings/inferences/models/
#          1104-whole-side-by-side-pan-moves-each-pair.json
#   11 07: ../soundings/inferences/models/
#          1107-whole-side-by-side-pan-places-each-half.json
_FIXTURES = {
    "01 31": {
        "inputs": ["in_l", "in_r"],
        "outputs": {"out_l": "out_l", "out_r": "out_r"},
        "nodes": [
            {
                "id": side,
                "kind": "envelope",
                "input": "in_l" if side == "detect_l" else "in_r",
                "detector": "rms",
                "topology": "branching",
                "domain": "linear",
                "attack_ms": {
                    "value": 1.0,
                    "source": "measured",
                    "rests_on": [
                        "data/units/roland-sc8850-01/efx-orders/01-31-03-the-threshold-byte-at-the-fourth-of-four-ratio-settings.json",
                        "data/units/roland-sc8850-01/efx-orders/01-31-03-the-threshold-byte-at-the-second-of-four-ratio-settings.json",
                    ],
                    "fitted_on": [
                        ["what-a-limiters-ratio-byte-is-the-ratio-of/limiter-ratio-3", 0],
                        ["what-a-limiters-ratio-byte-is-the-ratio-of/limiter-ratio-3", 42],
                        ["what-a-limiters-ratio-byte-is-the-ratio-of/limiter-ratio-1", 0],
                    ],
                },
                "release_ms": {
                    "value": 50.0,
                    "source": "measured",
                    "rests_on": [
                        "data/units/roland-sc8850-01/efx-orders/01-31-03-the-threshold-byte-at-the-fourth-of-four-ratio-settings.json",
                        "data/units/roland-sc8850-01/efx-orders/01-31-03-the-threshold-byte-at-the-second-of-four-ratio-settings.json",
                    ],
                    "fitted_on": [
                        ["what-a-limiters-ratio-byte-is-the-ratio-of/limiter-ratio-3", 0],
                        ["what-a-limiters-ratio-byte-is-the-ratio-of/limiter-ratio-3", 42],
                        ["what-a-limiters-ratio-byte-is-the-ratio-of/limiter-ratio-1", 0],
                    ],
                },
            }
            for side in ("detect_l", "detect_r")
        ],
    },
    "01 41": {
        "inputs": ["in_l", "in_r"],
        "outputs": {"out_l": "out_l", "out_r": "out_r"},
        "nodes": [
            {"id": "line_l", "kind": "delay", "input": "in_l"},
            {"id": "line_r", "kind": "delay", "input": "in_r"},
            {"id": "dip_l", "kind": "vca", "input": "line_l", "control": "trem_l"},
            {"id": "dip_r", "kind": "vca", "input": "line_r", "control": "trem_r"},
            {
                "id": "wet_l",
                "kind": "mix",
                "inputs": ["line_l", "dip_l"],
                "weights": {"line_l": {"value": 1.0}, "dip_l": {"value": -0.6144}},
            },
            {
                "id": "wet_r",
                "kind": "mix",
                "inputs": ["line_r", "dip_r"],
                "weights": {"line_r": {"value": 1.0}, "dip_r": {"value": -0.6144}},
            },
            {"id": "out_l", "kind": "mix", "inputs": ["in_l", "wet_l"]},
            {"id": "out_r", "kind": "mix", "inputs": ["in_r", "wet_r"]},
        ],
    },
    "02 0C": {
        "inputs": ["in_l", "in_r"],
        "outputs": {"out_l": "rt_sum_l", "out_r": "rt_sum_r"},
        "nodes": [
            {"id": "rt_sum_l", "kind": "mix", "inputs": ["rt_low_level_l", "rt_high_level_l"]},
            {"id": "rt_sum_r", "kind": "mix", "inputs": ["rt_low_level_r", "rt_high_level_r"]},
            # The acceleration nodes below are present in this p0 graph.  Their
            # fields are irrelevant to the overlay collision check; identity is
            # the part of the graph that apply_overlay validates here.
            *[
                {"id": node_id, "kind": "mix"}
                for node_id in (
                    "rt_low_gap_hz",
                    "rt_low_slow_short",
                    "rt_low_slow_climb",
                    "rt_low_slow_floor",
                    "rt_low_slow_over",
                    "rt_low_slow_past",
                    "rt_low_slow_rate",
                    "rt_low_fast_short",
                    "rt_low_fast_climb",
                    "rt_low_fast_floor",
                    "rt_low_fast_over",
                    "rt_low_fast_past",
                    "rt_low_fast_rate",
                    "rt_low_rate",
                    "rt_high_gap_hz",
                    "rt_high_slow_short",
                    "rt_high_slow_climb",
                    "rt_high_slow_floor",
                    "rt_high_slow_over",
                    "rt_high_slow_past",
                    "rt_high_slow_rate",
                    "rt_high_fast_short",
                    "rt_high_fast_climb",
                    "rt_high_fast_floor",
                    "rt_high_fast_over",
                    "rt_high_fast_past",
                    "rt_high_fast_rate",
                    "rt_high_rate",
                )
            ],
        ],
    },
    "11 04": {
        "inputs": ["in_l", "in_r"],
        "outputs": {"out_l": "b_sum_l", "out_r": "b_sum_r"},
        "nodes": [
            {"id": "a_sel", "kind": "mix", "inputs": ["a_od_post", "a_ds_post"]},
            {"id": "b_sum_l", "kind": "mix", "inputs": ["b_low_level_l", "b_high_level_l"]},
            {"id": "b_sum_r", "kind": "mix", "inputs": ["b_low_level_r", "b_high_level_r"]},
            *[
                {"id": node_id, "kind": "mix"}
                for node_id in (
                    "b_low_gap_hz",
                    "b_low_slow_short",
                    "b_low_slow_climb",
                    "b_low_slow_floor",
                    "b_low_slow_over",
                    "b_low_slow_past",
                    "b_low_slow_rate",
                    "b_low_fast_short",
                    "b_low_fast_climb",
                    "b_low_fast_floor",
                    "b_low_fast_over",
                    "b_low_fast_past",
                    "b_low_fast_rate",
                    "b_low_rate",
                    "b_high_gap_hz",
                    "b_high_slow_short",
                    "b_high_slow_climb",
                    "b_high_slow_floor",
                    "b_high_slow_over",
                    "b_high_slow_past",
                    "b_high_slow_rate",
                    "b_high_fast_short",
                    "b_high_fast_climb",
                    "b_high_fast_floor",
                    "b_high_fast_over",
                    "b_high_fast_past",
                    "b_high_fast_rate",
                    "b_high_rate",
                )
            ],
        ],
    },
    "11 07": {
        "inputs": ["in_l", "in_r"],
        "outputs": {"out_l": "out_l", "out_r": "out_r"},
        "nodes": [
            {"id": "sum_l", "kind": "mix", "inputs": ["a_pan_place.left", "b_pan_place.left"]},
            {"id": "sum_r", "kind": "mix", "inputs": ["a_pan_place.right", "b_pan_place.right"]},
            *[
                {"id": node_id, "kind": "mix"}
                for node_id in (
                    "b_low_gap_hz",
                    "b_low_slow_short",
                    "b_low_slow_climb",
                    "b_low_slow_floor",
                    "b_low_slow_over",
                    "b_low_slow_past",
                    "b_low_slow_rate",
                    "b_low_fast_short",
                    "b_low_fast_climb",
                    "b_low_fast_floor",
                    "b_low_fast_over",
                    "b_low_fast_past",
                    "b_low_fast_rate",
                    "b_low_rate",
                    "b_high_gap_hz",
                    "b_high_slow_short",
                    "b_high_slow_climb",
                    "b_high_slow_floor",
                    "b_high_slow_over",
                    "b_high_slow_past",
                    "b_high_slow_rate",
                    "b_high_fast_short",
                    "b_high_fast_climb",
                    "b_high_fast_floor",
                    "b_high_fast_over",
                    "b_high_fast_past",
                    "b_high_fast_rate",
                    "b_high_rate",
                )
            ],
        ],
    },
}

_ACCELERATION_SUFFIXES = (
    "gap_hz",
    "slow_short",
    "slow_climb",
    "slow_floor",
    "slow_over",
    "slow_past",
    "slow_rate",
    "fast_short",
    "fast_climb",
    "fast_floor",
    "fast_over",
    "fast_past",
    "fast_rate",
    "rate",
)
_REDUNDANT_ACCELERATION_IDS = {
    f"{prefix}_{band}_{suffix}"
    for prefix in ("rt", "b")
    for band in ("low", "high")
    for suffix in _ACCELERATION_SUFFIXES
}
_SEPARATE_IDS = {
    f"{prefix}_rotor_{part}"
    for prefix in ("rt", "b")
    for part in ("l", "r", "mid", "side_l", "side_r")
}
_POST_PLACEMENT_SEPARATE_IDS = {
    "b_sep_mid",
    "b_sep_side_l",
    "b_sep_side_r",
    "b_sep_l",
    "b_sep_r",
}
_CABINET_IDS = {
    "a_sel_curve",
    "a_cab_select",
    "a_cab_step",
    *(f"a_cab{cab}_{part}" for cab in range(4) for part in ("hp", "bump", "lp1", "lp2")),
}


def _entries(type_name: str) -> list[dict]:
    overlays, _ = CLASSIC.load_overlays(OVERLAY_DIR)
    return overlays[CLASSIC.type_number(type_name)]


class ClassicOverlayTest(unittest.TestCase):
    def test_current_overlay_keeps_cabinet_and_separate_structure(self) -> None:
        for type_name in ("02 0C", "11 04", "11 07"):
            with self.subTest(type=type_name):
                entries = _entries(type_name)
                roles = {
                    entry["node"]["id"]: "replace" if "replaces" in entry else "add"
                    for entry in entries
                }
                self.assertFalse(_REDUNDANT_ACCELERATION_IDS & roles.keys())
                if type_name == "11 04":
                    self.assertTrue(_CABINET_IDS <= roles.keys())
                    self.assertEqual(roles["a_sel"], "replace")
                expected_prefix = "rt" if type_name == "02 0C" else "b"
                separate = (
                    _POST_PLACEMENT_SEPARATE_IDS
                    if type_name == "11 07"
                    else {
                        node_id
                        for node_id in _SEPARATE_IDS
                        if node_id.startswith(expected_prefix + "_")
                    }
                )
                self.assertTrue(separate <= roles.keys())
                sum_ids = (
                    ("sum_l", "sum_r")
                    if type_name == "11 07"
                    else (f"{expected_prefix}_sum_l", f"{expected_prefix}_sum_r")
                )
                for node_id in sum_ids:
                    self.assertEqual(roles[node_id], "replace")

    def test_1107_separate_runs_after_mono_pan_placement(self) -> None:
        by_id = {entry["node"]["id"]: entry["node"] for entry in _entries("11 07")}
        self.assertEqual(by_id["b_sep_mid"]["inputs"], ["b_pan_place.left", "b_pan_place.right"])
        self.assertEqual(by_id["b_sep_side_l"]["inputs"], ["b_pan_place.left", "b_pan_place.right"])
        self.assertEqual(by_id["b_sep_side_r"]["inputs"], ["b_pan_place.left", "b_pan_place.right"])
        self.assertEqual(by_id["sum_l"]["inputs"], ["a_pan_place.left", "b_sep_l"])
        self.assertEqual(by_id["sum_r"]["inputs"], ["a_pan_place.right", "b_sep_r"])
        self.assertEqual(
            by_id["b_sep_l"]["weights"]["b_sep_side_l"]["anchor"],
            "b_sum_l.weights.b_low_level_l",
        )
        self.assertEqual(
            by_id["b_sep_r"]["weights"]["b_sep_side_r"]["anchor"],
            "b_sum_r.weights.b_low_level_r",
        )

    def test_overlays_apply_to_selected_p0_nodes_without_add_collisions(self) -> None:
        for type_name, fixture in _FIXTURES.items():
            with self.subTest(type=type_name):
                try:
                    CLASSIC.apply_overlay(copy.deepcopy(fixture), _entries(type_name))
                except SystemExit as exc:
                    self.fail(f"{type_name} overlay does not match its current p0 fixture: {exc}")

    def test_0141_tremolo_dips_wet_lines_before_output_balance(self) -> None:
        entries = _entries("01 41")
        by_id = {entry["node"]["id"]: entry["node"] for entry in entries}

        self.assertEqual(
            by_id["trem_spread_l"]["weights"]["trem_side_l"]["anchor"], "wet_l.weights.line_l"
        )
        self.assertEqual(
            by_id["trem_spread_r"]["weights"]["trem_side_r"]["anchor"], "wet_r.weights.line_r"
        )
        self.assertEqual(by_id["dip_l"]["input"], "line_l")
        self.assertEqual(by_id["dip_r"]["input"], "line_r")
        self.assertEqual(CLASSIC._fitted_constant(_FIXTURES["01 41"], "wet_l.weights.line_l"), 1.0)
        self.assertEqual(CLASSIC._fitted_constant(_FIXTURES["01 41"], "wet_r.weights.line_r"), 1.0)

    def test_0141_laws_are_anchored_at_power_on_unity(self) -> None:
        number = CLASSIC.type_number("01 41")
        overlays = {number: copy.deepcopy(_entries("01 41"))}
        CLASSIC.resolve_overlay_laws(
            COVERAGE,
            overlays,
            {"01 41": {5: "00–7F"}},
            {number: [16, 8, 40, 40, 60, 96] + [0] * 14},
            {number: _FIXTURES["01 41"]},
        )
        by_id = {entry["node"]["id"]: entry["node"] for entry in overlays[number]}
        for node_id, ref in (("trem_spread_l", "trem_side_l"), ("trem_spread_r", "trem_side_r")):
            with self.subTest(node=node_id):
                entries = by_id[node_id]["weights"][ref]
                self.assertEqual(entries["map"]["entries"][96], 1.0)

    def test_0131_overlay_preserves_the_current_rms_fit_except_release(self) -> None:
        number = CLASSIC.type_number("01 31")
        p0 = copy.deepcopy(_FIXTURES["01 31"])
        original = {node["id"]: node for node in p0["nodes"]}
        overlays = {number: copy.deepcopy(_entries("01 31"))}
        CLASSIC.resolve_overlay_laws(
            COVERAGE,
            overlays,
            {"01 31": {2: "00–7F"}},
            {number: [0] * CLASSIC.SLOTS},
            {number: p0},
        )
        applied = CLASSIC.apply_overlay(p0, overlays[number])
        by_id = {node["id"]: node for node in applied["nodes"]}
        for node_id, expected in original.items():
            with self.subTest(node=node_id):
                actual = by_id[node_id]
                expected_without_release = {k: v for k, v in expected.items() if k != "release_ms"}
                actual_without_release = {k: v for k, v in actual.items() if k != "release_ms"}
                self.assertEqual(actual_without_release, expected_without_release)
                self.assertEqual(actual["release_ms"]["byte"], "40 03 05")
                self.assertEqual(actual["release_ms"]["law"], "d.release_ms")
                self.assertEqual(actual["release_ms"]["map"]["entries"][0], 50.0)


if __name__ == "__main__":
    unittest.main()
