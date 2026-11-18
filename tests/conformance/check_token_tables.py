"""Check every facade's token table for a closed set against the core's own tokens.

A closed set the core writes by name (``to_string``) and reads back
(``<set>_from_string``) is spelled three more times on each surface: a runtime
table mapping token to the C ABI ordinal the scalar entry points take, sometimes
a token list indexed by that ordinal, and the declared union a type checker sees.
Each row below names those sources for one set. The written tokens are the set;
``from_string`` must accept exactly those plus the row's declared earlier forms,
so a third spelling fails. The C header's enumerators give each token's ordinal,
matched by name. A source that adds, drops or reorders a token fails with the
token named.

Document keys an earlier file form wrote follow the same rule (``KEY_FORMS``):
the core reader takes each beside its current key, nothing writes it, and the
only other places the old spelling may appear are the declared readers that
mirror the core's.

The rows are not the whole population. Every core function returning two or
more string literals is a writer, every facade literal union or literal-typed
field is a set, and a set drawing on a writer (``evaluate_population``) must be
pinned by a row or exempted with a reason, so a new set cannot go unchecked.

Run: python3 tests/conformance/check_token_tables.py
Self-tests: python3 -m unittest tests/conformance/test_token_tables.py
"""

from __future__ import annotations

import re
import sys
import types
import typing
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Source kinds: ordinal tables map token -> ordinal; set sources only list tokens.
# A field kind's symbol is ``Owner.field``: an interface member or a class attribute.
ORDINAL_KINDS = ("ts_object", "ts_array", "py_tuple")
SET_KINDS = ("ts_union", "ts_field", "py_literal", "py_field")


class Source(typing.NamedTuple):
    surface: str
    path: str
    kind: str
    symbol: str
    # A deliberate restriction of the set: it may omit tokens, never add one.
    subset: bool = False


class CoreTable(typing.NamedTuple):
    path: str
    enum: str  # C++ type whose writer overload spells the tokens
    from_string: str | None = None  # reader comparing against literals; None when it loops the writer
    writer: str = "to_string"


class TokenSet(typing.NamedTuple):
    name: str
    core: CoreTable
    sources: tuple[Source, ...]
    c_enum: tuple[str, str] | None = None  # (header, enumerator prefix)
    # Token -> enumerator suffix, only where the names do not normalize alike.
    c_aliases: typing.Mapping[str, str] = types.MappingProxyType({})
    # Spellings an earlier file form wrote, still read and never written.
    earlier_forms: tuple[str, ...] = ()
    # Further core writers of the same set, each required to write the same tokens.
    also_written_by: tuple[CoreTable, ...] = ()


def _src(surface: str, path: str, kind: str, symbol: str, *, subset: bool = False) -> Source:
    return Source(surface, path, kind, symbol, subset)


ROWS: tuple[TokenSet, ...] = (
    TokenSet(
        name="pan law",
        core=CoreTable("src/mixing/api/scene_json.cpp", "PanLaw", "pan_law_from_string"),
        c_enum=("include/sonare/sonare_c_mixing.h", "SONARE_PAN_LAW_"),
        c_aliases={"const4.5Db": "CONST_4P5DB"},
        sources=(
            _src("node", "bindings/node/src/value_coercion.ts", "ts_object", "PAN_LAW_VALUES"),
            _src("node", "bindings/node/src/types_mixing.ts", "ts_union", "PanLaw"),
            _src("wasm", "bindings/wasm/src/codes.ts", "ts_object", "PAN_LAW_VALUES"),
            _src("wasm", "bindings/wasm/src/public_types_mixing.ts", "ts_union", "PanLaw"),
            _src("python", "bindings/python/src/libsonare/_types_enums.py", "py_literal", "PanLaw"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "PanLaw"),
            _src(
                "python", "bindings/python/src/libsonare/_types_enums.py", "py_tuple", "_PAN_LAWS"
            ),
        ),
    ),
    TokenSet(
        name="pan mode",
        core=CoreTable("src/mixing/api/scene_json.cpp", "PanMode", "pan_mode_from_string"),
        c_enum=("include/sonare/sonare_c_mixing.h", "SONARE_PAN_MODE_"),
        sources=(
            _src("node", "bindings/node/src/value_coercion.ts", "ts_object", "PAN_MODE_VALUES"),
            _src("node", "bindings/node/src/types_mixing.ts", "ts_union", "PanMode"),
            _src("wasm", "bindings/wasm/src/codes.ts", "ts_object", "PAN_MODE_VALUES"),
            _src("wasm", "bindings/wasm/src/public_types_mixing.ts", "ts_union", "PanMode"),
            _src(
                "python", "bindings/python/src/libsonare/_types_enums.py", "py_literal", "PanMode"
            ),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "PanMode"),
            _src(
                "python", "bindings/python/src/libsonare/_types_enums.py", "py_tuple", "_PAN_MODES"
            ),
        ),
    ),
    TokenSet(
        name="send timing",
        core=CoreTable("src/mixing/api/scene_json.cpp", "SendTiming", "send_timing_from_string"),
        earlier_forms=("pre", "post"),
        c_enum=("include/sonare/sonare_c_mixing.h", "SONARE_SEND_TIMING_"),
        sources=(
            _src("node", "bindings/node/src/value_coercion.ts", "ts_object", "SEND_TIMING_VALUES"),
            _src("node", "bindings/node/src/types_mixing.ts", "ts_union", "SendTiming"),
            _src("wasm", "bindings/wasm/src/codes.ts", "ts_object", "SEND_TIMING_VALUES"),
            _src("wasm", "bindings/wasm/src/public_types_mixing.ts", "ts_union", "SendTiming"),
            _src(
                "python",
                "bindings/python/src/libsonare/_types_enums.py",
                "py_literal",
                "SendTiming",
            ),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "SendTiming"),
            _src(
                "python",
                "bindings/python/src/libsonare/_types_enums.py",
                "py_tuple",
                "_SEND_TIMINGS",
            ),
        ),
    ),
    TokenSet(
        name="synth condition relation",
        core=CoreTable("src/midi/synth/patch_tuning.cpp", "ActiveCondition::Relation", None, "active_relation_name"),
        sources=(
            _src("node", "bindings/node/src/types_instrument.ts", "ts_field", "SynthParamCondition.relation"),
            _src("python", "bindings/python/src/libsonare/_types_capabilities.py", "py_field", "SynthParamCondition.relation"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_field", "SynthParamCondition.relation"),
            _src("wasm", "bindings/wasm/src/instrument_types.ts", "ts_field", "SynthParamCondition.relation"),
        ),
    ),
    TokenSet(
        name="synth condition scope",
        core=CoreTable("src/midi/synth/patch_tuning.cpp", "ActiveCondition::Scope", None, "active_scope_name"),
        sources=(
            _src("node", "bindings/node/src/types_instrument.ts", "ts_field", "SynthParamCondition.scope"),
            _src("python", "bindings/python/src/libsonare/_types_capabilities.py", "py_field", "SynthParamCondition.scope"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_field", "SynthParamCondition.scope"),
            _src("wasm", "bindings/wasm/src/instrument_types.ts", "ts_field", "SynthParamCondition.scope"),
        ),
    ),
    TokenSet(
        name="eq band type",
        core=CoreTable("src/mastering/eq/eq_band_value.cpp", "EqBandType", None, "band_type_to_string"),
        sources=(
            _src("wasm", "bindings/wasm/src/public_types_realtime.ts", "ts_union", "EqBandType"),
        ),
    ),
    TokenSet(
        name="bpm candidate relation",
        core=CoreTable("src/analysis/analysis_json.cpp", "BpmCandidateRelation", None, "bpm_candidate_relation_name"),
        sources=(
            _src("node", "bindings/node/src/types_analysis.ts", "ts_union", "BpmCandidateRelation"),
            _src("node", "bindings/node/src/types_project.ts", "ts_field", "ProjectTempoCandidate.label", subset=True),
            _src("python", "bindings/python/src/libsonare/_types_analysis.py", "py_literal", "BpmCandidateRelation"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "BpmCandidateRelation"),
            _src("wasm", "bindings/wasm/src/project_internal.ts", "ts_field", "NativeProjectTempoCandidate.label", subset=True),
            _src("wasm", "bindings/wasm/src/project_types.ts", "ts_field", "ProjectTempoCandidate.label", subset=True),
            _src("wasm", "bindings/wasm/src/public_types_music.ts", "ts_union", "BpmCandidateRelation"),
        ),
    ),
    TokenSet(
        name="channel layout",
        core=CoreTable("src/core/channel_layout.h", "ChannelLayout", "channel_layout_from_string", "channel_layout_to_string"),
        also_written_by=(
            CoreTable("src/playback/config.cpp", "ChannelLayout", None, "bed_layout_name"),
            CoreTable("src/playback/renderer.cpp", "ChannelLayout", None, "layout_text"),
        ),
        sources=(
            _src("node", "bindings/node/src/types_engine.ts", "ts_union", "ChannelLayout"),
            _src("node", "bindings/node/src/types_playback.ts", "ts_field", "PlaybackDiagnostics.active_input_layout"),
            _src("node", "bindings/node/src/types_playback.ts", "ts_field", "PlaybackTargetConfig.layout", subset=True),
            _src("node", "bindings/node/src/types_project.ts", "ts_union", "ProjectStemLayout", subset=True),
            _src("python", "bindings/python/src/libsonare/_project_model.py", "py_literal", "ProjectStemLayout", subset=True),
            _src("python", "bindings/python/src/libsonare/_types_enums.py", "py_literal", "ChannelLayout"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "ChannelLayout"),
            _src("wasm", "bindings/wasm/src/audio.ts", "ts_union", "ChannelLayout"),
            _src("wasm", "bindings/wasm/src/project_types.ts", "ts_union", "ProjectStemLayout", subset=True),
            _src("wasm", "bindings/wasm/src/public_types_playback.ts", "ts_field", "PlaybackDiagnostics.active_input_layout"),
            _src("wasm", "bindings/wasm/src/public_types_playback.ts", "ts_field", "PlaybackTargetConfig.layout", subset=True),
        ),
    ),
    TokenSet(
        name="mastering channel policy",
        core=CoreTable("src/mastering/api/named_processor_registry.cpp", "ChannelPolicy", None, "channel_policy_to_string"),
        sources=(
            _src("node", "bindings/node/src/types_capabilities.ts", "ts_union", "MasteringChannelPolicy"),
            _src("python", "bindings/python/src/libsonare/_types_capabilities.py", "py_literal", "MasteringChannelPolicy"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "MasteringChannelPolicy"),
            _src("wasm", "bindings/wasm/src/public_types.ts", "ts_union", "MasteringChannelPolicy"),
        ),
    ),
    TokenSet(
        name="eq coefficient mode",
        core=CoreTable("src/mastering/eq/eq_band_value.cpp", "BiquadCoeffMode", None, "coeff_mode_to_string"),
        sources=(
            _src("wasm", "bindings/wasm/src/public_types_realtime.ts", "ts_union", "EqCoeffMode"),
        ),
    ),
    TokenSet(
        name="formant mode",
        core=CoreTable("src/editing/voice_changer/voice_changer.cpp", "FormantMode", None, "formant_mode_name"),
        sources=(
            _src("node", "bindings/node/src/types_mastering.ts", "ts_field", "MasteringProcessorParamMap.formantMode"),
            _src("node", "bindings/node/src/voice_changer.ts", "ts_union", "FormantMode"),
            _src("python", "bindings/python/src/libsonare/analyzer.pyi", "py_field", "RealtimeVoiceChangerConfig.formant_mode"),
            _src("python", "bindings/python/src/libsonare/audio.pyi", "py_field", "Audio.formant_mode"),
            _src("wasm", "bindings/wasm/src/effects_voice_change.ts", "ts_union", "FormantMode"),
            _src("wasm", "bindings/wasm/src/public_types_mastering.ts", "ts_field", "MasteringProcessorParamMap.formantMode"),
        ),
    ),
    TokenSet(
        name="playback input layout",
        core=CoreTable("src/playback/config.cpp", "InputLayout", None, "input_layout_name"),
        sources=(
            _src("node", "bindings/node/src/types_playback.ts", "ts_field", "PlaybackInputConfig.layout"),
            _src("wasm", "bindings/wasm/src/public_types_playback.ts", "ts_field", "PlaybackInputConfig.layout"),
        ),
    ),
    TokenSet(
        name="key mode",
        core=CoreTable("src/util/types.h", "Mode", None, "mode_name"),
        sources=(
            _src("node", "bindings/node/src/types_analysis.ts", "ts_union", "KeyMode", subset=True),
            _src("node", "bindings/node/src/types_project.ts", "ts_union", "ProjectKeyMode"),
            _src("python", "bindings/python/src/libsonare/_project_model.py", "py_literal", "ProjectKeyMode"),
            _src("python", "bindings/python/src/libsonare/_project_model.py", "py_tuple", "_PROJECT_KEY_MODES"),
            _src("python", "bindings/python/src/libsonare/_types_enums.py", "py_literal", "KeyMode", subset=True),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "KeyMode", subset=True),
            _src("wasm", "bindings/wasm/src/public_types_music.ts", "ts_union", "KeyMode", subset=True),
        ),
    ),
    TokenSet(
        name="eq band phase",
        core=CoreTable("src/mastering/eq/eq_band_value.cpp", "PhaseMode", None, "phase_mode_to_string"),
        sources=(
            _src("wasm", "bindings/wasm/src/public_types_realtime.ts", "ts_union", "EqBandPhase"),
        ),
    ),
    TokenSet(
        name="mastering preset",
        core=CoreTable("src/mastering/api/presets.cpp", "Preset", None, "preset_to_string"),
        sources=(
            _src("node", "bindings/node/src/types_mastering.ts", "ts_union", "MasteringPreset", subset=True),
            _src("python", "bindings/python/src/libsonare/analyzer.pyi", "py_literal", "MasteringPreset", subset=True),
            _src("wasm", "bindings/wasm/src/public_types_mastering.ts", "ts_union", "MasteringPreset", subset=True),
        ),
    ),
    TokenSet(
        name="insert parameter relation",
        core=CoreTable("src/mastering/api/param_meta.h", "Relation", None, "relation_name"),
        sources=(
            _src("node", "bindings/node/src/types_capabilities.ts", "ts_union", "MasteringInsertParamRelation"),
            _src("python", "bindings/python/src/libsonare/_types_capabilities.py", "py_literal", "MasteringInsertParamRelation"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "MasteringInsertParamRelation"),
            _src("wasm", "bindings/wasm/src/public_types.ts", "ts_union", "MasteringInsertParamRelation"),
        ),
    ),
    TokenSet(
        name="playback room preset",
        core=CoreTable("src/playback/config.cpp", "RoomPreset", None, "room_preset_name"),
        sources=(
            _src("node", "bindings/node/src/types_playback.ts", "ts_field", "PlaybackRoomConfig.preset"),
            _src("wasm", "bindings/wasm/src/public_types_playback.ts", "ts_field", "PlaybackRoomConfig.preset"),
        ),
    ),
    TokenSet(
        name="playback speaker role",
        core=CoreTable("src/playback/config.h", "SpeakerRole", None, "speaker_role_name"),
        sources=(
            _src("node", "bindings/node/src/types_playback.ts", "ts_union", "PlaybackChannelRole"),
            _src("wasm", "bindings/wasm/src/public_types_playback.ts", "ts_union", "PlaybackChannelRole"),
        ),
    ),
    TokenSet(
        name="repair stage",
        core=CoreTable("src/mastering/assistant/repair_session.cpp", "Stage", None, "stage_name"),
        sources=(
            _src("node", "bindings/node/src/repair_session.ts", "ts_union", "MasteringRepairStageName"),
            _src("python", "bindings/python/src/libsonare/_types_repair.py", "py_literal", "RepairStageName"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "RepairStageName"),
            _src("wasm", "bindings/wasm/src/repair_session.ts", "ts_union", "MasteringRepairStageName"),
        ),
    ),
    TokenSet(
        name="playback stage",
        core=CoreTable("src/playback/config.h", "Stage", None, "stage_name"),
        sources=(
            _src("node", "bindings/node/src/types_playback.ts", "ts_union", "PlaybackStageName"),
            _src("wasm", "bindings/wasm/src/public_types_playback.ts", "ts_union", "PlaybackStageName"),
        ),
    ),
    TokenSet(
        name="insert parameter unit",
        core=CoreTable("src/mastering/api/param_meta.h", "Unit", None, "unit_name"),
        sources=(
            _src("node", "bindings/node/src/types_capabilities.ts", "ts_union", "MasteringInsertParamUnit"),
            _src("python", "bindings/python/src/libsonare/_types_capabilities.py", "py_literal", "MasteringInsertParamUnit"),
            _src("python", "bindings/python/src/libsonare/types.pyi", "py_literal", "MasteringInsertParamUnit"),
            _src("wasm", "bindings/wasm/src/public_types.ts", "ts_union", "MasteringInsertParamUnit"),
        ),
    ),
)



class KeyForm(typing.NamedTuple):
    """A document key an earlier file form wrote, still read and never written."""

    name: str
    core: str  # reader and writer of the document
    current: str
    earlier: str
    mirrors: tuple[str, ...] = ()  # TS readers that must take both spellings as the core does


_SCENE = "src/mixing/api/scene_json.cpp"
_WORKLET_MIRROR = ("bindings/wasm/src/worklet/engine-mixer-facade.ts",)

KEY_FORMS: tuple[KeyForm, ...] = (
    KeyForm("scene insert processor", _SCENE, "processor", "processor_name", _WORKLET_MIRROR),
    KeyForm("scene insert params", _SCENE, "params", "params_json", _WORKLET_MIRROR),
    KeyForm("scene insert sidechain", _SCENE, "sidechainKey", "sidechain_key", _WORKLET_MIRROR),
)

# Trees an earlier key may be written from.
KEY_SWEEP_ROOTS = ("src", "include", "bindings/node/src", "bindings/wasm/src", "bindings/python/src")
KEY_SWEEP_SUFFIXES = (".cpp", ".h", ".ts", ".py", ".pyi")


def _core_reads(form: KeyForm) -> re.Pattern[str]:
    return re.compile(
        rf'_or_legacy\([^;]*?"{re.escape(form.current)}"\s*,\s*"{re.escape(form.earlier)}"'
    )


def _mirror_reads(form: KeyForm) -> re.Pattern[str]:
    return re.compile(
        rf"aliasedValue\(\s*\w+\s*,\s*'{re.escape(form.current)}'\s*,\s*'{re.escape(form.earlier)}'\s*\)"
    )


def _writes(key: str) -> re.Pattern[str]:
    return re.compile(rf'\bput\(\s*\w+\s*,\s*"{re.escape(key)}"')


def evaluate_key_forms(
    root: Path, forms: tuple[KeyForm, ...] = KEY_FORMS
) -> tuple[list[str], int, int]:
    """Problems, forms checked and reader sites matched."""
    problems: list[str] = []
    matched = 0
    sweep = [
        f
        for base in KEY_SWEEP_ROOTS
        if (root / base).is_dir()
        for f in sorted((root / base).rglob("*"))
        if f.suffix in KEY_SWEEP_SUFFIXES and not f.name.endswith(".d.ts")
    ]
    texts = {f: f.read_text(encoding="utf-8", errors="replace") for f in sweep}
    for form in forms:
        core_path = root / form.core
        core = core_path.read_text(encoding="utf-8") if core_path.is_file() else ""
        if not _core_reads(form).search(core):
            problems.append(f"{form.name}: {form.core} does not read {form.earlier!r} beside {form.current!r}")
        else:
            matched += 1
        if not re.search(rf'known_keys\([^;]*"{re.escape(form.earlier)}"', core):
            problems.append(f"{form.name}: {form.core} does not declare {form.earlier!r} a known key")
        if not _writes(form.current).search(core):
            problems.append(f"{form.name}: {form.core} writes no {form.current!r}")
        for mirror in form.mirrors:
            path = root / mirror
            text = path.read_text(encoding="utf-8") if path.is_file() else ""
            if _mirror_reads(form).search(text):
                matched += 1
            else:
                problems.append(f"{form.name}: {mirror} does not read {form.earlier!r} beside {form.current!r}")
        quoted = re.compile(rf"""(['"]){re.escape(form.earlier)}\1""")
        readers = (_core_reads(form), _mirror_reads(form), re.compile(r"known_keys\("))
        for f, text in texts.items():
            for i, line in enumerate(text.splitlines(), 1):
                if quoted.search(line) and not any(r.search(line) for r in readers):
                    rel = f.relative_to(root)
                    problems.append(f"{form.name}: {form.earlier!r} appears outside a reader at {rel}:{i}")
    return problems, len(forms), matched


_STR = re.compile(r"""(['"])((?:(?!\1).)+)\1""")


def _norm(name: str) -> str:
    return re.sub(r"[^0-9a-z]", "", name.lower())


def _balanced(text: str, start: int, open_ch: str, close_ch: str) -> str | None:
    """The text between the bracket at or after ``start`` and its match."""
    i = text.find(open_ch, start)
    if i < 0:
        return None
    depth = 0
    for j in range(i, len(text)):
        if text[j] == open_ch:
            depth += 1
        elif text[j] == close_ch:
            depth -= 1
            if depth == 0:
                return text[i + 1 : j]
    return None


def _function_body(text: str, head: re.Pattern[str]) -> str | None:
    m = head.search(text)
    if m is None:
        return None
    return _balanced(text, text.find("{", m.end()), "{", "}")


def read_tokens(text: str, function: str) -> list[str] | None:
    """Tokens a ``from_string`` function accepts, in source order."""
    body = _function_body(text, re.compile(rf"\b{re.escape(function)}\s*\("))
    return (re.findall(r'==\s*"([^"]+)"', body) or None) if body else None


def written_tokens(text: str, enum: str, writer: str = "to_string") -> list[str] | None:
    """Tokens the ``writer`` overload for ``enum`` returns, in source order."""
    head = re.compile(rf"\b{re.escape(writer)}\s*\(\s*(?:const\s+)?{re.escape(enum)}\b")
    body = _function_body(text, head)
    if not body:
        return None
    tokens = [t for r in re.findall(r"\breturn\b([^;]*);", body) for _q, t in _STR.findall(r)]
    return tokens or None


def c_enumerators(text: str, prefix: str) -> dict[str, int]:
    """Enumerator suffix -> value for every ``<prefix>NAME = n``."""
    return {
        m.group(1): int(m.group(2))
        for m in re.finditer(rf"\b{re.escape(prefix)}(\w+)\s*=\s*(-?\d+)", text)
    }


def read_source(text: str, src: Source) -> dict[str, int] | set[str] | None:
    """An ordinal kind as token -> ordinal, a set kind as a token set; None when absent."""
    sym = re.escape(src.symbol)
    if src.kind == "ts_object":
        m = re.search(rf"\bconst\s+{sym}\b[^=]*=", text)
        body = m and _balanced(text, m.end(), "{", "}")
        if not body:
            return None
        return {
            e.group(2) or e.group(3): int(e.group(4))
            for e in re.finditer(r"""(?:(['"])([^'"]+)\1|([\w$]+))\s*:\s*(-?\d+)""", body)
        }
    if src.kind == "ts_array":
        m = re.search(rf"\bconst\s+{sym}\b[^=]*=", text)
        body = m and _balanced(text, m.end(), "[", "]")
        if body is None or body is False:
            return None
        return {tok: i for i, (_q, tok) in enumerate(_STR.findall(body))}
    if src.kind == "ts_union":
        m = re.search(rf"\btype\s+{sym}\s*=([^;]*);", text)
        return {tok for _q, tok in _STR.findall(m.group(1))} if m else None
    if src.kind == "ts_field":
        owner, _, field = src.symbol.partition(".")
        m = re.search(rf"\b(?:interface\s+{re.escape(owner)}\b[^{{]*|type\s+{re.escape(owner)}\s*=\s*)\{{", text)
        body = m and _balanced(text, m.end() - 1, "{", "}")
        f = body and re.search(rf"^\s*(?:readonly\s+)?{re.escape(field)}\??\s*:\s*([^;]*);", body, re.MULTILINE)
        return {tok for _q, tok in _STR.findall(f.group(1))} if f else None
    if src.kind == "py_field":
        owner, _, field = src.symbol.partition(".")
        m = re.search(rf"^class\s+{re.escape(owner)}\b[^\n]*\n((?:[ \t]+[^\n]*\n|\n)+)", text, re.MULTILINE)
        f = m and re.search(rf"^[ \t]+{re.escape(field)}\s*:\s*(?:Optional\[)?Literal\[", m.group(1), re.MULTILINE)
        body = f and _balanced(m.group(1), f.end() - 1, "[", "]")
        return {tok for _q, tok in _STR.findall(body)} if body else None
    if src.kind == "py_literal":
        m = re.search(rf"^{sym}\s*(?::[^=\n]*)?=\s*Literal\[", text, re.MULTILINE)
        body = m and _balanced(text, m.end() - 1, "[", "]")
        return {tok for _q, tok in _STR.findall(body)} if body else None
    if src.kind == "py_tuple":
        m = re.search(rf"^{sym}\s*(?::[^=]*)?=\s*(.*)$", text, re.MULTILINE)
        if m is None:
            return None
        rhs = m.group(1)
        args = re.match(r"get_args\((\w+)\)", rhs)
        if args:
            lit = re.search(rf"^{args.group(1)}\s*(?::[^=\n]*)?=\s*Literal\[", text, re.MULTILINE)
            body = lit and _balanced(text, lit.end() - 1, "[", "]")
        else:
            body = _balanced(text, m.start(1), "(", ")")
        if not body:
            return None
        return {tok: i for i, (_q, tok) in enumerate(_STR.findall(body))}
    raise ValueError(f"unknown source kind {src.kind!r}")


def evaluate(root: Path, rows: tuple[TokenSet, ...] = ROWS) -> tuple[list[str], int, int]:
    """Problems, rows checked and sources compared."""
    problems: list[str] = []
    compared = 0

    def read(path: str) -> str | None:
        p = root / path
        return p.read_text(encoding="utf-8") if p.is_file() else None

    for row in rows:
        core_text = read(row.core.path) or ""
        tokens = written_tokens(core_text, row.core.enum, row.core.writer)
        if not tokens:
            problems.append(
                f"{row.name}: no tokens found in {row.core.writer}({row.core.enum}) ({row.core.path})"
            )
            continue
        legacy_writer = f"legacy_{row.core.writer}"
        legacy = written_tokens(core_text, row.core.enum, legacy_writer) or []
        earlier = set(row.earlier_forms) | (set(legacy) - set(tokens))
        for tok in sorted(earlier & set(tokens)):
            problems.append(f"{row.name}: earlier form {tok!r} is still written")
        if row.core.from_string is not None:
            accepted = read_tokens(core_text, row.core.from_string)
            if not accepted:
                problems.append(
                    f"{row.name}: no tokens found in {row.core.from_string} ({row.core.path})"
                )
                continue
            for tok in sorted(set(accepted) - set(tokens) - earlier):
                problems.append(f"{row.name}: {row.core.from_string} accepts undeclared {tok!r}")
            for tok in sorted((set(tokens) | earlier) - set(accepted)):
                problems.append(f"{row.name}: {row.core.from_string} does not accept {tok!r}")
        elif legacy and not re.search(rf"==\s*{re.escape(legacy_writer)}\s*\(", core_text):
            problems.append(f"{row.name}: {row.core.path} does not read what {legacy_writer} writes")
        for other in row.also_written_by:
            other_tokens = written_tokens(read(other.path) or "", other.enum, other.writer)
            if other_tokens is None:
                problems.append(f"{row.name}: no tokens found in {other.writer}({other.enum}) ({other.path})")
            elif set(other_tokens) != set(tokens):
                problems.append(
                    f"{row.name}: {other.writer} ({other.path}) writes {sorted(set(other_tokens))}, "
                    f"{row.core.writer} writes {sorted(set(tokens))}"
                )
        expected: dict[str, int] = {}
        if row.c_enum is not None:
            header = read(row.c_enum[0]) or ""
            enumerators = c_enumerators(header, row.c_enum[1])
            by_norm = {_norm(k): v for k, v in enumerators.items()}
            for tok in tokens:
                key = _norm(row.c_aliases.get(tok, tok))
                if key not in by_norm:
                    problems.append(
                        f"{row.name}: core token {tok!r} matches no {row.c_enum[1]}* enumerator"
                    )
                    continue
                expected[tok] = by_norm[key]
            unmatched = sorted(set(enumerators.values()) - set(expected.values()))
            if unmatched:
                problems.append(
                    f"{row.name}: {row.c_enum[1]}* ordinals {unmatched} carry no core token"
                )
        for src in row.sources:
            text = read(src.path)
            got = read_source(text, src) if text is not None else None
            where = f"{row.name}: {src.surface} {src.symbol} ({src.path})"
            if got is None:
                problems.append(f"{where}: not found")
                continue
            compared += 1
            names = set(got)
            for tok in sorted(names - set(tokens)):
                problems.append(f"{where}: extra token {tok!r}")
            for tok in sorted(set(tokens) - names) if not src.subset else ():
                problems.append(f"{where}: missing token {tok!r}")
            if isinstance(got, dict):
                for tok in sorted(names & set(expected)):
                    if got[tok] != expected[tok]:
                        problems.append(
                            f"{where}: token {tok!r} has ordinal {got[tok]}, the C ABI's is {expected[tok]}"
                        )
    return problems, len(rows), compared


# A facade literal set draws on a core writer when it shares two or more of the
# writer's tokens (current or earlier), spells nothing else, and no other such
# writer comes closer. Every set that draws on a writer is pinned by a row or
# exempted here, with the reason.
EXEMPT: typing.Mapping[tuple[str, str], str] = types.MappingProxyType(
    {
        (path, "MeterTap"): (
            "no core writer: the SonareMeterTap ordinals are its only core spelling, "
            "and it shares the send timing tokens by name only"
        )
        for path in (
            "bindings/node/src/types_mixing.ts",
            "bindings/wasm/src/public_types_mixing.ts",
            "bindings/python/src/libsonare/_types_enums.py",
            "bindings/python/src/libsonare/types.pyi",
        )
    }
)

POPULATION_ROOTS = {
    "core": ("src",),
    "ts": ("bindings/node/src", "bindings/wasm/src"),
    "py": ("bindings/python/src",),
}

_WRITER_HEAD = re.compile(
    r"(?:const\s+char\s*\*|std::string(?:_view)?)\s+(\w+)\s*\(\s*(?:const\s+)?"
    r"([A-Z][\w:]*)\s*&?\s*\w*\s*\)[^;{}]*\{"
)
_LIT = r"""(?:'[^'\n]+'|"[^"\n]+")"""
_LITERAL_UNION = re.compile(rf"^\s*\|?\s*{_LIT}(?:\s*\|\s*{_LIT})+\s*$")


def core_writers(root: Path) -> dict[tuple[str, str, str], set[str]]:
    """(path, writer, enum) -> tokens, for every function returning two or more literals."""
    found: dict[tuple[str, str, str], set[str]] = {}
    for base in POPULATION_ROOTS["core"]:
        for f in sorted((root / base).rglob("*")):
            if f.suffix not in (".cpp", ".h"):
                continue
            text = f.read_text(encoding="utf-8", errors="replace")
            for m in _WRITER_HEAD.finditer(text):
                body = _balanced(text, m.end() - 1, "{", "}") or ""
                toks = {t for r in re.findall(r"\breturn\b([^;]*);", body) for _q, t in _STR.findall(r)}
                if len(toks) >= 2:
                    found[(f.relative_to(root).as_posix(), m.group(1), m.group(2))] = toks
    return found


def facade_sets(root: Path) -> dict[tuple[str, str], set[str]]:
    """(path, symbol) -> tokens, for every literal union a facade declares."""
    found: dict[tuple[str, str], set[str]] = {}

    def literals(text: str) -> set[str]:
        return {t for _q, t in _STR.findall(text)}

    for base in POPULATION_ROOTS["ts"]:
        for f in sorted((root / base).rglob("*.ts")):
            if f.name.endswith(".d.ts"):
                continue
            rel = f.relative_to(root).as_posix()
            text = f.read_text(encoding="utf-8", errors="replace")
            for m in re.finditer(r"\btype\s+(\w+)\s*=([^;]*);", text):
                if _LITERAL_UNION.match(m.group(2).replace("\n", " ")):
                    found[(rel, m.group(1))] = literals(m.group(2))
            for m in re.finditer(r"\binterface\s+(\w+)[^{]*\{", text):
                body = _balanced(text, m.end() - 1, "{", "}") or ""
                for fm in re.finditer(r"^\s*(?:readonly\s+)?(\w+)\??\s*:\s*([^;]*);", body, re.MULTILINE):
                    if _LITERAL_UNION.match(fm.group(2).replace("\n", " ")):
                        found[(rel, f"{m.group(1)}.{fm.group(1)}")] = literals(fm.group(2))
    for base in POPULATION_ROOTS["py"]:
        for f in sorted((root / base).rglob("*.py*")):
            if f.suffix not in (".py", ".pyi"):
                continue
            rel = f.relative_to(root).as_posix()
            text = f.read_text(encoding="utf-8", errors="replace")
            for m in re.finditer(r"^(\w+)\s*(?::[^=\n]*)?=\s*Literal\[", text, re.MULTILINE):
                found[(rel, m.group(1))] = literals(_balanced(text, m.end() - 1, "[", "]") or "")
            for m in re.finditer(r"^class\s+(\w+)[^\n]*\n((?:[ \t]+[^\n]*\n|\n)+)", text, re.MULTILINE):
                block = m.group(2)
                for fm in re.finditer(r"^[ \t]+(\w+)\s*:\s*(?:Optional\[)?Literal\[", block, re.MULTILINE):
                    body = _balanced(block, fm.end() - 1, "[", "]") or ""
                    found.setdefault((rel, f"{m.group(1)}.{fm.group(1)}"), literals(body))
    return {k: v for k, v in found.items() if len(v) >= 2}


def evaluate_population(
    root: Path,
    rows: tuple[TokenSet, ...] = ROWS,
    exempt: typing.Mapping[tuple[str, str], str] = EXEMPT,
) -> tuple[list[str], int, int]:
    """Problems, facade sets that draw on a core writer, and writers drawn on."""
    problems: list[str] = []
    writers = core_writers(root)
    legacy = {
        (p, w[len("legacy_") :], e): toks for (p, w, e), toks in writers.items() if w.startswith("legacy_")
    }
    current = {k: v for k, v in writers.items() if not k[1].startswith("legacy_")}
    pinned = {(s.path, s.symbol) for row in rows for s in row.sources}
    named = {(t.path, t.writer, t.enum) for row in rows for t in (row.core, *row.also_written_by)}
    drawing: dict[tuple[str, str], list[tuple[str, str, str]]] = {}
    for key, toks in facade_sets(root).items():
        # The closest writers: those whose set exceeds this one by the fewest tokens.
        candidates: dict[tuple[str, str, str], int] = {}
        for wkey, wtoks in current.items():
            allowed = wtoks | legacy.get(wkey, set())
            if len(toks & allowed) >= 2 and toks <= allowed:
                candidates[wkey] = len(wtoks - toks)
        if candidates:
            closest = min(candidates.values())
            drawing[key] = [w for w, extra in candidates.items() if extra == closest]
    drawn: set[tuple[str, str, str]] = set()
    for (path, symbol), wkeys in sorted(drawing.items()):
        names = ", ".join(f"{w}({e}) in {p}" for p, w, e in sorted(wkeys))
        if (path, symbol) in exempt:
            continue
        drawn.update(wkeys)
        if (path, symbol) not in pinned:
            problems.append(f"{symbol} ({path}) spells the tokens of {names} and no row pins it")
    for p, w, e in sorted(drawn - named):
        problems.append(f"{w}({e}) in {p} writes a set a facade spells and no row names it")
    for path, symbol in sorted(set(exempt) - set(drawing)):
        problems.append(f"exemption for {symbol} ({path}) excuses nothing")
    return problems, len(drawing), len(drawn)


def main() -> int:
    problems, rows, compared = evaluate(ROOT)
    print(f"token tables: {rows} rows, {compared} sources compared")
    if rows == 0 or compared == 0:
        print("FAIL: nothing was compared")
        return 1
    key_problems, forms, readers = evaluate_key_forms(ROOT)
    print(f"earlier document keys: {forms} keys, {readers} reader sites matched")
    if forms == 0 or readers == 0:
        print("FAIL: no earlier document key was compared")
        return 1
    problems += key_problems
    pop_problems, sets, writers = evaluate_population(ROOT)
    print(f"population: {sets} facade sets draw on {writers} core writers")
    if sets == 0 or writers == 0:
        print("FAIL: the population search found no facade set drawing on a core writer")
        return 1
    problems += pop_problems
    for p in problems:
        print(f"FAIL: {p}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
