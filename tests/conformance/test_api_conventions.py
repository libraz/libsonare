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
from typing import ClassVar
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


def interface(surface: str, name: str) -> dict:
    return {
        "surface": surface,
        "kind": "type",
        "ts_kind": "interface",
        "name": name,
        "owner": "",
        "member": "",
    }


class InterfaceTeardownRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        interface("wasm", "MicrophoneInputBinding"),
        method("wasm", "MicrophoneInputBinding", "destroy"),
        method("wasm", "MicrophoneInputBinding", "close"),
        interface("wasm", "WebMidiBinding"),
        method("wasm", "WebMidiBinding", "destroy"),
        interface("wasm", "OfflineWorker"),
        method("wasm", "OfflineWorker", "terminate"),
        interface("wasm", "VocalRenderJob"),
        method("wasm", "VocalRenderJob", "abort"),
        interface("wasm", "VocalEditDraft"),
        method("wasm", "VocalEditDraft", "cancel"),
        interface("wasm", "RenderRequest"),
        field("wasm", "RenderRequest", "cancel", "boolean"),
        field("wasm", "RenderRequest", "free", "number"),
        interface("wasm", "Midi2PerNoteManagementRequest"),
        field("wasm", "Midi2PerNoteManagementRequest", "detach", "boolean"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("interface-teardown", self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_interface_released_without_destroy_is_named(self) -> None:
        planted = [
            interface("wasm", "OpfsClipPageProviderBinding"),
            method("wasm", "OpfsClipPageProviderBinding", "close"),
            interface("node", "ClipPageProviderBinding"),
            field("node", "ClipPageProviderBinding", "release", "() => void"),
        ]
        verdict = run("interface-teardown", [*self.CLEAN, *planted])
        self.assertEqual(
            sorted(f.key for f in verdict.violations),
            ["node:ClipPageProviderBinding", "wasm:OpfsClipPageProviderBinding"],
        )


class DocumentKeyScreenRule(unittest.TestCase):
    RECORDS: ClassVar[list[dict]] = [
        field("node", "RendererDoc", "input", "string"),
        field("node", "RendererDoc", "target", "string"),
        field("wasm", "RendererDoc", "input", "string"),
        field("wasm", "RendererDoc", "target", "string"),
        field("node", "RenderRequest", "doc", "RendererDoc"),
    ]

    def judge(self, node_keys: str, wasm_keys: str, facade: str = ""):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tools/api").mkdir(parents=True)
            (root / "tools/api/document_types.toml").write_text(
                '[[document]]\ntype = "RendererDoc"\n'
            )
            for surface, text in (("node", node_keys), ("wasm", wasm_keys)):
                (root / "bindings" / surface / "src").mkdir(parents=True)
                (root / "bindings" / surface / "src/request_keys.ts").write_text(text)
            (root / "bindings/node/src/renderer.ts").write_text(facade)
            with mock.patch.object(check, "ROOT", root):
                return run("document-key-screen", self.RECORDS)

    def test_request_object_screens_pass(self) -> None:
        verdict = self.judge(
            "const RENDER_KEYS = keySet('input', 'target', 'onProgress');\n",
            "const RenderRequestKeys = keysOf<RenderRequest>()(['input', 'target']);\n",
        )
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_screen_over_a_document_is_named(self) -> None:
        verdict = self.judge(
            "const RENDERER_DOC_KEYS = keySet('target', 'input');\n",
            "const RendererDocKeys = keysOf<RendererDoc>()(['input', 'target']);\n",
        )
        self.assertEqual(
            sorted(f.key for f in verdict.violations),
            ["node:RENDERER_DOC_KEYS", "wasm:RendererDocKeys"],
        )

    READERS = """
function docJson(fnName: string, config: RendererDoc): string {
  if (typeof config !== 'object' || config === null) {
    throw new TypeError(`${fnName}: config must be an object`);
  }
  return JSON.stringify(config);
}

export function render(request: RenderRequest): void {
  if (request.doc === undefined || request.doc === null) {
    throw new TypeError('render: give a doc');
  }
  native(docJson('render', request.doc));
}
"""

    def test_a_pass_through_reader_passes(self) -> None:
        verdict = self.judge("", "", self.READERS)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_a_reader_that_refuses_a_value_or_walks_keys_is_named(self) -> None:
        planted = """
function layoutOf(config: RendererDoc): number {
  if (config.target.layout === 'odd') {
    throw new RangeError('target.layout must be stereo');
  }
  return 2;
}

function flatten(config: RendererDoc): string[] {
  return Object.keys(config);
}

export function apply(request: RenderRequest): void {
  if (request.doc.length > 4) {
    throw new RangeError('apply: too many entries');
  }
}
"""
        verdict = self.judge("", "", self.READERS + planted)
        self.assertEqual(
            sorted(f.key for f in verdict.violations),
            ["node:renderer.apply", "node:renderer.flatten", "node:renderer.layoutOf"],
        )


class GettersRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
        field("wasm", "PanOptions", "law", "'stereoPan' | 'balance' | undefined"),
        field("wasm", "ChangerOptions", "numChannels", "1 | 2"),
        {"surface": "python", "kind": "class", "name": "Frame", "owner": "", "bases": []},
    ]
    COUNT: ClassVar[dict[str, str]] = {"wasm:ChangerOptions.numChannels": "a count"}

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
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
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

    def test_python_document_parameter_is_exempt_by_type(self) -> None:
        classes = {
            "Aliased": "config: Doc | None = None",
            "AliasedText": "config: Doc | str",
            "BareMapping": "config: Mapping[str, Any] | str",
            "BareOptions": "options: dict[str, Any]",
            "AliasedOptions": "options: Doc",
            "Record": "config: StreamConfig",
            "Text": "config: str",
            "Untyped": "config",
            "PresetOrDoc": "config: PresetId | Doc = 'neutral'",
            "PresetOnly": "config: PresetId",
            "InlineIds": "config: Literal['a', 'b'] | None = None",
            "PresetOrText": "config: PresetId | str",
            "PresetOrBag": "config: PresetId | dict[str, Any]",
            "OptionsDict": "options: dict",
            "SetterRecord": "config: PresetId | Doc | StreamConfig = 'neutral'",
        }
        setters = {"SetterRecord": "config: PresetId | StreamConfig"}
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "m.py"
            src.write_text(
                "Doc = dict[str, Any]\nPresetId = Literal['neutral', 'bright']\n"
                + "".join(
                    f"\nclass {n}:\n    def __init__(self, {p}) -> None: ...\n"
                    + (
                        f"    def set_config(self, {setters[n]}) -> None: ...\n"
                        if n in setters
                        else ""
                    )
                    for n, p in classes.items()
                )
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
                for n in classes
            ]
            with (
                mock.patch.object(check, "ROOT", Path(tmp)),
                mock.patch.object(check, "_PY_PACKAGE", Path(tmp)),
            ):
                verdict = self.judge(records)
        self.assertEqual(verdict.read, 15)
        self.assertEqual(
            sorted(f.key for f in verdict.violations),
            [
                "python:AliasedOptions",
                "python:AliasedText",
                "python:BareMapping",
                "python:BareOptions",
                "python:OptionsDict",
                "python:PresetOrBag",
                "python:PresetOrText",
                "python:Record",
                "python:Text",
                "python:Untyped",
            ],
        )


class InputTypesRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
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
    CLEAN: ClassVar[list[dict]] = [
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


def ref_param(surface: str, owner: str, member: str, type_: str) -> dict:
    return {
        "surface": surface,
        "kind": "option",
        "owner": owner,
        "member": member,
        "name": f"{owner}({member})",
        "type": type_,
    }


class StripRefRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        ref_param("node", "Mixer.setFaderDb", "strip", "StripRef"),
        ref_param("node", "Mixer.busMeter", "busId", "string"),
        ref_param("python", "Mixer.bus_meter", "bus_id", "str"),
        ref_param("node", "Engine.setBusGainDb", "busId", "number"),
        ref_param("node", "Engine.setBusPan", "busId", "number"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("strip-ref", self.CLEAN)
        self.assertEqual(verdict.read, 5)
        self.assertFalse(verdict.failed)

    def test_plain_strip_parameter_is_named(self) -> None:
        planted = ref_param("wasm", "Mixer.setPan", "strip", "number")
        verdict = run("strip-ref", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:Mixer.setPan(strip)"])

    def test_lone_differently_typed_bus_is_named(self) -> None:
        planted = ref_param("node", "Engine.setBusWidth", "busId", "string")
        verdict = run("strip-ref", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["node:Engine.setBusWidth(busId)"])

    def test_bus_id_whose_null_names_the_master_mix_passes(self) -> None:
        planted = [
            ref_param("node", "Engine.setTrackOutputBus", "busId", "number | null"),
            ref_param("python", "Engine.set_track_output_bus", "bus_id", "int | None"),
            ref_param("python", "Engine.set_bus_gain_db", "bus_id", "int"),
        ]
        verdict = run("strip-ref", [*self.CLEAN, *planted])
        self.assertEqual(verdict.read, 8)
        self.assertFalse(verdict.failed)

    def test_bus_kind_differing_across_surfaces_is_named(self) -> None:
        planted = ref_param("python", "Engine.set_bus_gain_db", "bus_id", "str")
        verdict = run("strip-ref", [*self.CLEAN, planted])
        self.assertIn("python:Engine.set_bus_gain_db(bus_id)", [f.key for f in verdict.violations])

    def test_id_of_a_strip_being_created_is_exempt_by_position(self) -> None:
        def method_with(owner: str, *params: str) -> dict:
            return {
                "surface": "python",
                "kind": "method",
                "name": owner,
                "sigs": [{"params": [{"name": p} for p in params]}],
            }

        records = [
            *self.CLEAN,
            method_with("Mixer.add_strip", "strip_id", "enabled"),
            ref_param("python", "Mixer.add_strip", "strip_id", "str"),
            method_with("Mixer.add_send", "source", "strip_id"),
            ref_param("python", "Mixer.add_send", "strip_id", "str"),
            method_with("Mixer.set_strip", "strip_id"),
            ref_param("python", "Mixer.set_strip", "strip_id", "str"),
        ]
        verdict = run("strip-ref", records)
        self.assertEqual(
            sorted(f.key for f in verdict.violations),
            ["python:Mixer.add_send(strip_id)", "python:Mixer.set_strip(strip_id)"],
        )


def keyed_field(surface: str, owner: str, member: str, kind: str = "field") -> dict:
    return {
        "surface": surface,
        "kind": kind,
        "owner": owner,
        "member": member,
        "name": f"{owner}.{member}",
        "type": "number",
    }


class PayloadChannelsRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        keyed_field("node", "LoopDesc", "audioChannels"),
        keyed_field("node", "LoopDesc", "audioSampleRate"),
        keyed_field("node", "EngineConfig", "maxChannels"),
        keyed_field("wasm", "EngineOptions", "offlineNumChannels"),
        keyed_field("node", "Bounce", "numChannels"),
        keyed_field("python", "Loop", "audio_channels"),
        keyed_field("python", "Loop", "audio_sample_rate"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("payload-channels", self.CLEAN)
        self.assertEqual(verdict.read, 4)
        self.assertFalse(verdict.failed)

    def test_payload_count_without_rate_is_named(self) -> None:
        planted = keyed_field("wasm", "ImportDesc", "sourceChannels")
        verdict = run("payload-channels", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:ImportDesc.sourceChannels"])

    def test_python_payload_count_without_rate_is_named(self) -> None:
        planted = keyed_field("python", "Import", "source_channels")
        verdict = run("payload-channels", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:Import.source_channels"])


class TypeNamesRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        {"surface": "wasm", "kind": "type", "name": "Capabilities", "owner": "", "member": ""},
        {"surface": "python", "kind": "class", "name": "Audio", "owner": "", "member": ""},
    ]

    def judge(self, records):
        base = dataclasses.replace(
            rule("type-names"),
            pending={},
            exempt={"wasm:SonareError": "the error class"},
        )
        return check.judge(base, records)

    def test_clean_surface_passes(self) -> None:
        planted_error = {
            "surface": "wasm",
            "kind": "class",
            "name": "SonareError",
            "owner": "",
            "member": "",
        }
        verdict = self.judge([*self.CLEAN, planted_error])
        self.assertEqual(verdict.read, 3)
        self.assertFalse(verdict.failed)

    def test_prefixed_type_is_named(self) -> None:
        planted = {
            "surface": "wasm",
            "kind": "type",
            "name": "SonareCapabilities",
            "owner": "",
            "member": "",
        }
        verdict = self.judge([*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:SonareCapabilities"])

    def test_companion_of_a_registered_class_keeps_the_prefix(self) -> None:
        def record(surface: str, name: str) -> dict:
            return {"surface": surface, "kind": "type", "name": name, "owner": "", "member": ""}

        planted = [
            record("wasm", "SonareEngine"),
            record("wasm", "SonareEngineOptions"),
            record("wasm", "SonareStreamAnalyzerNodeOptions"),
            record("node", "SonareEngineOptions"),
            record("wasm", "SonareEngineeringNotes"),
            record("wasm", "SonareMixerOptions"),
        ]
        verdict = self.judge([*self.CLEAN, *planted])
        self.assertEqual(
            sorted(f.key for f in verdict.violations),
            [
                "node:SonareEngineOptions",
                "wasm:SonareEngine",
                "wasm:SonareEngineeringNotes",
                "wasm:SonareMixerOptions",
            ],
        )


class EngineCounterpartsRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        method("python", "RealtimeEngine", "set_strip_pan"),
        method("node", "RealtimeEngine", "setStripPan"),
        method("python", "RealtimeEngine", "set_bus_eq_band"),
        method("node", "RealtimeEngine", "setBusEqBand"),
        method("python", "RealtimeEngine", "_native"),
        method("python", "Mixer", "set_track_strip_pan"),
    ]

    def test_matching_names_pass(self) -> None:
        verdict = run("engine-counterparts", self.CLEAN)
        self.assertEqual(verdict.read, 4)
        self.assertFalse(verdict.failed)

    def test_a_name_without_its_counterpart_is_named_on_both_sides(self) -> None:
        planted = [
            method("python", "RealtimeEngine", "set_track_strip_pan_law"),
            method("node", "RealtimeEngine", "setStripPanLaw"),
        ]
        verdict = run("engine-counterparts", [*self.CLEAN, *planted])
        self.assertEqual(
            sorted(f.key for f in verdict.violations),
            [
                "node:RealtimeEngine.setStripPanLaw",
                "python:RealtimeEngine.set_track_strip_pan_law",
            ],
        )


class EngineVocabularyRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        field("wasm", "EngineTelemetryRecord", "graphLatencySamples"),
        field("python", "Telemetry", "graph_latency_samples"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("engine-vocabulary", self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_q8_name_is_named(self) -> None:
        planted = field("python", "Telemetry", "graph_latency_samples_q8", "int")
        verdict = run("engine-vocabulary", [*self.CLEAN, planted])
        self.assertEqual(
            [f.key for f in verdict.violations], ["python:Telemetry.graph_latency_samples_q8"]
        )


class DerivedSummaryRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        field("node", "BounceResult", "channels", "Float32Array[]"),
        field("node", "BounceResult", "sampleRate"),
        field("node", "RenderResult", "numChannels"),
        field("python", "TakeResult", "takes", "list[Take]"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("derived-summary", self.CLEAN)
        self.assertEqual(verdict.read, 1)
        self.assertFalse(verdict.failed)

    def test_count_beside_its_list_is_named(self) -> None:
        planted = field("node", "BounceResult", "numChannels")
        verdict = run("derived-summary", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["node:BounceResult.numChannels"])

    def test_python_count_beside_its_list_is_named(self) -> None:
        planted = field("python", "TakeResult", "take_count", "int")
        verdict = run("derived-summary", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:TakeResult.take_count"])


class CollectionsRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        method("node", "Project", "markers"),
        method("node", "Mixer", "stripCount"),
        method("python", "Mixer", "strip_count"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("collections", self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_count_and_index_pair_is_named(self) -> None:
        planted = [
            method("wasm", "Project", "markerCount"),
            method("wasm", "Project", "markerByIndex", params=["i"]),
        ]
        verdict = run("collections", [*self.CLEAN, *planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:Project.markerCount"])

    def test_python_count_and_index_pair_is_named(self) -> None:
        planted = [
            method("python", "Project", "track_count"),
            method("python", "Project", "track_by_index", params=["i"]),
        ]
        verdict = run("collections", [*self.CLEAN, *planted])
        self.assertEqual([f.key for f in verdict.violations], ["python:Project.track_count"])


class ErrorClassByCauseRule(unittest.TestCase):
    def tree(self, wasm_source: str) -> Path:
        root = Path(self._tmp.name)
        for surface, text in (("node", "throw new RangeError('bad');\n"), ("wasm", wasm_source)):
            path = root / "bindings" / surface / "src" / "m.ts"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        return root

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)

    def judge(self, wasm_source: str):
        r = dataclasses.replace(rule("error-class-by-cause"), pending={}, exempt={})
        return check.judge(r, r.scan(self.tree(wasm_source)))

    def test_clean_tree_passes(self) -> None:
        verdict = self.judge(
            "throw new SonareError(1, 'x', {\n  cause: new Error('why'),\n});\n// throw new Error('doc');\n"
        )
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_plain_error_is_named(self) -> None:
        verdict = self.judge("const a = 1;\nthrow new Error('short write');\n")
        self.assertEqual([f.key for f in verdict.violations], ["wasm:bindings/wasm/src/m.ts:2"])


class WorkletRenderQuantumRule(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)

    def judge(self, source: str):
        root = Path(self._tmp.name)
        path = root / "bindings" / "wasm" / "src" / "worklet" / "m.ts"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source)
        r = dataclasses.replace(rule("worklet-render-quantum"), pending={}, exempt={})
        return check.judge(r, r.scan(root))

    def test_clean_tree_passes(self) -> None:
        verdict = self.judge(
            "export const AUDIO_WORKLET_RENDER_QUANTUM = 128;\n"
            "const ring = requireIntegerOption(options.commandRingCapacity,\n  128,\n  'x');\n"
            "// a render quantum of 128\n"
        )
        self.assertEqual(verdict.read, 1)
        self.assertFalse(verdict.failed)

    def test_bare_quantum_is_named(self) -> None:
        verdict = self.judge("const size = options.maxBlockSize ?? 128;\n")
        self.assertEqual(
            [f.key for f in verdict.violations], ["wasm:bindings/wasm/src/worklet/m.ts:1"]
        )


class Matrix2dRule(unittest.TestCase):
    CLEAN: ClassVar[list[dict]] = [
        field("node", "StftResult", "magnitude", "Matrix2D"),
        field("node", "StftResult", "nFft"),
        field("node", "Matrix2D", "data", "Float32Array"),
        field("node", "Matrix2D", "rows"),
        field("node", "Matrix2D", "cols"),
    ]

    def test_clean_surface_passes(self) -> None:
        verdict = run("matrix-2d", self.CLEAN)
        self.assertEqual(verdict.read, 2)
        self.assertFalse(verdict.failed)

    def test_dimension_field_on_result_is_named(self) -> None:
        planted = field("wasm", "MfccResult", "nMels")
        verdict = run("matrix-2d", [*self.CLEAN, planted])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:MfccResult.nMels"])

    def test_ad_hoc_matrix_shape_is_named(self) -> None:
        shape = [
            field("wasm", "Grid", "data", "Float32Array"),
            field("wasm", "Grid", "rows"),
            field("wasm", "Grid", "cols"),
        ]
        verdict = run("matrix-2d", [*self.CLEAN, *shape])
        self.assertEqual([f.key for f in verdict.violations], ["wasm:Grid"])


class CErrorPathRule(unittest.TestCase):
    GOOD = (
        "static void reject(const char* m) { set_last_error(SONARE_ERROR_INVALID_PARAMETER, m); }\n"
        "SonareError sonare_good(int* out) {\n"
        "  SONARE_C_API_ENTRY;\n"
        '  if (!out) { set_last_error(SONARE_ERROR_INVALID_PARAMETER, "null"); return SONARE_ERROR_INVALID_PARAMETER; }\n'
        '  if (*out < 0) { reject("negative"); return SONARE_ERROR_INVALID_PARAMETER; }\n'
        "  return SONARE_OK;\n"
        "}\n"
    )
    BAD = (
        "SonareError sonare_bad(int* out) {\n"
        "  SONARE_C_API_ENTRY;\n"
        '  if (*out > 9) { set_last_error(SONARE_ERROR_INVALID_PARAMETER, "big"); }\n'
        "  if (!out) return SONARE_ERROR_INVALID_PARAMETER;\n"
        "  return SONARE_OK;\n"
        "}\n"
    )

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)

    def judge(self, held: dict[str, int | None] | None = None, **files: str):
        root = Path(self._tmp.name)
        (root / "src" / "c_api").mkdir(parents=True)
        for name, text in files.items():
            (root / "src" / "c_api" / f"{name}.cpp").write_text(text)
        held = held or {}
        r = dataclasses.replace(
            rule("c-error-path"),
            pending={key: "awaits a fix" for key in held},
            pending_size={key: n for key, n in held.items() if n is not None},
            exempt={},
        )
        return check.judge(r, r.scan(root))

    def test_clean_tree_passes(self) -> None:
        verdict = self.judge(good=self.GOOD)
        self.assertEqual(verdict.read, 1)
        self.assertFalse(verdict.failed)

    def test_return_without_message_is_named(self) -> None:
        verdict = self.judge(good=self.GOOD, bad=self.BAD)
        self.assertEqual([f.key for f in verdict.violations], ["c:src/c_api/bad.cpp"])
        self.assertIn("sonare_bad:4", verdict.violations[0].detail)

    def test_pending_entry_holding_the_measured_count_defers(self) -> None:
        verdict = self.judge({"c:src/c_api/bad.cpp": 1}, bad=self.BAD)
        self.assertEqual(verdict.deferred, ["c:src/c_api/bad.cpp"])
        self.assertFalse(verdict.failed)

    def test_pending_count_above_the_source_fails_until_lowered(self) -> None:
        verdict = self.judge({"c:src/c_api/bad.cpp": 2}, bad=self.BAD)
        self.assertEqual([f.key for f in verdict.violations], ["c:src/c_api/bad.cpp"])
        self.assertIn("holds 2 sites and the source has 1", verdict.violations[0].detail)

    def test_new_site_in_a_pending_file_fails(self) -> None:
        grown = self.BAD.replace(
            "  return SONARE_OK;",
            "  if (*out == 3) return SONARE_ERROR_INVALID_PARAMETER;\n  return SONARE_OK;",
        )
        verdict = self.judge({"c:src/c_api/bad.cpp": 1}, bad=grown)
        self.assertEqual([f.key for f in verdict.violations], ["c:src/c_api/bad.cpp"])
        self.assertIn("source has 2", verdict.violations[0].detail)

    def test_pending_entry_without_a_count_fails(self) -> None:
        verdict = self.judge({"c:src/c_api/bad.cpp": None}, bad=self.BAD)
        self.assertTrue(verdict.failed)


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
