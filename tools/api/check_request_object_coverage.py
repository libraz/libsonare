#!/usr/bin/env python3
"""Report JS/TS one-shot exports that still lack a request-object overload.

This is deliberately a source-level guard: options/request reshapes are invisible
to the C-ABI parity checker.  It reports, rather than mutates, the public facade
inventory so the migration plan can be completed without losing functions.
"""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SURFACES = {
    "node": ROOT / "bindings/node/src",
    "wasm": ROOT / "bindings/wasm/src",
}
EXCLUDED = {
    "_chain_config.ts",
    "errors.ts",
    "native.ts",
    "validation.ts",
    "value_coercion.ts",
    "codes.ts",
    "module_state.ts",
    "project_internal.ts",
    "analysis_helpers.ts",
}
# These are public exports, but deliberately outside the request-object policy:
# scalar/unit conversion helpers, discovery/version metadata, and factories for
# stateful or platform services.  Keep this manifest explicit: a newly exported
# one-shot audio operation must either gain a *Request overload or be reviewed
# here with a reason.
EXEMPT_FILES = {
    "project.ts",
    "project_synth.ts",
    "realtime_engine.ts",
    "realtime_voice_changer.ts",
    "opfs_clip_pages.ts",
    "scale.ts",
    "stream_analyzer.ts",
    "web_midi.ts",
    "worker.ts",
}
SCALAR_HELPERS = {
    "hzToMel",
    "melToHz",
    "hzToMidi",
    "midiToHz",
    "hzToNote",
    "noteToHz",
    "framesToTime",
    "timeToFrames",
    "framesToSamples",
    "samplesToFrames",
    "dbToPower",
    "dbToAmplitude",
    "fixFrames",
    "tonnetz",
    "scaleQuantizeMidi",
    "scaleCorrectionSemitones",
    "scalePitchClassEnabled",
}
METADATA_HELPERS = {
    "version",
    "capabilities",
    "capabilityCatalog",
    "abiVersion",
    "engineAbiVersion",
    "voiceChangerAbiVersion",
    "projectAbiVersion",
    "hasFfmpegSupport",
    "isInitialized",
    "engineCapabilities",
    "masteringPresetNames",
    "masteringProcessorNames",
    "masteringPairProcessorNames",
    "masteringPairAnalysisNames",
    "masteringStereoAnalysisNames",
    "masteringInsertNames",
    "masteringInsertParamNames",
    "masteringInsertParamInfo",
    "masteringProcessorCatalog",
    "mixingScenePresetNames",
    "mixingScenePresetJson",
    "realtimeVoiceChangerPresetNames",
    "realtimeVoiceChangerPresetJson",
    "validateRealtimeVoiceChangerPresetJson",
    "voiceCharacterPresetId",
    "realtimeVoiceChangerPresetConfig",
}
# These two functions were already object-shaped before this migration.  They
# have semantic options types rather than a newly named *Request interface.
OBJECT_SHAPED = {"synthesizeRir"}
# This attach helper coordinates pre-existing engine/streamer instances.  Its
# final argument is already the semantic options object; wrapping those opaque
# stateful handles in a one-shot request would only add a second API shape.
STATEFUL_ATTACH_HELPERS = {"attachOpfsClipStream"}
FUNCTION = re.compile(r"export function (\w+)\s*\(")


def functions(path: Path) -> list[str]:
    text = path.read_text()
    names = []
    for match in FUNCTION.finditer(text):
        name = match.group(1)
        # Discovery/version/scalar helpers are not one-shot audio APIs. The
        # remaining candidates are reviewed through the migration manifest.
        if name not in names:
            names.append(name)
    return names


def has_request_overload(text: str, name: str) -> bool:
    # Canonical overloads are adjacent to legacy overloads and use a Request
    # type. Keeping this simple makes deviations visible during review.
    return bool(
        re.search(
            rf"export function {re.escape(name)}\(\s*\w+:\s*[^)]*Request",
            text,
        )
    )


def exemption(path: Path, name: str) -> str | None:
    if path.name in EXEMPT_FILES:
        return "stateful/project/platform API"
    if name in SCALAR_HELPERS:
        return "scalar or small utility"
    if name in METADATA_HELPERS:
        return "catalog, capability, or version metadata"
    if name in OBJECT_SHAPED:
        return "pre-existing object-shaped API"
    if name in STATEFUL_ATTACH_HELPERS:
        return "stateful attach helper with an existing options object"
    return None


REQUEST = re.compile(r"\w+Request")
STAR_REEXPORT = re.compile(r"export\s+\*\s+from\s+['\"]\./([\w/]+?)(?:\.js)?['\"]")
NAMED_REEXPORT = re.compile(
    r"export\s+(?:type\s+)?\{(.*?)\}\s*from\s*['\"]\./([\w/]+?)(?:\.js)?['\"]", re.S
)
NAMED_TYPE_EXPORT = re.compile(r"export\s+type\s*\{(.*?)\}", re.S)
EXPORTED_REQUEST_DEF = re.compile(r"export (?:interface|type) (\w+Request)\b")
EXPORTED_FUNCTION_PARAMS = re.compile(r"export function \w+\s*\(([^;{]*?)\)\s*:", re.S)


def _module_request_exports(mod_path: Path, visited: set[Path]) -> set[str]:
    # Every *Request name a module surfaces, following `export * from` chains so
    # a barrel that re-exports a sub-barrel (Node's style) is fully resolved.
    if not mod_path.exists() or mod_path in visited:
        return set()
    visited.add(mod_path)
    text = mod_path.read_text()
    names = set(EXPORTED_REQUEST_DEF.findall(text))
    for block, _module in NAMED_REEXPORT.findall(text):
        names.update(REQUEST.findall(block))
    for module in STAR_REEXPORT.findall(text):
        names |= _module_request_exports(mod_path.parent / f"{module}.ts", visited)
    return names


def entry_exported_requests(surface_dir: Path, index_path: Path) -> set[str]:
    # The *Request types reachable through the package entry (index.ts): explicit
    # `export type { ... }` lists plus everything pulled in by `export *` chains.
    text = index_path.read_text()
    names: set[str] = set()
    for block in NAMED_TYPE_EXPORT.findall(text):
        names.update(REQUEST.findall(block))
    for module in STAR_REEXPORT.findall(text):
        names |= _module_request_exports(surface_dir / f"{module}.ts", set())
    return names


def param_request_types(surface_dir: Path) -> set[str]:
    # The *Request types used as a parameter of an exported one-shot function —
    # the types a caller must be able to import to call the public API.
    names: set[str] = set()
    for path in sorted(surface_dir.glob("*.ts")):
        if path.name == "index.ts":
            continue
        text = path.read_text()
        for params in EXPORTED_FUNCTION_PARAMS.findall(text):
            names.update(REQUEST.findall(params))
    return names


def unexported_from_entry() -> list[str]:
    # A request type accepted by an exported function but not re-exported from the
    # package entry is unusable by consumers (they cannot name the argument type).
    # This drift is invisible to the C-ABI parity checker, so guard it here.
    markers: list[str] = []
    for surface, directory in SURFACES.items():
        index_path = directory / "index.ts"
        if not index_path.exists():
            continue
        entry = entry_exported_requests(directory, index_path)
        for name in sorted(param_request_types(directory) - entry):
            markers.append(f"{surface}:{name}")
    return markers


def main() -> int:
    missing: list[str] = []
    covered: list[str] = []
    exempt: list[str] = []
    for surface, directory in SURFACES.items():
        for path in sorted(directory.glob("*.ts")):
            if path.name in EXCLUDED:
                continue
            text = path.read_text()
            for name in functions(path):
                marker = f"{surface}:{path.name}:{name}"
                if has_request_overload(text, name):
                    covered.append(marker)
                elif reason := exemption(path, name):
                    exempt.append(f"{marker} ({reason})")
                else:
                    missing.append(marker)
    print(
        "request-object coverage: "
        f"{len(covered)} covered, {len(exempt)} explicit exemptions, "
        f"{len(missing)} missing"
    )
    for marker in missing:
        print(f"MISSING {marker}")

    unexported = unexported_from_entry()
    print(
        f"entry-export coverage: {len(unexported)} request types unexported from entry"
    )
    for marker in unexported:
        print(f"UNEXPORTED {marker}")

    return 1 if (missing or unexported) else 0


if __name__ == "__main__":
    raise SystemExit(main())
