"""Stdlib self-tests for the interface-convention check.

Each rule is driven with hand-built inventory records: a clean set passes, then one
planted violation must be named. The framework cases cover the two ways a rule can
go blind -- reading nothing, and a table entry that excuses nothing.
"""

from __future__ import annotations

import dataclasses
import importlib.util
import sys
import unittest
from pathlib import Path

_SPEC = importlib.util.spec_from_file_location(
    "check_api_conventions", Path(__file__).resolve().parent / "check_api_conventions.py"
)
assert _SPEC and _SPEC.loader
check = importlib.util.module_from_spec(_SPEC)
# dataclasses resolve their module through sys.modules.
sys.modules[_SPEC.name] = check
_SPEC.loader.exec_module(check)


def rule(name: str):
    return next(r for r in check.RULES if r.name == name)


def member(surface: str, owner: str, name: str, *, ts_kind: str, params=(), static=False) -> dict:
    sigs = [] if ts_kind == "accessor" else [{"params": [{"name": p} for p in params]}]
    return {
        "surface": surface,
        "kind": "method",
        "owner": owner,
        "member": name,
        "name": f"{owner}.{name}",
        "ts_kind": ts_kind,
        "sigs": sigs,
        "static": static,
    }


def js_class(surface: str, name: str, *, param_types=()) -> dict:
    params = [{"name": f"p{i}", "type": t} for i, t in enumerate(param_types)]
    return {
        "surface": surface,
        "kind": "class",
        "name": name,
        "owner": "",
        "member": "",
        "sigs": [{"params": params}],
    }


def method(surface: str, owner: str, name: str, *, returns: str = "void", params=()) -> dict:
    return {
        "surface": surface,
        "kind": "method",
        "owner": owner,
        "member": name,
        "name": f"{owner}.{name}",
        "ts_kind": "method",
        "sigs": [{"params": [{"name": p} for p in params], "returns": returns}],
        "static": False,
    }


def field(surface: str, owner: str, name: str, type_: str = "number", **extra) -> dict:
    return {
        "surface": surface,
        "kind": "field",
        "owner": owner,
        "member": name,
        "name": f"{owner}.{name}",
        "type": type_,
        **extra,
    }


def run(name: str, records: list[dict]):
    """Judge hand-built records against a rule with its exempt and pending tables emptied."""
    r = dataclasses.replace(rule(name), exempt={}, pending={})
    return check.judge(r, r.derive(records) if r.derive else records)


class GettersRule(unittest.TestCase):
    CLEAN = [
        member("node", "Project", "trackCount", ts_kind="accessor"),
        member("wasm", "Mixer", "stripNonFiniteDiscardCount", ts_kind="method", params=["strip"]),
        member("python", "Project", "track_count", ts_kind="method"),
        member("node", "Project", "toJson", ts_kind="method"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = check.judge(rule("getters"), self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_argument_less_count_method_is_named(self) -> None:
        planted = member("wasm", "Project", "soundFontPresetCount", ts_kind="method")
        verdict = check.judge(rule("getters"), [*self.CLEAN, planted])
        self.assertTrue(verdict.failed)
        self.assertEqual([f.key for f in verdict.violations], ["wasm:Project.soundFontPresetCount"])

    def test_static_count_method_is_outside_the_rule(self) -> None:
        factory = member("node", "Pool", "defaultCount", ts_kind="method", static=True)
        self.assertFalse(check.judge(rule("getters"), [*self.CLEAN, factory]).failed)


class AsyncTeardownRule(unittest.TestCase):
    CLEAN = [
        js_class("wasm", "JobClient", param_types=["JobClientOptions", "Worker"]),
        method("wasm", "JobClient", "destroy", returns="Promise<void>"),
        method("wasm", "JobClient", "[Symbol.asyncDispose]", returns="Promise<void>"),
        js_class("wasm", "NoiseProcessor", param_types=["Options"]),
        method("wasm", "NoiseProcessor", "destroy"),
        field("wasm", "JobClientOptions", "terminateWorkerOnDestroy", "boolean | undefined"),
    ]

    def judge(self, records):
        return run("async-teardown", records)

    def test_clean_surface_passes(self) -> None:
        verdict = self.judge(self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_synchronous_destroy_is_named(self) -> None:
        bad = [r for r in self.CLEAN if r["name"] != "JobClient.destroy"]
        bad.append(method("wasm", "JobClient", "destroy"))
        verdict = self.judge(bad)
        self.assertEqual([f.key for f in verdict.violations], ["wasm:JobClient"])

    def test_dispose_option_name_is_named(self) -> None:
        planted = field("wasm", "ClientOptions", "terminateWorkerOnDispose", "boolean | undefined")
        verdict = self.judge([*self.CLEAN, planted])
        self.assertEqual(
            [f.key for f in verdict.violations], ["wasm:ClientOptions.terminateWorkerOnDispose"]
        )


class LoudnessNamesRule(unittest.TestCase):
    CLEAN = [
        field("wasm", "AnalysisDynamics", "loudnessRangeLu"),
        field("wasm", "LufsResult", "momentaryLufs"),
        field("node", "MeterTelemetry", "truePeakDbtpL"),
        field("python", "LufsResult", "short_term_lufs", "float"),
        field("python", "AnalysisDynamics", "loudness_range_lu", "float"),
        field("wasm", "MixerMeterOptions", "measureTruePeak", "boolean | undefined"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("loudness-names", self.CLEAN)
        self.assertEqual(verdict.read, 5)
        self.assertFalse(verdict.failed)

    def test_unitless_loudness_range_is_named(self) -> None:
        planted = field("wasm", "LufsResult", "loudnessRange")
        verdict = run("loudness-names", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:LufsResult.loudnessRange"])

    def test_true_peak_in_decibels_is_named(self) -> None:
        planted = field("python", "Profile", "true_peak_db", "float")
        verdict = run("loudness-names", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:Profile.true_peak_db"])


class StringClosedSetsRule(unittest.TestCase):
    CLEAN = [
        field("wasm", "PanOptions", "law", "'stereoPan' | 'balance' | undefined"),
        field("wasm", "ChangerOptions", "numChannels", "1 | 2"),
        {"surface": "python", "kind": "class", "name": "Frame", "owner": "", "bases": []},
    ]
    COUNT = {"wasm:ChangerOptions.numChannels": "a count"}

    def judge(self, records):
        base = dataclasses.replace(rule("string-closed-sets"), pending={}, exempt=self.COUNT)
        return check.judge(base, records)

    def test_clean_surface_passes(self) -> None:
        verdict = self.judge(self.CLEAN)
        self.assertEqual(verdict.read, 1)
        self.assertFalse(verdict.failed)

    def test_numeric_union_is_named(self) -> None:
        planted = field("node", "Mixer", "law", "0 | 1 | 2")
        verdict = self.judge([*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["node:Mixer.law"])

    def test_int_enum_is_named(self) -> None:
        planted = {
            "surface": "python",
            "kind": "class",
            "name": "Mode",
            "owner": "",
            "bases": ["IntEnum"],
        }
        verdict = self.judge([*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:Mode"])


class Framework(unittest.TestCase):
    def test_reading_nothing_fails(self) -> None:
        verdict = check.judge(rule("getters"), [])
        self.assertEqual(verdict.read, 0)
        self.assertTrue(verdict.failed)

    def test_table_entry_that_excuses_nothing_is_stale(self) -> None:
        base = rule("getters")
        stale = check.Rule(
            base.name,
            base.summary,
            base.population,
            base.violation,
            exempt={"node:Project.trackCount": "reason"},
        )
        verdict = check.judge(stale, GettersRule.CLEAN)
        self.assertEqual(verdict.stale, ["node:Project.trackCount"])
        self.assertTrue(verdict.failed)

    def test_exempt_and_pending_entries_excuse_their_violation(self) -> None:
        base = rule("getters")
        planted = [
            member("node", "A", "xCount", ts_kind="method"),
            member("node", "B", "yCount", ts_kind="method"),
        ]
        tabled = check.Rule(
            base.name,
            base.summary,
            base.population,
            base.violation,
            exempt={"node:A.xCount": "reason"},
            pending={"node:B.yCount": "owner"},
        )
        verdict = check.judge(tabled, planted)
        self.assertFalse(verdict.failed)
        self.assertEqual(
            (verdict.excused, verdict.deferred), (["node:A.xCount"], ["node:B.yCount"])
        )


if __name__ == "__main__":
    unittest.main()
