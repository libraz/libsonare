"""The knob model: what a fit may move, and over what range.

A knob is one value the fit sets. Two kinds, freely mixed in one spec: a
*runtime* knob names a `SONARE_TUNABLE` (or a patch field), pushed in through
`SONARE_TUNING_OVERRIDES` so the library builds once for the whole run; a
*source* knob points a regex at a numeric literal, which puts every evaluation
behind a rebuild.

`auto_spec` derives the whole list from the library's own catalogue instead of a
spec file, and `_auto_range` is where each knob's search range comes from — the
clamp bound the library reports where there is one, a name-and-default heuristic
only where there is not.
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from _repo import REPO_ROOT
from catalogue import Catalogue, TunableDef, drum_patch_key, resolve_knob_name, scan_tunables


@dataclass
class Knob:
    """One value the fit may move.

    Either a runtime knob (`tunable` set: a `SONARE_TUNABLE` constant pushed in
    through the environment) or a source knob (`file`/`pattern` set: a numeric
    literal spliced into the source before a rebuild). Never both.
    """

    label: str
    lo: float
    hi: float
    log: bool
    start_value: float
    tunable: str | None = None
    file: Path | None = None
    pattern: str | None = None
    span_start: int = 0  # group(1) offsets in the pristine file text
    span_end: int = 0


def load_spec(spec_path: Path) -> list[dict]:
    """Read and shape-check a hand-written knob spec."""
    data = json.loads(spec_path.read_text())
    if not isinstance(data, list) or not data:
        raise ValueError(f"spec {spec_path} must be a non-empty JSON array of knobs")
    return data


def _range_of(entry: dict, i: int) -> tuple[float, float, bool]:
    """Validate and unpack one knob entry's min/max/scale."""
    try:
        lo = float(entry["min"])
        hi = float(entry["max"])
    except KeyError as exc:
        raise ValueError(f"knob #{i}: missing required field {exc}") from None
    scale = entry.get("scale", "linear")
    if scale not in ("linear", "log"):
        raise ValueError(f"knob #{i}: scale must be 'linear' or 'log', got {scale!r}")
    if lo >= hi:
        raise ValueError(f"knob #{i}: min ({lo}) must be < max ({hi})")
    if scale == "log" and lo <= 0.0:
        raise ValueError(f"knob #{i}: log scale needs min > 0, got {lo}")
    return lo, hi, scale == "log"


def _clamp_start(value: float, lo: float, hi: float, label: str) -> float:
    clamped = min(max(value, lo), hi)
    if clamped != value:
        print(
            f"note: {label} start value {value} outside [{lo}, {hi}], clamped to {clamped}",
            file=sys.stderr,
        )
    return clamped


def build_knobs(
    spec: list[dict], pristine: dict[Path, str], catalogue: Catalogue | None = None
) -> list[Knob]:
    """Resolve every spec entry to a Knob, validating it against the source.

    Populates `pristine` with the original text of each file a source knob
    targets (and of each file a runtime knob's default lives in, so the final
    report can diff it). `catalogue` is the library's own knob dump, which is
    what validates a per-program patch field — those have no declaration in
    src/ to check against.
    """
    tunables: dict[str, TunableDef] | None = None
    knobs: list[Knob] = []
    for i, entry in enumerate(spec):
        lo, hi, log = _range_of(entry, i)

        if "tunable" in entry:
            if "file" in entry or "pattern" in entry:
                raise ValueError(
                    f"knob #{i}: 'tunable' and 'file'/'pattern' are alternatives, not a pair"
                )
            if tunables is None:
                tunables = scan_tunables()
            name = resolve_knob_name(entry["tunable"], tunables)
            found = tunables.get(name)
            if found is None:
                # Not a SONARE_TUNABLE: a per-program patch field, whose default
                # lives in the program table as a literal with no unique
                # anchor. Legal, but only if the catalogue confirms the key —
                # an unrecognised key is accepted silently by the override
                # table and reads as "this knob does nothing".
                if catalogue is None or name not in catalogue.defaults:
                    known = catalogue.defaults if catalogue else {}
                    near = [k for k in known if k.startswith(name.split(".")[0] + ".")]
                    hint = f" (this patch has {', '.join(sorted(near)[:4])}, ...)" if near else ""
                    raise ValueError(
                        f"knob #{i}: {name!r} is neither a SONARE_TUNABLE in src/ nor a key "
                        f"the library reported{hint}. Run --dump-knobs to list what exists."
                    )
                start = entry.get("start", catalogue.defaults[name])
                knobs.append(Knob(
                    label=name, lo=lo, hi=hi, log=log,
                    start_value=_clamp_start(float(start), lo, hi, name),
                    tunable=name,
                ))
                continue
            if found.file not in pristine:
                pristine[found.file] = found.file.read_text()
            knobs.append(Knob(
                label=name, lo=lo, hi=hi, log=log,
                start_value=_clamp_start(found.value, lo, hi, name),
                tunable=name, file=found.file,
                span_start=found.span_start, span_end=found.span_end,
            ))
            continue

        try:
            rel = entry["file"]
            pattern = entry["pattern"]
        except KeyError as exc:
            raise ValueError(f"knob #{i}: missing required field {exc}") from None

        path = (REPO_ROOT / rel).resolve()
        if not path.exists():
            raise FileNotFoundError(f"knob #{i}: file not found: {rel}")
        if path not in pristine:
            pristine[path] = path.read_text()
        text = pristine[path]

        matches = list(re.compile(pattern).finditer(text))
        if len(matches) != 1:
            raise ValueError(
                f"knob #{i}: pattern {pattern!r} matched {len(matches)} times in {rel} "
                f"(need exactly 1)"
            )
        m = matches[0]
        if m.lastindex is None or m.lastindex < 1:
            raise ValueError(f"knob #{i}: pattern {pattern!r} has no capturing group")
        try:
            start_value = float(m.group(1))
        except (TypeError, ValueError):
            raise ValueError(
                f"knob #{i}: captured text {m.group(1)!r} is not a number"
            ) from None
        label = f"{Path(rel).name}:{pattern}"
        knobs.append(Knob(
            label=label, lo=lo, hi=hi, log=log,
            start_value=_clamp_start(start_value, lo, hi, label),
            file=path, pattern=pattern,
            span_start=m.start(1), span_end=m.end(1),
        ))
    return knobs


# Patch fields that are structural rather than voicing: moving them changes
# what the engine *is* rather than how it sounds, and a fitter given them
# wanders into configurations no instrument occupies.
AUTO_SKIP_SUFFIXES = (
    ".filter_env.delay_ms", ".filter_env.hold_ms", ".amp_env.delay_ms", ".amp_env.hold_ms",
    ".glide_ms", ".pitch_offset_cents", ".key_track", ".lfo2_rate_hz",
)

# Fields whose meaning is a normalized amount; auto ranges are clamped to [0,1]
# rather than scaled off the default, since 2x a 0.9 is not a thing the engine
# accepts and 0 is always a legal end of the range.
AUTO_UNIT_HINTS = (
    "brightness", "damping", "breath", "chiff", "reed", "radiation", "keytrack", "swell",
    "sustain", "mix", "spread", "drive", "rosin", "stribeck", "polarization", "sympathetic",
    "slap", "buzz", "nail", "dispersion", "overblow", "vortex", "growl", "tonehole", "mute",
    "brassiness", "half_valve", "dynamic_lip", "cuivre_dynamics", "noise", "click", "depth",
    "position", "opening", "stiffness", "tension", "gain", "level", "wind_sag", "vent",
)

# Suffixes whose natural scale is multiplicative.
AUTO_LOG_SUFFIXES = ("_ms", "_hz", "_s")

# Widest clamp interval still searched end to end rather than as a window around
# the default. Set above the [0,4] gains so every normalized amount and every
# small physical ratio is searched whole, and below the envelope times.
AUTO_UNIT_SPAN = 4.0

# How far either side of its default a magnitude knob is searched, inside its
# clamp bound. Generous on purpose: the bound stops the search leaving the space,
# so the only cost of a wide window is optimiser budget, while a narrow one puts
# the answer out of reach.
AUTO_MAGNITUDE_SPAN = 8.0


def _auto_range(
    key: str, value: float, bound: tuple[float, float] | None = None
) -> tuple[float, float, bool] | None:
    """Pick a search range for one catalogue knob from its bound, name and default.

    A patch field's range is not a guess: `clamp_synth_patch` bounds every one
    of them, and the library reports those bounds in the knob dump, so `bound`
    is the interval the engine actually accepts. Searching it directly beats any
    heuristic in both directions — a heuristic range is too wide where the clamp
    flattens the loss outside the space, and too narrow whenever the best value
    is several times the default.

    Only the two ends are widened from the raw bound: a knob whose interval
    spans orders of magnitude (`release_ms` at 1..5000) is searched
    logarithmically, and a bound that reaches 0 keeps a linear scale since a log
    one cannot represent it.

    The name-and-default heuristics below are the fallback for the knobs with no
    reported bound — every engine calibration constant (a `SONARE_TUNABLE` is a
    bare float with no clamp anywhere) and the handful of patch fields
    `clamp_synth_patch` leaves open. Deliberately narrow, since a hand-written
    spec is always available when a knob needs a different range.

    Returns None for a knob that should not be fitted automatically.
    """
    leaf = key.rsplit(".", 1)[-1]
    if any(key.endswith(s) for s in AUTO_SKIP_SUFFIXES):
        return None

    if bound is not None:
        lo, hi = bound
        if hi <= lo:
            return None
        if hi - lo <= AUTO_UNIT_SPAN:
            # A normalized or otherwise small interval — [0,1] for the amounts,
            # [0,4] for the gains, [0.02,0.5] for a bow position. Searchable
            # whole, and searching it whole is the point: 0 and 1 are both
            # meaningful settings that a window around the default would hide.
            return lo, hi, False
        if value > 0.0:
            # A magnitude bound (an envelope time at 1..20000 ms, a cutoff at
            # 10..22000 Hz). Too wide to search directly — the default would sit
            # in the first percent of a linear cube — so it is intersected with
            # a generous window around the default and searched
            # logarithmically. The clamp still caps both ends, so the window can
            # be wide without leaving the space.
            lo = max(lo, value / AUTO_MAGNITUDE_SPAN)
            hi = min(hi, value * AUTO_MAGNITUDE_SPAN)
            return (lo, hi, True) if hi > lo else None
        # A wide bound with a zero default: the field is switched off and there
        # is no magnitude to anchor a window on. Picking one would be exactly
        # the guess these bounds replace, so it is left to a hand-written spec.
        return None

    log = any(leaf.endswith(s) for s in AUTO_LOG_SUFFIXES)
    unit = any(h in leaf for h in AUTO_UNIT_HINTS) and not log

    if unit:
        return 0.0, 1.0, False
    if value == 0.0:
        # A disabled feature: let the fit switch it on, over a modest range.
        return (0.0, 1.0, False) if not log else None
    if value < 0.0:
        return 2.0 * value, 0.5 * value, False
    lo, hi = 0.5 * value, 2.0 * value
    return (lo, hi, True) if log else (lo, hi, False)


def auto_spec(
    program: int, catalogue: Catalogue, *, drum_note: int | None = None
) -> list[dict]:
    """Build a knob spec for `program` from the library's own catalogue.

    Two groups are offered: the patch fields of whichever patch voices this
    program (its voicing — what makes a viola a viola rather than a violin),
    and the calibration constants of the engine that patch runs on (its
    physics — shared with every other program on that engine, so moving one
    moves them all).

    `drum_note` selects a drum-note patch instead. A drum note is not a GM
    program — the program selects the kit and the note selects the instrument —
    so its patch is addressed directly by note number rather than looked up in
    the program map, which has no entry for it.
    """
    defaults = catalogue.defaults
    if drum_note is not None:
        key = drum_patch_key(drum_note)
        if not any(k.startswith(key + ".") for k in defaults):
            raise ValueError(
                f"the library reported no patch fields for drum note {drum_note} "
                f"({key!r}); the note may be outside the kit"
            )
    else:
        key = catalogue.programs.get(program)
        if key is None:
            raise ValueError(
                f"the library did not report a patch for program {program}; "
                f"rebuild with BUILD_TUNING=ON so the catalogue is written"
            )
    patch_keys = sorted(k for k in defaults if k.startswith(key + "."))
    if not patch_keys:
        raise ValueError(f"no patch fields under {key!r} in the catalogue")

    # The engine section is the second path element of a patch key that names
    # one (`violin.bowed_string.bow_force` -> bowed_string), which is also the
    # stem prefix of the engine's own source file.
    sections = sorted({k.split(".")[1] for k in patch_keys if k.count(".") >= 2})
    engine = next(
        (s for s in sections if any(c.startswith(f"{s}_voice.") for c in defaults)), None
    )
    engine_keys = sorted(k for k in defaults if engine and k.startswith(f"{engine}_voice."))

    spec: list[dict] = []
    for k in patch_keys + engine_keys:
        rng = _auto_range(k, defaults[k], catalogue.bound_for(k))
        if rng is None:
            continue
        lo, hi, log = rng
        spec.append({
            "tunable": k, "min": round(lo, 6), "max": round(hi, 6),
            "scale": "log" if log else "linear", "start": defaults[k],
        })
    return spec


def format_value(v: float) -> str:
    """Render a float as a C++ literal fragment that always carries a decimal.

    The captured group excludes any `f` suffix, so the substituted text must be
    a valid float literal on its own (e.g. `22` would become an invalid `22f`).
    """
    s = f"{v:.6g}"
    if "e" not in s and "E" not in s and "." not in s:
        s += ".0"
    return s


def tunable_overrides(
    knobs: list[Knob], values: list[float], *, changed_only: bool = False
) -> str:
    """Format the runtime knobs as a `SONARE_TUNING_OVERRIDES` value.

    `changed_only` drops the knobs still at their compiled-in default, which is
    what a reported result wants: a hundred-knob spec produces a string too long
    to read, and every entry equal to the default in it says nothing.
    """
    return ",".join(
        f"{knob.tunable}={format_value(value)}"
        for knob, value in zip(knobs, values)
        if knob.tunable is not None
        and not (changed_only and format_value(value) == format_value(knob.start_value))
    )
