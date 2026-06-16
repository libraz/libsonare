"""Internal NativeSynth patch enum-name tables and coercion helpers.

These map the :class:`SynthPatch` / :class:`Project` facade's string enum
spellings to the integer ordinals the C ABI expects, and expose the canonical
enum-name tables. Pure helpers over the loaded library; consumed by
``_project.py`` and re-exported from it (``SYNTH_ENUM_TABLES`` /
``synth_enum_tables``) -- not a public module in its own right.
"""

from __future__ import annotations

from collections.abc import Mapping

from ._runtime import _get_lib

# NativeSynth patch enum names (mirror the SonareSynth* enums in
# sonare_c_types.h; 0 / "default" keeps the base patch's value).
_SYNTH_ENGINE_MODES = {
    "default": 0,
    "subtractive": 1,
    "fm": 2,
    "karplus-strong": 3,
    "modal": 4,
    "additive": 5,
    "percussion": 6,
    "piano": 7,
}
_SYNTH_OSC_WAVEFORMS = {
    "default": 0,
    "sine": 1,
    "saw": 2,
    "square": 3,
    "triangle": 4,
    "noise": 5,
}
_SYNTH_FILTER_MODELS = {
    "default": 0,
    "svf": 1,
    "moog-ladder": 2,
    "diode-ladder": 3,
    "sallen-key": 4,
}
_SYNTH_FILTER_OUTPUTS = {
    "default": 0,
    "lowpass": 1,
    "bandpass": 2,
    "highpass": 3,
}
_SYNTH_BODY_TYPES = {
    "default": 0,
    "none": 1,
    "guitar": 2,
    "violin": 3,
    "wood-tube": 4,
}
_SYNTH_MOD_SOURCES = {
    "none": 0,
    "amp-env": 1,
    "filter-env": 2,
    "lfo1": 3,
    "lfo2": 4,
    "velocity": 5,
    "key-track": 6,
    "mod-wheel": 7,
    "random": 8,
}
_SYNTH_MOD_DESTINATIONS = {
    "none": 0,
    "pitch-cents": 1,
    "cutoff-cents": 2,
    "amp-gain": 3,
    "pan-units": 4,
}
SYNTH_ENUM_TABLES = {
    "engine_modes": tuple(_SYNTH_ENGINE_MODES),
    "waveforms": tuple(_SYNTH_OSC_WAVEFORMS),
    "filter_models": tuple(_SYNTH_FILTER_MODELS),
    "filter_outputs": tuple(_SYNTH_FILTER_OUTPUTS),
    "body_types": tuple(_SYNTH_BODY_TYPES),
    "mod_sources": tuple(_SYNTH_MOD_SOURCES),
    "mod_destinations": tuple(_SYNTH_MOD_DESTINATIONS),
}
_SYNTH_ENUM_KINDS = {
    "engine_modes": 0,
    "waveforms": 1,
    "filter_models": 2,
    "filter_outputs": 3,
    "body_types": 4,
    "mod_sources": 5,
    "mod_destinations": 6,
}


def synth_enum_tables() -> dict[str, tuple[str, ...]]:
    """Canonical NativeSynth enum-name tables supplied by the C ABI."""
    lib = _get_lib()
    if not hasattr(lib, "sonare_synth_enum_names"):
        raise RuntimeError("libsonare was built without the NativeSynth enum ABI")
    out: dict[str, tuple[str, ...]] = {}
    for key, kind in _SYNTH_ENUM_KINDS.items():
        raw = lib.sonare_synth_enum_names(kind)
        if not raw:
            out[key] = ()
            continue
        out[key] = tuple(name for name in raw.decode("utf-8").split("\n") if name)
    return out


def _synth_enum_value(value: str | int, names: Mapping[str, int], what: str) -> int:
    if isinstance(value, int):
        return value
    key = value.lower()
    if key not in names:
        raise ValueError(f"unknown {what}: {value!r} (expected one of {sorted(names)})")
    return names[key]


def _synth_enum_name(value: int, names: Mapping[str, int]) -> str | int:
    for name, ordinal in names.items():
        if ordinal == value:
            return name
    return value


def _strip_va_prefix(name: str) -> str:
    return name[3:] if name.startswith("va:") else name
