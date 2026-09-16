"""Runtime payload schema matching and cross-surface response comparison.

Owns what a live CLI response must look like: schema matching, the
path-specific semantic invariants, and the comparison of two surfaces'
responses.  The manifest vocabulary it builds on lives in
``cli_contract_schema``.
"""

from __future__ import annotations

import json
import re
from typing import Any

from cli_contract_schema import (
    _SCHEMA_METADATA_KEYS,
    _SPECTRAL_FEATURES,
    _SPECTRAL_STATS,
    _is_bool,
    _is_int,
    _is_number,
)


_SECTION_TYPE_RE = re.compile(r"[a-z][a-z0-9]*(?:-[a-z0-9]+)*\Z")

# A chord symbol as the core spells it: a root, a quality suffix, and an
# optional slash bass. The four triads are the only qualities a triads-only
# analysis can produce, so anything else in the suffix means the full template
# set was searched.
_CHORD_NAME_RE = re.compile(r"\A(?P<root>[A-G][#b]?)(?P<suffix>[^/]*)(?:/[A-G][#b]?)?\Z")
_TRIAD_SUFFIXES = frozenset({"", "m", "dim", "aug"})


def _chord_name_is_extended(name: str) -> bool:
    """True when a chord symbol names a quality outside the four triads."""
    match = _CHORD_NAME_RE.match(name)
    if match is None:
        # "N.C." and anything else unparseable is not evidence either way.
        return False
    return match.group("suffix") not in _TRIAD_SUFFIXES


def _payload_type(value: Any, token: str) -> bool:
    """Return whether a value matches a primitive Batch-0 payload token."""

    if token == "boolean":
        return _is_bool(value)
    if token == "integer":
        return _is_int(value)
    if token == "number":
        return _is_number(value)
    if token in {"string", "path"}:
        return isinstance(value, str)
    if token == "null":
        return value is None
    if token == "any":
        return True
    if token == "array:string":
        return isinstance(value, list) and all(isinstance(item, str) for item in value)
    if token == "array:number:12":
        return (
            isinstance(value, list)
            and len(value) == 12
            and all(_is_number(item) for item in value)
        )
    return False


def _schema_alternatives(schema: Any) -> list[Any] | None:
    """Return alternative schemas, or ``None`` when *schema* is one node."""

    if isinstance(schema, list):
        return schema
    if isinstance(schema, str) and "|" in schema:
        return schema.split("|")
    if not isinstance(schema, dict):
        return None
    for key in ("one_of", "any_of", "oneOf", "anyOf", "variants"):
        if key in schema:
            return schema[key]
    schema_type = schema.get("type")
    if isinstance(schema_type, list):
        return schema_type
    if isinstance(schema_type, str) and "|" in schema_type:
        return schema_type.split("|")
    if schema_type in {"variant", "union"}:
        return schema.get("variants")
    return None


def _schema_metadata(schema: dict[str, Any]) -> dict[str, Any]:
    """Drop field-level optional/required metadata from a schema node."""

    return {
        key: value
        for key, value in schema.items()
        if not (key in _SCHEMA_METADATA_KEYS and isinstance(value, bool))
    }


def _object_schema_parts(
    schema: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any], bool]:
    """Normalize all supported object spellings to required/optional maps."""

    core = _schema_metadata(schema)
    required: dict[str, Any] = {}
    optional: dict[str, Any] = {}

    def add_field(target: dict[str, Any], key: str, value: Any) -> None:
        # A field-level marker is useful for compact ``keys`` objects, while
        # the required/optional maps and lists remain the unambiguous form for
        # larger schemas. Explicit map/list membership wins over the marker.
        if isinstance(value, dict) and value.get("optional") is True:
            optional[key] = value
        elif isinstance(value, dict) and value.get("required") is True:
            required[key] = value
        else:
            target[key] = value

    if "keys" in core and isinstance(core["keys"], dict):
        for key, value in core["keys"].items():
            add_field(required, key, value)
    if "properties" in core and isinstance(core["properties"], dict):
        properties = core["properties"]
    else:
        properties = {}
    if isinstance(core.get("required"), dict):
        for key, value in core["required"].items():
            required[key] = value
    if isinstance(core.get("optional"), dict):
        for key, value in core["optional"].items():
            optional[key] = value
    if isinstance(core.get("required"), list):
        required_names = set(core["required"])
        for key, value in properties.items():
            if key in required_names:
                required[key] = value
            else:
                add_field(optional, key, value)
    else:
        for key, value in properties.items():
            if key not in required:
                add_field(optional, key, value)
    if isinstance(core.get("optional"), list):
        for key in core["optional"]:
            if key in required:
                optional[key] = required.pop(key)
    additional = core.get(
        "additional_properties", core.get("additionalProperties", False)
    )
    return required, optional, bool(additional)


def _schema_matches(value: Any, schema: Any, path: str) -> list[str]:
    """Validate *value* against one recursive payload schema node."""

    alternatives = _schema_alternatives(schema)
    if alternatives is not None:
        if not isinstance(alternatives, list) or not alternatives:
            return [f"{path}: expected a non-empty alternative schema"]
        alternative_errors = [
            _schema_matches(value, item, path) for item in alternatives
        ]
        if any(not errors for errors in alternative_errors):
            return []
        expected = ", ".join(_schema_description(item) for item in alternatives)
        return [f"{path}: expected one of {expected}, got {type(value).__name__}"]

    if isinstance(schema, str):
        if _payload_type(value, schema):
            return []
        return [f"{path}: expected {schema}, got {type(value).__name__}"]
    if not isinstance(schema, dict):
        return [f"{path}: invalid payload schema"]

    core = _schema_metadata(schema)
    schema_type = core.get("type")
    if schema_type is None and "keys" in core:
        schema_type = "object"
    if schema_type == "object":
        if not isinstance(value, dict):
            return [f"{path}: expected object, got {type(value).__name__}"]
        required, optional, additional = _object_schema_parts(core)
        errors: list[str] = []
        expected = set(required) | set(optional)
        missing = sorted(set(required) - set(value))
        unknown = sorted(set(value) - expected)
        if missing:
            errors.append(f"{path}: missing keys {', '.join(missing)}")
        if unknown and not additional:
            errors.append(f"{path}: unknown keys {', '.join(unknown)}")
        for key, field_schema in (*required.items(), *optional.items()):
            if key in value:
                errors.extend(
                    _schema_matches(value[key], field_schema, f"{path}.{key}")
                )
        return errors
    if schema_type == "array":
        if not isinstance(value, list):
            return [f"{path}: expected array, got {type(value).__name__}"]
        errors = []
        if "length" in core and len(value) != core["length"]:
            errors.append(
                f"{path}: expected array length {core['length']}, got {len(value)}"
            )
        if "min_items" in core and len(value) < core["min_items"]:
            errors.append(
                f"{path}: expected at least {core['min_items']} items, got {len(value)}"
            )
        if "max_items" in core and len(value) > core["max_items"]:
            errors.append(
                f"{path}: expected at most {core['max_items']} items, got {len(value)}"
            )
        if "items" in core:
            for index, item in enumerate(value):
                errors.extend(_schema_matches(item, core["items"], f"{path}[{index}]"))
        return errors
    if isinstance(schema_type, str):
        if _payload_type(value, schema_type):
            return []
        return [f"{path}: expected {schema_type}, got {type(value).__name__}"]
    return [f"{path}: invalid payload schema"]


def _schema_description(schema: Any) -> str:
    if isinstance(schema, str):
        return schema
    if isinstance(schema, dict):
        if isinstance(schema.get("type"), str):
            return schema["type"]
        if "keys" in schema or schema.get("type") == "object":
            return "object"
        return "schema"
    return "schema"


def validate_payload(payload: Any, schema: Any, label: str) -> list[str]:
    return _schema_matches(payload, schema, label)


def parse_single_json(stdout: str) -> Any:
    text = stdout.strip()
    if not text:
        raise ValueError("stdout is empty")
    decoder = json.JSONDecoder()
    try:
        value, end = decoder.raw_decode(text)
    except json.JSONDecodeError as exc:
        raise ValueError(f"stdout is not JSON: {exc.msg}") from exc
    if text[end:].strip():
        raise ValueError("stdout contains more than one JSON document")
    return value


def _compare_values(
    left: Any, right: Any, path: str, absolute: float, relative: float
) -> str | None:
    if isinstance(left, bool) or isinstance(right, bool):
        return None if left == right else f"{path}: {left!r} != {right!r}"
    if _is_number(left) and _is_number(right):
        difference = abs(float(left) - float(right))
        limit = max(absolute, relative * max(abs(float(left)), abs(float(right))))
        return (
            None
            if difference <= limit
            else f"{path}: {left!r} != {right!r} (delta {difference:g} > {limit:g})"
        )
    if type(left) is not type(right):
        return f"{path}: {type(left).__name__} != {type(right).__name__}"
    if isinstance(left, dict):
        if set(left) != set(right):
            return f"{path}: object keys differ ({sorted(left)!r} != {sorted(right)!r})"
        for key in left:
            mismatch = _compare_values(
                left[key], right[key], f"{path}.{key}", absolute, relative
            )
            if mismatch:
                return mismatch
        return None
    if isinstance(left, list):
        if len(left) != len(right):
            return f"{path}: array lengths differ ({len(left)} != {len(right)})"
        for index, (left_item, right_item) in enumerate(zip(left, right, strict=True)):
            mismatch = _compare_values(
                left_item, right_item, f"{path}[{index}]", absolute, relative
            )
            if mismatch:
                return mismatch
        return None
    return None if left == right else f"{path}: {left!r} != {right!r}"


def _compare_payloads(
    path: str,
    left: Any,
    right: Any,
    manifest: dict[str, Any],
    case_id: str,
    writes_surface_artifact: bool = False,
) -> str | None:
    absolute = float(manifest["comparison"]["absolute"])
    relative = float(manifest["comparison"]["relative"])
    if writes_surface_artifact and isinstance(left, dict) and isinstance(right, dict):
        # A case that declares an artifact is given a per-surface destination so
        # the two files survive to be digested, so the path each payload echoes
        # is the harness's, not the surface's. The bytes at it are compared.
        left = {key: value for key, value in left.items() if key != "output"}
        right = {key: value for key, value in right.items() if key != "output"}
    if path == "version":
        if not isinstance(left, dict) or not isinstance(right, dict):
            return f"{path}.{case_id}: expected objects for surface comparison"
        for key in ("cli_version", "lib_version"):
            mismatch = _compare_values(
                left.get(key), right.get(key), f"{path}.{key}", absolute, relative
            )
            if mismatch:
                return mismatch
        return None
    if path == "voice-preset-validate" and case_id == "failure":
        if left.get("ok") is not False or right.get("ok") is not False:
            return f"{path}.{case_id}: both failure payloads must set ok=false"
        return None
    if path == "voice-preset-validate" and case_id == "success":
        if left.get("ok") is not True or right.get("ok") is not True:
            return f"{path}.{case_id}: both success payloads must set ok=true"
        try:
            left_json = json.loads(left["normalized_json"])
            right_json = json.loads(right["normalized_json"])
        except (TypeError, json.JSONDecodeError) as exc:
            return f"{path}.{case_id}: normalized_json is not valid JSON: {exc}"
        return _compare_values(
            left_json, right_json, f"{path}.normalized_json", absolute, relative
        )
    if path == "synthesize-rir":
        if not isinstance(left, dict) or not isinstance(right, dict):
            return f"{path}.{case_id}: expected objects for surface comparison"
        for key in ("sample_rate", "samples"):
            mismatch = _compare_values(
                left.get(key), right.get(key), f"{path}.{key}", absolute, relative
            )
            if mismatch:
                return mismatch
        return None
    return _compare_values(left, right, f"{path}.{case_id}", absolute, relative)


def _validate_case_payload(
    path: str,
    case: dict[str, Any],
    payload: Any,
    contract: dict[str, Any],
    surface: str,
    label: str,
    report: list[tuple[str, str]],
) -> None:
    """Validate one successful JSON response, including path-specific invariants.

    The legacy exit compatibility switch changes only exit status.  Keeping all
    semantic checks here means the normal and legacy runs are deliberately held
    to the same payload contract rather than merely the same JSON shape.
    """
    payload_schema = contract["payloads"][case["payload"]]
    for error in validate_payload(payload, payload_schema, label):
        report.append(("fail", error))
    if not isinstance(payload, dict):
        return

    case_id = case["id"]
    if path == "version" and payload.get("cli") != surface:
        report.append(
            (
                "fail",
                f"{label}.cli: expected {surface!r}, got {payload.get('cli')!r}",
            )
        )
    if path == "chroma":
        if payload.get("n_chroma") != 12:
            report.append(
                (
                    "fail",
                    f"{label}.n_chroma: expected 12, got {payload.get('n_chroma')!r}",
                )
            )
        if isinstance(payload.get("n_frames"), int) and payload["n_frames"] < 0:
            report.append(("fail", f"{label}.n_frames: expected non-negative integer"))
    # Section-type spelling is keyed on the payload carrying a ``sections`` list,
    # not on the command that produced it.  ``analyze`` and ``sections``
    # serialize the same enum, and pinning the rule to one command name let the
    # other publish a Title-Case spelling that matched nothing a script written
    # against the first looks for.  Any path promoted into the manifest inherits
    # the check with no edit here.
    sections = payload.get("sections")
    if isinstance(sections, list):
        for index, section in enumerate(sections):
            if not isinstance(section, dict):
                continue
            section_type = section.get("type")
            if (
                not isinstance(section_type, str)
                or _SECTION_TYPE_RE.fullmatch(section_type) is None
            ):
                report.append(
                    (
                        "fail",
                        f"{label}.sections[{index}].type: expected lowercase-kebab section type, "
                        f"got {section_type!r}",
                    )
                )
    if path == "analyze":
        if case_id == "with_seventh":
            # What the flag promises is that the template set is not restricted
            # to the four triads, so that is what this asserts. It used to look
            # for a name ending in "7", which was a proxy rather than the
            # contract: the fixture is a single sine with one pitch class, so
            # whichever non-triad wins is arbitrary, and pinning one spelling
            # made the check fail whenever the vocabulary changed even though
            # the flag went on working.
            chords = payload.get("chords")
            has_extended = isinstance(chords, list) and any(
                isinstance(chord, dict)
                and isinstance(chord.get("name"), str)
                and _chord_name_is_extended(chord["name"])
                for chord in chords
            )
            if not has_extended:
                report.append(
                    (
                        "fail",
                        f"{label}.chords: --with-seventh must widen the vocabulary past the "
                        f"four triads; got {chords!r}",
                    )
                )
    if path == "mastering" and case_id == "target_within_ceiling":
        # The flag says the true-peak ceiling, not the target, decided the level.
        # Reading it as a value rather than as a type is what separates a surface
        # that computes it from one that publishes a constant. A target this far
        # below the ceiling is reached outright, so both surfaces must say so.
        limited = payload.get("loudness_target_limited")
        if limited is not False:
            report.append(
                (
                    "fail",
                    f"{label}.loudness_target_limited: expected False, got {limited!r}",
                )
            )
    if path == "mastering" and case_id == "ceiling_limits_target":
        # The opposite direction of target_within_ceiling: the fixture is a -6
        # dBFS tone, so a -6 LUFS target under a -3 dBTP ceiling cannot be
        # reached and both surfaces must say so. The cross-surface comparison
        # protects what applied_gain_db means -- the pre-limiter static gain,
        # not the bare ceiling headroom, which sits about 0.86 dB away on this
        # fixture. Omitting --report also makes this the one mastering case
        # entering through the standalone loudness helper rather than the chain.
        limited = payload.get("loudness_target_limited")
        if limited is not True:
            report.append(
                (
                    "fail",
                    f"{label}.loudness_target_limited: expected True, got {limited!r}",
                )
            )
        output_lufs = payload.get("output_lufs")
        if not _is_number(output_lufs) or float(output_lufs) > -6.5:
            report.append(
                (
                    "fail",
                    f"{label}.output_lufs: ceiling-limited run must stop short of "
                    f"its target, got {output_lufs!r}",
                )
            )
    if path == "spectral":
        n_frames = payload.get("n_frames")
        if not isinstance(n_frames, int) or n_frames <= 0:
            report.append(
                (
                    "fail",
                    f"{label}.n_frames: expected a positive integer, got {n_frames!r}",
                )
            )
        features = payload.get("features")
        if isinstance(features, dict):
            missing = sorted(_SPECTRAL_FEATURES - set(features))
            extra = sorted(set(features) - _SPECTRAL_FEATURES)
            if missing or extra:
                details = []
                if missing:
                    details.append(f"missing {', '.join(missing)}")
                if extra:
                    details.append(f"unexpected {', '.join(extra)}")
                report.append(
                    (
                        "fail",
                        f"{label}.features: canonical feature keys differ ({'; '.join(details)})",
                    )
                )
            for name in sorted(_SPECTRAL_FEATURES & set(features)):
                stats = features[name]
                if not isinstance(stats, dict):
                    continue
                missing_stats = sorted(_SPECTRAL_STATS - set(stats))
                extra_stats = sorted(set(stats) - _SPECTRAL_STATS)
                if missing_stats or extra_stats:
                    details = []
                    if missing_stats:
                        details.append(f"missing {', '.join(missing_stats)}")
                    if extra_stats:
                        details.append(f"unexpected {', '.join(extra_stats)}")
                    report.append(
                        (
                            "fail",
                            f"{label}.features.{name}: canonical statistic keys differ "
                            f"({'; '.join(details)})",
                        )
                    )
    if path == "voice-change":
        latency = payload.get("latency_samples")
        if case_id == "simple" and latency != 0:
            report.append(
                (
                    "fail",
                    f"{label}.latency_samples: simple-knob mode must report 0, got {latency!r}",
                )
            )
        if case_id == "preset" and (
            not isinstance(payload.get("preset"), str) or not payload["preset"]
        ):
            report.append(
                (
                    "fail",
                    f"{label}.preset: explicit preset mode must report its preset id",
                )
            )
        if case_id == "custom":
            if "preset" in payload:
                report.append(
                    (
                        "fail",
                        f"{label}: --preset-json without --preset must omit preset",
                    )
                )
            if not _is_int(latency) or latency <= 0:
                report.append(
                    (
                        "fail",
                        f"{label}.latency_samples: custom preset must report actual positive latency, got {latency!r}",
                    )
                )
    if path == "project.compile":
        diagnostics = payload.get("diagnostics")
        diagnostic_count = payload.get("diagnostic_count")
        if isinstance(diagnostics, list) and diagnostic_count != len(diagnostics):
            report.append(
                ("fail", f"{label}: diagnostic_count does not equal diagnostics length")
            )
        if case_id == "clean" and (
            not _is_bool(payload.get("has_timeline")) or diagnostic_count != 0
        ):
            report.append(
                (
                    "fail",
                    f"{label}: clean project must report a boolean timeline flag and no diagnostics",
                )
            )
    if path == "synthesize-rir":
        if not _is_int(payload.get("sample_rate")) or payload.get("sample_rate") <= 0:
            report.append(
                (
                    "fail",
                    f"{label}.sample_rate: expected a positive integer",
                )
            )
        if not _is_int(payload.get("samples")) or payload.get("samples") <= 0:
            report.append(("fail", f"{label}.samples: expected a positive integer"))
    if path == "rhythm":
        time_signature = payload.get("time_signature")
        if isinstance(time_signature, dict):
            for key in ("numerator", "denominator"):
                if not _is_int(time_signature.get(key)) or time_signature[key] <= 0:
                    report.append(
                        (
                            "fail",
                            f"{label}.time_signature.{key}: expected a positive integer",
                        )
                    )
            if not _is_number(time_signature.get("confidence")):
                report.append(
                    (
                        "fail",
                        f"{label}.time_signature.confidence: expected a finite number",
                    )
                )
        intervals = payload.get("beat_intervals")
        if isinstance(intervals, dict) and (
            not _is_int(intervals.get("count")) or intervals["count"] < 0
        ):
            report.append(
                (
                    "fail",
                    f"{label}.beat_intervals.count: expected a non-negative integer",
                )
            )
    if path == "pitch":
        n_frames = payload.get("n_frames")
        voiced_count = payload.get("voiced_count")
        if _is_int(n_frames) and _is_int(voiced_count) and voiced_count > n_frames:
            report.append(
                (
                    "fail",
                    f"{label}.voiced_count: cannot exceed n_frames",
                )
            )
        ratio = payload.get("voiced_ratio")
        if ratio is not None and (not _is_number(ratio) or not 0.0 <= ratio <= 1.0):
            report.append(
                (
                    "fail",
                    f"{label}.voiced_ratio: expected null or a number in [0, 1]",
                )
            )
    if path == "eq":
        if payload.get("processor") != "eq.equalizer":
            report.append(
                (
                    "fail",
                    f"{label}.processor: expected 'eq.equalizer', got {payload.get('processor')!r}",
                )
            )
        if case_id == "default" and payload.get("output") != "":
            report.append(
                (
                    "fail",
                    f"{label}.output: omitted output must serialize as an empty string",
                )
            )
        if case_id == "explicit_output" and not isinstance(payload.get("output"), str):
            report.append(
                ("fail", f"{label}.output: explicit output must serialize as a path")
            )
    if path == "mastering-processor":
        expected_stereo = case_id == "stereo"
        if payload.get("stereo") is not expected_stereo:
            report.append(
                (
                    "fail",
                    f"{label}.stereo: expected {expected_stereo!r}, got {payload.get('stereo')!r}",
                )
            )
        expected_processor = "stereo.imager" if expected_stereo else "eq.equalizer"
        if payload.get("processor") != expected_processor:
            report.append(
                (
                    "fail",
                    f"{label}.processor: expected {expected_processor!r}, got {payload.get('processor')!r}",
                )
            )
    if path == "project.validate":
        diagnostics = payload.get("diagnostics")
        diagnostic_count = payload.get("diagnostic_count")
        if isinstance(diagnostics, list) and diagnostic_count != len(diagnostics):
            report.append(
                ("fail", f"{label}: diagnostic_count does not equal diagnostics length")
            )
        if (
            case_id in {"clean", "warning", "warning_strict_artifact"}
            and payload.get("valid") is not True
        ):
            report.append(
                (
                    "fail",
                    f"{label}.valid: expected true for a successfully loaded project",
                )
            )
        if case_id == "clean" and diagnostic_count != 0:
            report.append(
                ("fail", f"{label}: clean project must not report diagnostics")
            )
        if case_id in {"warning", "warning_strict_artifact"} and (
            not isinstance(diagnostic_count, int) or diagnostic_count <= 0
        ):
            report.append(
                (
                    "fail",
                    f"{label}: warning fixture must report at least one diagnostic",
                )
            )
    if path == "voice-preset-validate":
        # The declared payload shape, not the case id, is what says whether the
        # invocation is meant to validate: several cases produce each shape.
        expected_ok = case["payload"] == "success"
        if payload.get("ok") is not expected_ok:
            report.append(
                (
                    "fail",
                    f"{label}.ok: expected {expected_ok!r}, got {payload.get('ok')!r}",
                )
            )
        if expected_ok:
            try:
                normalized = parse_single_json(payload["normalized_json"])
            except (KeyError, TypeError, ValueError) as exc:
                report.append(("fail", f"{label}.normalized_json: {exc}"))
                return
            if not isinstance(normalized, dict):
                report.append(("fail", f"{label}.normalized_json: expected an object"))
                return
            required = {"schemaVersion", "id", "name", "category"}
            missing = sorted(required - set(normalized))
            if missing:
                report.append(
                    (
                        "fail",
                        f"{label}.normalized_json: missing preset fields {', '.join(missing)}",
                    )
                )
            if ("dsp" in normalized) == ("macros" in normalized):
                report.append(
                    (
                        "fail",
                        f"{label}.normalized_json: expected exactly one of dsp or macros",
                    )
                )


def _validate_analyze_case_relationships(
    surface: str,
    payloads: dict[tuple[str, str], Any],
    manifest: dict[str, Any],
    report: list[tuple[str, str]],
) -> None:
    """Check fixture-level semantics that cannot be expressed by field types."""

    default = payloads.get(("analyze", "default"))
    explicit = payloads.get(("analyze", "chroma_highpass_explicit"))
    if default is not None and explicit is not None:
        mismatch = _compare_values(
            default,
            explicit,
            f"{surface}.analyze.default-vs-explicit-highpass",
            float(manifest["comparison"]["absolute"]),
            float(manifest["comparison"]["relative"]),
        )
        if mismatch:
            report.append(
                (
                    "fail",
                    mismatch + " (--chroma-highpass default must be canonical 80 Hz)",
                )
            )

    no_hpss = payloads.get(("analyze", "no_hpss"))
    if default is not None and no_hpss is not None:
        mismatch = _compare_values(
            default,
            no_hpss,
            f"{surface}.analyze.default-vs-no-hpss",
            float(manifest["comparison"]["absolute"]),
            float(manifest["comparison"]["relative"]),
        )
        if mismatch is None:
            report.append(
                (
                    "fail",
                    f"{surface}.analyze.no_hpss: --no-hpss did not change the canonical fixture result",
                )
            )


def _validate_voice_case_relationships(
    payloads: dict[tuple[str, str], Any],
    manifest: dict[str, Any],
    surface: str,
    report: list[tuple[str, str]],
) -> None:
    """Pin the live voice fixtures, including config-dependent latency."""

    expected_length = manifest["fixtures"]["audio"]["frames"]
    expected_rate = manifest["fixtures"]["audio"]["sample_rate"]
    for case_id in ("simple", "preset", "custom"):
        payload = payloads.get(("voice-change", case_id))
        if not isinstance(payload, dict):
            continue
        if payload.get("length") != expected_length:
            report.append(
                (
                    "fail",
                    f"{surface}.voice-change.{case_id}.length: expected {expected_length}, "
                    f"got {payload.get('length')!r}",
                )
            )
        if payload.get("sample_rate") != expected_rate:
            report.append(
                (
                    "fail",
                    f"{surface}.voice-change.{case_id}.sample_rate: expected {expected_rate}, "
                    f"got {payload.get('sample_rate')!r}",
                )
            )
    simple = payloads.get(("voice-change", "simple"))
    if isinstance(simple, dict):
        for key, expected in (("pitch_semitones", 5.0), ("formant_factor", 1.1)):
            mismatch = _compare_values(
                simple.get(key),
                expected,
                f"{surface}.voice-change.simple.{key}",
                float(manifest["comparison"]["absolute"]),
                float(manifest["comparison"]["relative"]),
            )
            if mismatch:
                report.append(("fail", mismatch))
    preset = payloads.get(("voice-change", "preset"))
    if isinstance(preset, dict):
        if preset.get("preset") != "bright-idol":
            report.append(
                (
                    "fail",
                    f"{surface}.voice-change.preset.preset: expected 'bright-idol', "
                    f"got {preset.get('preset')!r}",
                )
            )
        if preset.get("latency_samples") != 1042:
            report.append(
                (
                    "fail",
                    f"{surface}.voice-change.preset.latency_samples: expected 1042, "
                    f"got {preset.get('latency_samples')!r}",
                )
            )
    custom = payloads.get(("voice-change", "custom"))
    if isinstance(custom, dict) and custom.get("latency_samples") != 82:
        report.append(
            (
                "fail",
                f"{surface}.voice-change.custom.latency_samples: expected actual custom latency 82, "
                f"got {custom.get('latency_samples')!r}",
            )
        )
