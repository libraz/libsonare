"""Report JS/TS one-shot exports that still lack a request-object overload.

This is deliberately a source-level guard: options/request reshapes are invisible
to the C-ABI parity checker.  It reports, rather than mutates, the public facade
inventory so the migration plan can be completed without losing functions.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from _repo import REPO_ROOT

SURFACES = {
    "node": REPO_ROOT / "bindings/node/src",
    "wasm": REPO_ROOT / "bindings/wasm/src",
}
# A leading underscore marks a module as internal: no `index.ts` re-exports one,
# so everything it exports is a helper shared between facades rather than a
# one-shot API a consumer can call. That is structural, so it is a prefix rule
# rather than a manifest -- splitting a facade creates such a module routinely,
# and a hand-listed name would turn every split into a coverage failure.
INTERNAL_PREFIX = "_"
EXCLUDED = {
    "errors.ts",
    "native.ts",
    "validation.ts",
    "value_coercion.ts",
    "codes.ts",
    "module_state.ts",
    "project_internal.ts",
    "analysis_helpers.ts",
    # Published type-only (`export type *`); its functions are worker-internal helpers.
    "vocal_edit_worker_protocol.ts",
}
# These are public exports, but deliberately outside the request-object policy:
# scalar/unit conversion helpers, discovery/version metadata, and factories for
# stateful or platform services.  Keep this manifest explicit: a newly exported
# one-shot audio operation must either gain a *Request overload or be reviewed
# here with a reason.
EXEMPT_FILES = {
    "project.ts",
    "vocal_project.ts",
    "project_synth.ts",
    "realtime_engine.ts",
    "synth_catalog.ts",
    "realtime_voice_changer.ts",
    "opfs_clip_pages.ts",
    "scale.ts",
    "stream_analyzer.ts",
    "web_midi.ts",
    "worker.ts",
    "vocal_edit_worker.ts",
}
SCALAR_HELPERS = {
    "hzToMel",
    "melToHz",
    "hzToMidi",
    "midiToHz",
    "tuningToReferenceHz",
    "referenceHzToTuning",
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
    "scaleMaskForMode",
    # Analysis-to-annotation vocabulary conversions: one chord or one mode in,
    # its annotation fields out, no audio and no options to name.
    "chordSymbolFromAnalysis",
    "keyModeFromAnalysis",
    # MIDI and GM vocabulary lookups: one number or one name in, its name or
    # number out, no event data and no settings to name.
    "gmInstrumentName",
    "gmProgramForName",
    "gmFamilyName",
    "gmFamilyFirstProgram",
    "gmDrumName",
    "gmDrumNoteForName",
    "gm2DrumSetName",
    "midiCcName",
    "midiCcIndexForName",
    "perNoteControllerName",
    # MIDI 1.0 <-> 2.0 value scalers: one integer in, one number out.
    "midiVelocityToMidi2",
    "midiNoteOnVelocityToMidi2",
    "midi2VelocityToMidi1",
    "midi2NoteOnVelocityToMidi1",
    "midiControlToMidi2",
    "midiControl14ToMidi2",
    "midi2ControlToMidi1",
    "midi2ControlToMidi1Control14",
    "midiPitchBendToMidi2",
    "midi2PitchBendToMidi1",
}
# Do not add a zero-argument accessor here: the zero-argument clause in
# exemption() already covers every one of them structurally, and it is the rule
# these names were spelling out by hand. This list is kept only so the entries
# that predate that clause keep their more specific reason string. A name
# belongs here only if it takes arguments and is still metadata.
METADATA_HELPERS = {
    "version",
    "capabilities",
    "capabilityCatalog",
    "abiVersion",
    "engineAbiVersion",
    "voiceChangerAbiVersion",
    "projectAbiVersion",
    "checkAbiVersion",
    "isInitialized",
    "engineCapabilities",
    "masteringPresetNames",
    "masteringPresetParams",
    "masteringProcessorNames",
    "masteringPairProcessorNames",
    "masteringPairAnalysisNames",
    "masteringStereoAnalysisNames",
    "masteringInsertNames",
    "masteringInsertParamNames",
    "masteringInsertParamInfo",
    # Catalog query beside the two above: it answers what a configured insert
    # would report and processes no audio.
    "masteringInsertTiming",
    "masteringProcessorCatalog",
    "mixingScenePresetNames",
    # Catalog read by one preset name; the document it returns is the catalog entry.
    "mixingScenePreset",
    # Name-to-index lookup over the mixing assistant's source taxonomy. A
    # request object around a single enum name would name nothing.
    "mixingAssistantSourceClassFromName",
    "realtimeVoiceChangerPresetNames",
    "realtimeVoiceChangerPresetJson",
    "realtimeVoiceChangerValidatePresetJson",
    "voiceCharacterPresetId",
    "realtimeVoiceChangerPresetConfig",
    # Document reader beside the preset validator above: one chain document in,
    # its config out, no audio and no settings to name.
    "masteringChainConfigFromJson",
}
# Channel-plane primitives beside `Audio.fromMemory(data)`: the decode takes the
# encoded bytes alone and the fold takes planes plus a layout code, both
# positional low-level building blocks with no options to name.
CHANNEL_PRIMITIVES = {"decodeChannels", "downmix"}
# These two functions were already object-shaped before this migration.  They
# have semantic options types rather than a newly named *Request interface.
OBJECT_SHAPED = {"synthesizeRir"}
# This attach helper coordinates pre-existing engine/streamer instances.  Its
# final argument is already the semantic options object; wrapping those opaque
# stateful handles in a one-shot request would only add a second API shape.
STATEFUL_ATTACH_HELPERS = {"attachOpfsClipStream"}
DECLARATION = re.compile(r"export function (\w+)\s*")


def _close_type_parameters(text: str, i: int) -> int:
    """Index just past the `>` closing the `<` at `i`; an arrow's `>` closes nothing."""
    depth = 0
    for j in range(i, len(text)):
        char = text[j]
        if char == "<":
            depth += 1
        elif char == ">" and text[j - 1] != "=":
            depth -= 1
            if depth == 0:
                return j + 1
    return -1


def _close_parameters(text: str, i: int) -> int:
    """Index of the `)` closing the `(` at `i`."""
    depth = 0
    for j in range(i, len(text)):
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
            if depth == 0:
                return j
    return -1


def declarations(text: str) -> list[tuple[str, str | None]]:
    """(name, parameter list) for every `export function`, type parameters skipped.

    The parameter list is None where the declaration cannot be read, so a caller
    treats it as unknown rather than as empty.
    """
    found: list[tuple[str, str | None]] = []
    for match in DECLARATION.finditer(text):
        i = match.end()
        if text.startswith("<", i):
            i = _close_type_parameters(text, i)
            if i < 0:
                found.append((match.group(1), None))
                continue
            while i < len(text) and text[i].isspace():
                i += 1
        if not text.startswith("(", i) or (end := _close_parameters(text, i)) < 0:
            found.append((match.group(1), None))
            continue
        found.append((match.group(1), text[i + 1 : end]))
    return found


def function_names(text: str) -> list[str]:
    names: list[str] = []
    for name, _ in declarations(text):
        if name not in names:
            names.append(name)
    return names


def functions(path: Path) -> list[str]:
    # Discovery/version/scalar helpers are not one-shot audio APIs. The remaining
    # candidates are reviewed through the migration manifest.
    return function_names(path.read_text())


CANONICAL_FIRST_PARAMETER = re.compile(r"\s*\w+\??\s*:\s*\w+Request\b")


def has_request_overload(text: str, name: str) -> bool:
    # The canonical overload takes its *Request as the first parameter. The
    # parameter is optional wherever every request field has a default, so `?`
    # is part of the canonical shape rather than a deviation from it.
    return any(
        params is not None and CANONICAL_FIRST_PARAMETER.match(params)
        for declared, params in declarations(text)
        if declared == name
    )


def takes_no_arguments(text: str, name: str) -> bool:
    """True when every exported declaration of `name` has an empty parameter list.

    Deliberately conservative: a signature the reader cannot parse counts as
    taking arguments, so an unreadable declaration is reviewed rather than
    exempted.
    """
    params = [p for declared, p in declarations(text) if declared == name]
    return bool(params) and all(p is not None and not p.strip() for p in params)


def exemption(path: Path, name: str, text: str) -> str | None:
    if path.name in EXEMPT_FILES:
        return "stateful/project/platform API"
    if name in SCALAR_HELPERS:
        return "scalar or small utility"
    if name in METADATA_HELPERS:
        return "catalog, capability, or version metadata"
    if name in CHANNEL_PRIMITIVES:
        return "low-level channel-plane primitive"
    if name in OBJECT_SHAPED:
        return "pre-existing object-shaped API"
    if name in STATEFUL_ATTACH_HELPERS:
        return "stateful attach helper with an existing options object"
    # A request object exists to name and order arguments; a function that takes
    # none has nothing to name, and wrapping it would produce `f({})`. This is
    # structural rather than a judgement about the individual function: a
    # one-shot audio operation takes at least a buffer, so a zero-argument
    # export is discovery or metadata by construction. Checked last so the named
    # sets above keep their existing, more specific reason strings.
    if takes_no_arguments(text, name):
        return "zero-argument accessor (no arguments to name)"
    return None


REQUEST = re.compile(r"\w+Request")
STAR_REEXPORT = re.compile(r"export\s+(?:type\s+)?\*\s+from\s+['\"]\./([\w/]+?)(?:\.js)?['\"]")
NAMED_REEXPORT = re.compile(
    r"export\s+(?:type\s+)?\{(.*?)\}\s*from\s*['\"]\./([\w/]+?)(?:\.js)?['\"]", re.DOTALL
)
NAMED_TYPE_EXPORT = re.compile(r"export\s+type\s*\{(.*?)\}", re.DOTALL)
EXPORTED_REQUEST_DEF = re.compile(r"export (?:interface|type) (\w+Request)\b")
EXPORTED_FUNCTION_PARAMS = re.compile(r"export function \w+\s*\(([^;{]*?)\)\s*:", re.DOTALL)


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
            if path.name.startswith(INTERNAL_PREFIX) or path.name in EXCLUDED:
                continue
            text = path.read_text()
            for name in functions(path):
                marker = f"{surface}:{path.name}:{name}"
                if has_request_overload(text, name):
                    covered.append(marker)
                elif reason := exemption(path, name, text):
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
    print(f"entry-export coverage: {len(unexported)} request types unexported from entry")
    for marker in unexported:
        print(f"UNEXPORTED {marker}")

    return 1 if (missing or unexported) else 0


if __name__ == "__main__":
    raise SystemExit(main())
