#!/usr/bin/env python3
"""Report JS/TS one-shot exports that still lack a request-object overload.

This is deliberately a source-level guard: options/request reshapes are invisible
to the C-ABI parity checker.  It reports, rather than mutates, the public facade
inventory so the migration plan can be completed without losing functions.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SURFACES = {
    "node": ROOT / "bindings/node/src",
    "wasm": ROOT / "bindings/wasm/src",
}
EXCLUDED = {
    "_chain_config.ts", "errors.ts", "native.ts", "validation.ts", "value_coercion.ts",
    "codes.ts", "module_state.ts", "project_internal.ts", "analysis_helpers.ts",
}
# These are public exports, but deliberately outside the request-object policy:
# scalar/unit conversion helpers, discovery/version metadata, and factories for
# stateful or platform services.  Keep this manifest explicit: a newly exported
# one-shot audio operation must either gain a *Request overload or be reviewed
# here with a reason.
EXEMPT_FILES = {
    "project.ts", "project_synth.ts", "realtime_engine.ts", "realtime_voice_changer.ts",
    "opfs_clip_pages.ts", "scale.ts", "stream_analyzer.ts", "web_midi.ts",
}
SCALAR_HELPERS = {
    "hzToMel", "melToHz", "hzToMidi", "midiToHz", "hzToNote", "noteToHz",
    "framesToTime", "timeToFrames", "framesToSamples", "samplesToFrames",
    "dbToPower", "dbToAmplitude", "fixFrames", "tonnetz",
    "scaleQuantizeMidi", "scaleCorrectionSemitones", "scalePitchClassEnabled",
}
METADATA_HELPERS = {
    "version", "abiVersion", "engineAbiVersion", "voiceChangerAbiVersion",
    "projectAbiVersion", "hasFfmpegSupport", "isInitialized", "engineCapabilities",
    "masteringPresetNames", "masteringProcessorNames", "masteringPairProcessorNames",
    "masteringPairAnalysisNames", "masteringStereoAnalysisNames", "masteringInsertNames",
    "masteringInsertParamNames", "masteringInsertParamInfo", "masteringProcessorCatalog",
    "mixingScenePresetNames", "mixingScenePresetJson",
    "realtimeVoiceChangerPresetNames", "realtimeVoiceChangerPresetJson",
    "validateRealtimeVoiceChangerPresetJson", "voiceCharacterPresetId",
    "realtimeVoiceChangerPresetConfig",
}
# These two functions were already object-shaped before this migration.  They
# have semantic options types rather than a newly named *Request interface.
OBJECT_SHAPED = {"synthesizeRir"}
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
    return None


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
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
