#!/usr/bin/env python3
"""Check the cross-language streaming contract snapshot and corpus schema."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CORPUS_PATH = ROOT / "tests/conformance/public_input_corpus.json"


def _read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def _block(text: str, pattern: str, label: str, errors: list[str]) -> str:
    match = re.search(pattern, text, re.DOTALL | re.MULTILINE)
    if match is None:
        errors.append(f"{label}: declaration not found")
        return ""
    return match.group("body")


def _require_fields(
    label: str, body: str, fields: list[str], errors: list[str]
) -> None:
    for field in fields:
        if re.search(rf"\b{re.escape(field)}\b", body) is None:
            errors.append(f"{label}: missing field {field}")


def _camel(name: str) -> str:
    head, *tail = name.split("_")
    return head + "".join(part.title() for part in tail)


def main() -> int:
    corpus = json.loads(CORPUS_PATH.read_text(encoding="utf-8"))
    errors: list[str] = []
    if corpus.get("schema_version") != 1:
        errors.append("corpus: schema_version must be 1")

    transaction = corpus.get("marker_transaction", {})
    case_ids = {case.get("id") for case in transaction.get("cases", [])}
    required_cases = {
        "empty_commit",
        "uint32_upper_commit",
        "zero_id",
        "negative_ppq",
        "nan_ppq",
        "infinite_ppq",
        "negative_id",
        "fractional_id",
        "oversize_id",
        "partial_failure",
    }
    missing_cases = sorted(required_cases - case_ids)
    if missing_cases:
        errors.append(f"corpus: missing marker cases {', '.join(missing_cases)}")

    contract = corpus["streaming_contract"]
    snake_fields = contract["frame_fields"]
    camel_fields = [_camel(field) for field in snake_fields]

    c_header = _read("include/sonare/sonare_c_streaming.h")
    cpp_header = _read("src/streaming/stream_frame.h")
    node_types = _read("bindings/node/src/types_features.ts")
    wasm_types = _read("bindings/wasm/src/stream_types.ts")
    wasm_raw_types = _read("bindings/wasm/src/sonare.js.d.ts")
    python_types = _read("bindings/python/src/libsonare/_types_streaming.py")
    python_stubs = _read("bindings/python/src/libsonare/types.pyi")

    declarations = [
        (
            "C SonareStreamFrames",
            _block(
                c_header,
                r"typedef struct\s*\{(?P<body>.*?)\}\s*SonareStreamFrames;",
                "C SonareStreamFrames",
                errors,
            ),
            snake_fields,
        ),
        (
            "C++ FrameBuffer",
            _block(
                cpp_header,
                r"struct FrameBuffer\s*\{(?P<body>.*?)\n\};",
                "C++ FrameBuffer",
                errors,
            ),
            snake_fields,
        ),
        (
            "Node StreamFramesSoa",
            _block(
                node_types,
                r"export interface StreamFramesSoa\s*\{(?P<body>.*?)\n\}",
                "Node StreamFramesSoa",
                errors,
            ),
            camel_fields,
        ),
        (
            "WASM FrameBuffer",
            _block(
                wasm_types,
                r"export interface FrameBuffer\s*\{(?P<body>.*?)\n\}",
                "WASM FrameBuffer",
                errors,
            ),
            camel_fields,
        ),
        (
            "raw WASM WasmFrameBuffer",
            _block(
                wasm_raw_types,
                r"export interface WasmFrameBuffer\s*\{(?P<body>.*?)\n\}",
                "raw WASM WasmFrameBuffer",
                errors,
            ),
            camel_fields,
        ),
        (
            "Python StreamFrames",
            _block(
                python_types,
                r"class StreamFrames:\n(?P<body>.*?)(?=\n@dataclass|\Z)",
                "Python StreamFrames",
                errors,
            ),
            snake_fields,
        ),
        (
            "Python StreamFrames stub",
            _block(
                python_stubs,
                r"class StreamFrames:\n(?P<body>.*?)(?=\nclass StreamFramesU8:|\Z)",
                "Python StreamFrames stub",
                errors,
            ),
            snake_fields,
        ),
    ]
    for label, body, fields in declarations:
        _require_fields(label, body, fields, errors)

    flags = contract["feature_flags"]
    flag_names = {
        "mel": "MEL",
        "chroma": "CHROMA",
        "onset": "ONSET",
        "spectral": "SPECTRAL",
    }
    for key, value in flags.items():
        shift = value.bit_length() - 1
        upper = flag_names[key]
        if f"SONARE_STREAM_FEATURE_{upper} = 1u << {shift}" not in c_header:
            errors.append(f"C feature flag {upper} no longer equals {value}")
        title = key.title()
        if f"kStreamFeature{title} = 1u << {shift}" not in cpp_header:
            errors.append(f"C++ feature flag {title} no longer equals {value}")

    config_header = _read("src/streaming/stream_config.h")
    c_impl = _read("src/c_api/features_streaming.cpp")
    if contract["output_format_default"] != 0 or "Float32 = 0" not in config_header:
        errors.append(
            "C++ streaming output format default is not the snapshotted Float32=0"
        )
    for legacy in contract["legacy_output_formats_rejected"]:
        token = {1: "INT16", 2: "UINT8"}.get(legacy)
        if token is None or f"SONARE_STREAM_OUTPUT_{token} = {legacy}" not in c_header:
            errors.append(f"C legacy output format {legacy} declaration drifted")
    if "config->output_format != SONARE_STREAM_OUTPUT_FLOAT32" not in c_impl:
        errors.append("C analyzer no longer rejects legacy generic output formats")

    if contract["disabled_feature_shape"] != "empty":
        errors.append("streaming contract disabled_feature_shape must remain empty")
    if contract["overflow_policy"] != "drop_newest":
        errors.append("streaming contract overflow_policy must remain drop_newest")
    for label, text, phrase in [
        ("C overflow docs", c_header, "overflow drops newest"),
        ("C++ overflow docs", config_header, "newly produced frame"),
        ("Python disabled-shape docs", python_types, "empty scalar array"),
    ]:
        if phrase not in text:
            errors.append(f"{label}: missing contract phrase {phrase!r}")

    if errors:
        print("public contract conformance failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("public contract conformance: OK (marker corpus + 7 streaming surfaces)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
