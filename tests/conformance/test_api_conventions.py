"""Stdlib self-tests for the interface-convention check.

Each rule is driven with hand-built inventory records: a clean set passes, then one
planted violation must be named. The framework cases cover the two ways a rule can
go blind -- reading nothing, and a table entry that excuses nothing.
"""

from __future__ import annotations

import dataclasses
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

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


def call(surface: str, owner: str, name: str, returns: str, *param_types: str) -> dict:
    rec = method(surface, owner, name, returns=returns)
    rec["sigs"][0]["params"] = [{"name": f"p{i}", "type": t} for i, t in enumerate(param_types)]
    return rec


class CancellationRule(unittest.TestCase):
    CLEAN = [
        field("wasm", "RenderRequest", "cancel", "(() => boolean) | undefined"),
        field("wasm", "CallOptions", "signal", "AbortSignal | undefined"),
        call("wasm", "Engine", "render", "Result", "RenderRequest"),
        call(
            "wasm",
            "Client",
            "render",
            "Promise<Result>",
            "WorkerRequest<RenderRequest>",
            "CallOptions",
        ),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("cancellation", self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_promise_call_taking_cancel_is_named(self) -> None:
        planted = call("wasm", "Client", "analyze", "Promise<Result>", "RenderRequest")
        verdict = run("cancellation", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:Client.analyze"])

    def test_synchronous_call_taking_signal_is_named(self) -> None:
        planted = call("wasm", "Engine", "bounce", "Result", "CallOptions")
        verdict = run("cancellation", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:Engine.bounce"])


class TempoRangeRule(unittest.TestCase):
    CLEAN = [
        field(
            "wasm",
            "AnalyzeRequest",
            "bpmMin",
            "number | undefined",
            doc_text="Lower bound. Default `60`.",
        ),
        field(
            "node",
            "AnalyzeRequest",
            "bpmMax",
            "number | undefined",
            doc_text="Upper bound. Default 200.",
        ),
        field(
            "c",
            "SonareMusicAnalyzeOptions",
            "bpm_min",
            "float",
            doc_text="Lower bound. Default 60.",
        ),
        {
            **field("python", "", "bpm_max", "float | None"),
            "kind": "option",
            "name": "analyze(bpm_max)",
        },
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("tempo-range", self.CLEAN)
        self.assertEqual(verdict.read, 4)
        self.assertFalse(verdict.failed)

    def test_wide_default_is_named(self) -> None:
        planted = field(
            "wasm", "BeatRequest", "bpmMax", "number | undefined", doc_text="Default 300."
        )
        verdict = run("tempo-range", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:BeatRequest.bpmMax"])

    def test_python_literal_default_is_named(self) -> None:
        planted = {
            **field("python", "", "bpm_min", "float"),
            "kind": "option",
            "name": "detect_bpm(bpm_min)",
        }
        verdict = run("tempo-range", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:detect_bpm(bpm_min)"])


def function(surface: str, name: str, returns: str, *params: tuple[str, str]) -> dict:
    return {
        "surface": surface,
        "kind": "function",
        "owner": "",
        "member": "",
        "name": name,
        "sigs": [{"params": [{"name": n, "type": t} for n, t in params], "returns": returns}],
    }


class MidiModuleFunctionsRule(unittest.TestCase):
    CLEAN = [
        function("node", "midiNoteOn", "ProjectMidiEvent", ("request", "MidiNoteOnRequest")),
        function("node", "gmProgramForName", "number", ("name", "string")),
        function("node", "toJson", "string"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("midi-module-functions", self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_project_static_is_named(self) -> None:
        planted = method("wasm", "Project", "midiNoteOff")
        planted["static"] = True
        verdict = run("midi-module-functions", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:Project.midiNoteOff"])

    def test_lookup_returning_minus_one_is_named(self) -> None:
        planted = function("node", "gmDrumNoteForName", "number | -1", ("name", "string"))
        verdict = run("midi-module-functions", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["node:gmDrumNoteForName"])

    def test_positional_packer_is_named(self) -> None:
        planted = function(
            "node", "midiCc", "ProjectMidiEvent", ("channel", "number"), ("cc", "number")
        )
        verdict = run("midi-module-functions", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["node:midiCc"])


class ConstructionRule(unittest.TestCase):
    CLEAN = [
        js_class("wasm", "Mixer", param_types=["MixerConfig"]),
        {**js_class("wasm", "SonareError", param_types=["code", "message"]), "bases": ["Error"]},
        {
            **function("wasm", "Node.create", "Promise<Node>"),
            "kind": "method",
            "static": True,
            "member": "create",
        },
    ]

    def judge(self, records):
        return run("construction", records)

    def test_clean_surface_passes(self) -> None:
        verdict = self.judge(self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_positional_constructor_is_named(self) -> None:
        planted = js_class("node", "Engine", param_types=["number", "number"])
        verdict = self.judge([*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["node:Engine"])

    def test_synchronous_create_is_named(self) -> None:
        planted = {
            **function("node", "Project.create", "Project"),
            "kind": "method",
            "static": True,
            "member": "create",
        }
        verdict = self.judge([*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["node:Project.create"])

    def test_python_config_parameter_is_named(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "m.py"
            src.write_text(
                "class Good:\n    def __init__(self, *, rate: int) -> None: ...\n\nclass Bad:\n    def __init__(self, config) -> None: ...\n"
            )
            records = [
                {
                    "surface": "python",
                    "kind": "class",
                    "name": n,
                    "owner": "",
                    "member": "",
                    "file": "m.py",
                }
                for n in ("Good", "Bad")
            ]
            with mock.patch.object(check, "ROOT", Path(tmp)):
                verdict = self.judge(records)
        self.assertEqual(verdict.read, 2)
        self.assertEqual([f.key for f in verdict.violations], ["python:Bad"])


class InputTypesRule(unittest.TestCase):
    CLEAN = [
        method("wasm", "Project", "setMarker"),
        method("python", "Project", "set_marker"),
        function("node", "exposure", "number"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("input-types", self.CLEAN)
        self.assertEqual(verdict.read, 3)
        self.assertFalse(verdict.failed)

    def test_ex_method_is_named(self) -> None:
        planted = method("python", "Project", "set_marker_ex")
        verdict = run("input-types", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:Project.set_marker_ex"])


class OneInputFormRule(unittest.TestCase):
    CLEAN = [
        field("wasm", "SetClipsRequest", "clips", "readonly EngineClip[]"),
        field("wasm", "ChangerConfig", "harmonySteps", "readonly [number, number, number]"),
        field("node", "StageResult", "reports", "Report[] | [Report]"),
        field("python", "Engine", "markers", "Sequence[EngineMarker]"),
        field("python", "Project", "takes", "Sequence[Mapping[str, object]] | None"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("one-input-form", self.CLEAN)
        self.assertEqual(verdict.read, 4)
        self.assertFalse(verdict.failed)

    def test_tuple_beside_record_is_named(self) -> None:
        planted = field(
            "wasm",
            "SetEventsRequest",
            "events",
            "readonly (EngineMidiEvent | readonly [number, number])[]",
        )
        verdict = run("one-input-form", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:SetEventsRequest.events"])

    def test_python_mapping_beside_record_is_named(self) -> None:
        planted = field("python", "Engine", "clips", "Sequence[EngineClip | Mapping[str, object]]")
        verdict = run("one-input-form", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:Engine.clips"])


class AbiProbesRule(unittest.TestCase):
    CLEAN = [
        function("node", "engineAbiVersion", "number"),
        function("python", "voice_changer_abi_version", "int"),
        function("wasm", "abiVersion", "number"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("abi-probes", self.CLEAN)
        self.assertEqual(verdict.read, 3)
        self.assertFalse(verdict.failed)

    def test_bare_api_version_is_named(self) -> None:
        planted = function("wasm", "vocalEditApiVersion", "number")
        verdict = run("abi-probes", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:vocalEditApiVersion"])

    def test_feature_probe_is_named(self) -> None:
        planted = function("node", "hasFfmpegSupport", "boolean")
        verdict = run("abi-probes", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["node:hasFfmpegSupport"])

    def test_library_feature_available_probe_is_named(self) -> None:
        planted = function("python", "vocal_edit_available", "bool")
        verdict = run("abi-probes", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:vocal_edit_available"])


def stereo_fn(surface: str, name: str, *params: str, owner: str = "") -> dict:
    rec = function(surface, name, "Result", *[(p, "Float32Array") for p in params])
    rec["owner"], rec["member"] = owner, name
    return rec


class StereoPairRule(unittest.TestCase):
    CLEAN = [
        stereo_fn("node", "normalizeStereo", "left", "right"),
        stereo_fn("wasm", "normalizeStereo", "left", "right"),
        stereo_fn("python", "normalize_stereo", "left", "right"),
        stereo_fn("node", "meteringStereoCorrelation", "left", "right"),
        stereo_fn("node", "normalize", "channels"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("stereo-pair", self.CLEAN)
        self.assertEqual(verdict.read, 4)
        self.assertFalse(verdict.failed)

    def test_python_only_stereo_twin_is_named(self) -> None:
        planted = stereo_fn("python", "repair_declick_stereo", "left", "right")
        verdict = run("stereo-pair", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:repair_declick_stereo"])

    def test_left_right_outside_two_channel_algorithm_is_named(self) -> None:
        planted = stereo_fn("wasm", "denoise", "left", "right")
        verdict = run("stereo-pair", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:denoise"])

    def test_stereo_family_member_taking_channels_is_named(self) -> None:
        records = [stereo_fn(s, "repairStereo", "channels") for s in ("node", "wasm", "python")]
        verdict = run("stereo-pair", [*self.CLEAN, *records])
        self.assertEqual(len(verdict.violations), 3)


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
