#!/usr/bin/env python3
"""Regenerate or check the tracked capability catalog from a shared library."""

from __future__ import annotations

import argparse
import ctypes
import json
from pathlib import Path
import re
import sys
from typing import Any


README_DEFAULT_COUNT_PATTERNS = {
    Path("README.md"): r"(\d+) distinct named DSP processors",
    Path("README_ja.md"): r"(\d+) 個の個別の名前付き DSP プロセッサ",
    Path("bindings/node/README.md"): r"(\d+) named DSP processors",
    Path(
        "bindings/wasm/README.md"
    ): r"(\d+) named mastering DSP processors|(\d+) named DSP processors",
    Path("bindings/python/README.md"): r"(\d+) named DSP processors",
}
README_FX_OFF_COUNT_PATTERNS = {
    Path("README.md"): r"or (\d+) with\s+`BUILD_FX=OFF`",
    Path("README_ja.md"): r"外れて (\d+) 個になります",
}
PRESET_GROUPS = ("mastering", "synth", "mixingScene", "voiceChanger")


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
        raise ValueError(f"{path} must be an object")
    return value


def require_keys(value: dict[str, Any], path: str, keys: set[str]) -> None:
    missing = keys - value.keys()
    if missing:
        raise ValueError(
            f"{path} is missing required keys: {', '.join(sorted(missing))}"
        )


def validate_catalog(catalog: Any) -> dict[str, Any]:
    """Guard the generated artifact's stable schema without a third-party dependency."""
    root = require_object(catalog, "catalog")
    require_keys(root, "catalog", {"version", "abi", "processors", "presets"})
    if not isinstance(root["version"], str):
        raise ValueError("catalog.version must be a string")
    abi = require_object(root["abi"], "catalog.abi")
    require_keys(abi, "catalog.abi", {"project", "engine"})
    if not all(isinstance(abi[name], int) for name in ("project", "engine")):
        raise ValueError("catalog.abi values must be integers")
    if not isinstance(root["processors"], list):
        raise ValueError("catalog.processors must be an array")
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
                "params",
            },
        )
        if not isinstance(processor["params"], list):
            raise ValueError(f"catalog.processors[{index}].params must be an array")
        realtime_cost = processor["realtimeCost"]
        if realtime_cost is not None and realtime_cost not in {"low", "moderate", "high"}:
            raise ValueError(
                f"catalog.processors[{index}].realtimeCost must be low, moderate, high, or null"
            )
        if processor["realtimeInsertable"] != (realtime_cost is not None):
            raise ValueError(
                f"catalog.processors[{index}].realtimeCost must be non-null exactly for realtime inserts"
            )
        for parameter_index, parameter_value in enumerate(processor["params"]):
            parameter = require_object(
                parameter_value,
                f"catalog.processors[{index}].params[{parameter_index}]",
            )
            require_keys(
                parameter,
                f"catalog.processors[{index}].params[{parameter_index}]",
                {"name", "id", "rtSafe", "type", "min", "max", "default", "unit"},
            )
    presets = require_object(root["presets"], "catalog.presets")
    require_keys(
        presets,
        "catalog.presets",
        set(PRESET_GROUPS),
    )
    unexpected_groups = set(presets) - set(PRESET_GROUPS)
    if unexpected_groups:
        raise ValueError(
            "catalog.presets has unexpected groups: "
            + ", ".join(sorted(unexpected_groups))
        )
    if not all(isinstance(presets[name], list) for name in presets):
        raise ValueError("catalog preset groups must be arrays")
    for name in PRESET_GROUPS:
        preset_names = presets[name]
        if any(
            not isinstance(preset_name, str) or not preset_name
            for preset_name in preset_names
        ):
            raise ValueError(f"catalog.presets.{name} must contain non-empty strings")
        if len(preset_names) != len(set(preset_names)):
            raise ValueError(f"catalog.presets.{name} must contain unique names")
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


def documented_numbers(pattern: str, text: str) -> list[int]:
    """Return every nonempty capturing group in a README count pattern."""
    values: list[int] = []
    for match in re.finditer(pattern, text, flags=re.MULTILINE):
        values.extend(int(value) for value in match.groups() if value is not None)
    return values


def validate_documented_counts(catalog_text: str) -> None:
    """Keep every public README count derived from the checked catalog."""
    catalog = validate_catalog(json.loads(catalog_text))
    processor_count = len(catalog["processors"])
    feature_off_count = sum(
        not processor["id"].startswith("effects.")
        for processor in catalog["processors"]
    )

    for path, pattern in README_DEFAULT_COUNT_PATTERNS.items():
        numbers = documented_numbers(pattern, path.read_text(encoding="utf-8"))
        if not numbers:
            raise ValueError(f"could not find a processor count in {path}")
        if any(number != processor_count for number in numbers):
            raise ValueError(
                f"processor count in {path} is {numbers}, expected {processor_count} from catalog"
            )
    for path, pattern in README_FX_OFF_COUNT_PATTERNS.items():
        numbers = documented_numbers(pattern, path.read_text(encoding="utf-8"))
        if numbers != [feature_off_count]:
            raise ValueError(
                f"BUILD_FX=OFF count in {path} is {numbers}, expected {feature_off_count} from catalog"
            )


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
        try:
            validate_documented_counts(current)
        except ValueError as exc:
            print(f"capability catalog documentation is stale: {exc}", file=sys.stderr)
            return 1
        print(f"capability catalog is current: {args.output}")
        return 0

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    print(f"wrote capability catalog: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
