"""Regenerate or check the tracked capability catalog from a shared library."""

from __future__ import annotations

import argparse
import ctypes
import json
import sys
from pathlib import Path
from typing import Any

PARAMETER_UNITS = {
    "dB",
    "dBFS",
    "LUFS",
    "Hz",
    "ms",
    "s",
    "samples",
    "m",
    "cm",
    "deg",
    "percent",
    "degC",
    "V",
    "inPerSec",
    "dBPerOct",
    "semitones",
    "cents",
    "ratio",
    "bits",
    "count",
    "none",
}
DEPENDENCY_RELATIONS = {"lt", "le", "gt", "ge"}
PRESET_GROUPS = ("mastering", "synth", "mixingScene", "voiceChanger", "playbackRoom")
MASTERING_PRESET_KEYS = {
    "name",
    "kind",
    "targetLufs",
    "truePeakCeilingDb",
    "maxLimiterGainReductionDb",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--library", type=Path, required=True, help="path to libsonare shared library"
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("tools/capability-catalog.json"),
        help="tracked JSON catalog path",
    )
    parser.add_argument(
        "--check", action="store_true", help="fail instead of rewriting a stale output"
    )
    return parser.parse_args()


def require_object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(  # noqa: TRY004 -- one error class per document
            f"{path} must be an object"
        )
    return value


def require_keys(value: dict[str, Any], path: str, keys: set[str]) -> None:
    missing = keys - value.keys()
    if missing:
        raise ValueError(f"{path} is missing required keys: {', '.join(sorted(missing))}")


def optional_number(value: Any) -> bool:
    """A JSON number or null. ``bool`` is excluded: it is an ``int`` in Python."""
    return value is None or (isinstance(value, (int, float)) and not isinstance(value, bool))


def validate_choices(parameter: dict[str, Any], path: str) -> None:
    """A closed accepted set: named values in ascending order, and then no range."""
    choices = parameter["choices"]
    if choices is None:
        if parameter["type"] == "enum":
            raise ValueError(f"{path} is an enum and must publish its choices")
        return
    if parameter["type"] == "boolean":
        raise ValueError(f"{path} is boolean and must publish no choices")
    if not isinstance(choices, list) or not choices:
        raise ValueError(f"{path}.choices must be a non-empty array or null")
    values: list[float] = []
    for choice_index, choice_value in enumerate(choices):
        choice_path = f"{path}.choices[{choice_index}]"
        choice = require_object(choice_value, choice_path)
        require_keys(choice, choice_path, {"name", "value"})
        if not isinstance(choice["name"], str) or not choice["name"]:
            raise ValueError(f"{choice_path}.name must be a non-empty string")
        if choice["value"] is None or not optional_number(choice["value"]):
            raise ValueError(f"{choice_path}.value must be a number")
        values.append(choice["value"])
    if values != sorted(set(values)):
        raise ValueError(f"{path}.choices must be in strictly ascending value order")
    if len({choice["name"] for choice in choices}) != len(choices):
        raise ValueError(f"{path}.choices must carry unique names")
    if parameter["min"] is not None or parameter["max"] is not None:
        raise ValueError(f"{path} publishes choices and must publish no bounds")


def validate_parameter(parameter: dict[str, Any], path: str) -> None:
    """Guard the host-facing shape of one parameter descriptor.

    Coverage — that a construction key actually carries a default, and that a
    published bound is the bound validation enforces — is checked in the C++
    suite, where the config structs and the construction path are both visible.
    What can only be checked here is that the JSON says what the `.pyi` / `.d.ts`
    mirrors promise a host it says.
    """
    if parameter["type"] not in {"number", "boolean", "enum", "string", "array"}:
        raise ValueError(f"{path}.type must be number, boolean, enum, string or array")
    identifier = parameter["id"]
    if identifier is not None and (
        not isinstance(identifier, int) or isinstance(identifier, bool) or identifier < 0
    ):
        raise ValueError(f"{path}.id must be a non-negative integer or null")
    if identifier is None and parameter["rtSafe"] is not False:
        raise ValueError(f"{path} has no id and so cannot be rtSafe")
    validate_choices(parameter, path)
    if parameter["type"] in {"string", "array"}:
        # Carried on the JSON side-channel: nothing numeric to publish.
        for field in ("min", "max", "default", "choices"):
            if parameter[field] is not None:
                raise ValueError(f"{path} is a {parameter['type']} and must publish no {field}")
        if parameter["id"] is not None:
            raise ValueError(f"{path} is a {parameter['type']} and cannot be automated")
    for bound in ("min", "max"):
        if not optional_number(parameter[bound]):
            raise ValueError(f"{path}.{bound} must be a number or null")
    for bound in ("min", "max"):
        exclusive = parameter[f"{bound}Exclusive"]
        if not isinstance(exclusive, bool):
            raise TypeError(f"{path}.{bound}Exclusive must be a boolean")
        if exclusive and parameter[bound] is None:
            raise ValueError(f"{path}.{bound}Exclusive is set without a {bound}")
    if parameter["maxRelativeTo"] not in {None, "nyquist"}:
        raise ValueError(f"{path}.maxRelativeTo must be null or 'nyquist'")
    if parameter["maxRelativeTo"] is not None and not parameter["maxExclusive"]:
        raise ValueError(f"{path} has a rate-relative max and so must be exclusive")
    if (
        parameter["min"] is not None
        and parameter["max"] is not None
        and parameter["min"] > parameter["max"]
    ):
        raise ValueError(f"{path} publishes a min above its max")
    if parameter["type"] == "boolean":
        # A boolean carries a JSON boolean default and no range: it cannot be out
        # of range, and 0/1 would read as a number to every typed facade.
        if parameter["default"] is not None and not isinstance(parameter["default"], bool):
            raise ValueError(f"{path}.default must be a JSON boolean for a boolean param")
        if parameter["min"] is not None or parameter["max"] is not None:
            raise ValueError(f"{path} is boolean and must publish no bounds")
    elif not optional_number(parameter["default"]):
        raise ValueError(f"{path}.default must be a number or null for a number param")
    unit = parameter["unit"]
    if parameter["type"] == "number":
        # `none` is a declaration; an absent unit is a parameter nobody declared.
        if unit not in PARAMETER_UNITS:
            raise ValueError(f"{path}.unit must be one of the declared units for a number")
    elif unit is not None:
        raise ValueError(f"{path} is not a number and must publish no unit")
    if parameter["scale"] not in {"linear", "log"}:
        raise ValueError(f"{path}.scale must be linear or log")
    for bound in ("uiMin", "uiMax"):
        if not optional_number(parameter[bound]):
            raise ValueError(f"{path}.{bound} must be a number or null")
        if parameter[bound] is None:
            continue
        if parameter["min"] is not None and parameter[bound] < parameter["min"]:
            raise ValueError(f"{path}.{bound} lies below min")
        if parameter["max"] is not None and parameter[bound] > parameter["max"]:
            raise ValueError(f"{path}.{bound} lies above max")
    if (
        parameter["uiMin"] is not None
        and parameter["uiMax"] is not None
        and parameter["uiMin"] > parameter["uiMax"]
    ):
        raise ValueError(f"{path} publishes a uiMin above its uiMax")
    if not isinstance(parameter["dependsOn"], list):
        raise TypeError(f"{path}.dependsOn must be an array")
    for dependency_index, dependency_value in enumerate(parameter["dependsOn"]):
        dependency_path = f"{path}.dependsOn[{dependency_index}]"
        dependency = require_object(dependency_value, dependency_path)
        require_keys(dependency, dependency_path, {"key", "relation"})
        if not isinstance(dependency["key"], str) or not dependency["key"]:
            raise ValueError(f"{dependency_path}.key must be a non-empty string")
        if dependency["relation"] not in DEPENDENCY_RELATIONS:
            raise ValueError(f"{dependency_path}.relation must be lt, le, gt or ge")


def validate_slots(processor: dict[str, Any], path: str) -> set[str]:
    """Guard a processor's slot table and return the declared slot names.

    A parent must be declared before the slot naming it, so a host can build the
    tree in one pass.
    """
    slots = processor["slots"]
    if not isinstance(slots, list):
        raise ValueError(f"{path}.slots must be an array")  # noqa: TRY004
    if slots and not processor["realtimeInsertable"]:
        raise ValueError(f"{path} is not a realtime insert and must publish no slots")
    names: set[str] = set()
    for slot_index, slot_value in enumerate(slots):
        slot_path = f"{path}.slots[{slot_index}]"
        slot = require_object(slot_value, slot_path)
        require_keys(slot, slot_path, {"name", "parent", "activation", "minCrossoverCutoffs"})
        name = slot["name"]
        if not isinstance(name, str) or not name or name in names:
            raise ValueError(f"{slot_path}.name must be a unique non-empty string")
        parent = slot["parent"]
        if parent is not None and parent not in names:
            raise ValueError(f"{slot_path}.parent must name an earlier slot or be null")
        if slot["activation"] not in {"anyKey", "always"}:
            raise ValueError(f"{slot_path}.activation must be anyKey or always")
        cutoffs = slot["minCrossoverCutoffs"]
        if not isinstance(cutoffs, int) or isinstance(cutoffs, bool) or cutoffs < 0:
            raise ValueError(f"{slot_path}.minCrossoverCutoffs must be a non-negative integer")
        names.add(name)
    return names


def validate_mastering_presets(root: dict[str, Any]) -> None:
    """Guard `masteringPresets`: kind decides whether the three numbers are null."""
    entries = root["masteringPresets"]
    if not isinstance(entries, list):
        raise ValueError(  # noqa: TRY004 -- one error class per document
            "catalog.masteringPresets must be an array"
        )
    seen_names: list[str] = []
    for index, entry_value in enumerate(entries):
        path = f"catalog.masteringPresets[{index}]"
        entry = require_object(entry_value, path)
        require_keys(entry, path, MASTERING_PRESET_KEYS)
        if not isinstance(entry["name"], str) or not entry["name"]:
            raise ValueError(f"{path}.name must be a non-empty string")
        seen_names.append(entry["name"])
        if entry["kind"] not in {"mastering", "restoration"}:
            raise ValueError(f"{path}.kind must be 'mastering' or 'restoration'")
        is_mastering = entry["kind"] == "mastering"
        for key in ("targetLufs", "truePeakCeilingDb", "maxLimiterGainReductionDb"):
            value = entry[key]
            if is_mastering:
                if not optional_number(value) or value is None:
                    raise ValueError(f"{path}.{key} must be a number for a mastering preset")
            elif value is not None:
                raise ValueError(f"{path}.{key} must be null for a restoration preset")
    if len(seen_names) != len(set(seen_names)):
        raise ValueError("catalog.masteringPresets must contain unique names")
    mastering_names = root["presets"]["mastering"] if "presets" in root else None
    if mastering_names is not None and seen_names != list(mastering_names):
        raise ValueError("catalog.masteringPresets order must match catalog.presets.mastering")


def validate_catalog(catalog: Any) -> dict[str, Any]:
    """Guard the generated artifact's stable schema without a third-party dependency."""
    root = require_object(catalog, "catalog")
    require_keys(root, "catalog", {"version", "abi", "processors", "presets", "masteringPresets"})
    if not isinstance(root["version"], str):
        raise ValueError(  # noqa: TRY004 -- one error class per document
            "catalog.version must be a string"
        )
    abi = require_object(root["abi"], "catalog.abi")
    require_keys(abi, "catalog.abi", {"project", "engine"})
    if not all(isinstance(abi[name], int) for name in ("project", "engine")):
        raise ValueError("catalog.abi values must be integers")
    if not isinstance(root["processors"], list):
        raise ValueError(  # noqa: TRY004 -- one error class per document
            "catalog.processors must be an array"
        )
    for index, processor_value in enumerate(root["processors"]):
        processor = require_object(processor_value, f"catalog.processors[{index}]")
        require_keys(
            processor,
            f"catalog.processors[{index}]",
            {
                "id",
                "kind",
                "realtimeInsertable",
                "stereoOnly",
                "latencySamples",
                "tailSamples",
                "realtimeCost",
                "channelPolicy",
                "category",
                "causal",
                "params",
                "slots",
            },
        )
        if not isinstance(processor["causal"], bool):
            raise ValueError(f"catalog.processors[{index}].causal must be a boolean")
        if not isinstance(processor["params"], list):
            raise ValueError(  # noqa: TRY004 -- one error class per document
                f"catalog.processors[{index}].params must be an array"
            )
        realtime_cost = processor["realtimeCost"]
        if realtime_cost is not None and realtime_cost not in {"low", "moderate", "high"}:
            raise ValueError(
                f"catalog.processors[{index}].realtimeCost must be low, moderate, high, or null"
            )
        if processor["realtimeInsertable"] != (realtime_cost is not None):
            raise ValueError(
                f"catalog.processors[{index}].realtimeCost must be non-null exactly for realtime inserts"
            )
        slot_names = validate_slots(processor, f"catalog.processors[{index}]")
        for parameter_index, parameter_value in enumerate(processor["params"]):
            path = f"catalog.processors[{index}].params[{parameter_index}]"
            parameter = require_object(parameter_value, path)
            require_keys(
                parameter,
                path,
                {
                    "name",
                    "id",
                    "rtSafe",
                    "type",
                    "min",
                    "max",
                    "minExclusive",
                    "maxExclusive",
                    "maxRelativeTo",
                    "default",
                    "unit",
                    "uiMin",
                    "uiMax",
                    "scale",
                    "choices",
                    "slot",
                    "dependsOn",
                },
            )
            validate_parameter(parameter, path)
            sibling_names = {sibling["name"] for sibling in processor["params"]}
            for dependency in parameter["dependsOn"]:
                if dependency["key"] == parameter["name"] or dependency["key"] not in sibling_names:
                    raise ValueError(
                        f"{path}.dependsOn must name another parameter of the processor"
                    )
            if parameter["slot"] is not None and parameter["slot"] not in slot_names:
                raise ValueError(f"{path}.slot must name one of the processor's slots or be null")
    presets = require_object(root["presets"], "catalog.presets")
    require_keys(
        presets,
        "catalog.presets",
        set(PRESET_GROUPS),
    )
    unexpected_groups = set(presets) - set(PRESET_GROUPS)
    if unexpected_groups:
        raise ValueError(
            "catalog.presets has unexpected groups: " + ", ".join(sorted(unexpected_groups))
        )
    if not all(isinstance(presets[name], list) for name in presets):
        raise ValueError("catalog preset groups must be arrays")
    for name in PRESET_GROUPS:
        preset_names = presets[name]
        if any(not isinstance(preset_name, str) or not preset_name for preset_name in preset_names):
            raise ValueError(f"catalog.presets.{name} must contain non-empty strings")
        if len(preset_names) != len(set(preset_names)):
            raise ValueError(f"catalog.presets.{name} must contain unique names")
    validate_mastering_presets(root)
    return root


def render_catalog(library_path: Path) -> str:
    library = ctypes.CDLL(str(library_path))
    function = library.sonare_capability_catalog_json
    function.argtypes = []
    function.restype = ctypes.c_char_p
    raw = function()
    if raw is None:
        raise RuntimeError("sonare_capability_catalog_json returned null")
    catalog = validate_catalog(json.loads(raw.decode("utf-8")))
    return json.dumps(catalog, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def main() -> int:
    args = parse_args()
    if not args.library.is_file():
        print(f"shared library does not exist: {args.library}", file=sys.stderr)
        return 2
    try:
        rendered = render_catalog(args.library)
    except (
        AttributeError,
        OSError,
        RuntimeError,
        TypeError,
        UnicodeDecodeError,
        ValueError,
        json.JSONDecodeError,
    ) as exc:
        print(f"could not generate capability catalog: {exc}", file=sys.stderr)
        return 2

    if args.check:
        try:
            current = args.output.read_text(encoding="utf-8")
        except FileNotFoundError:
            print(f"capability catalog is missing: {args.output}", file=sys.stderr)
            return 1
        if current != rendered:
            print(
                f"capability catalog is stale: {args.output} (run make capability-catalog)",
                file=sys.stderr,
            )
            return 1
        print(f"capability catalog is current: {args.output}")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    print(f"wrote capability catalog: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
