"""The knob model: what a fit may move, and over what range.

A knob is one value the fit sets. Two kinds, freely mixed in one spec: a
*runtime* knob names a `SONARE_TUNABLE` (or a patch field), pushed in through
`SONARE_TUNING_OVERRIDES` so the library builds once for the whole run; a
*source* knob points a regex at a numeric literal, which puts every evaluation
behind a rebuild.

`auto_spec` derives the whole list from the library's own catalogue instead of a
spec file, and `_auto_range` is where each knob's search range comes from — the
clamp bound the library reports where there is one, a name-and-default heuristic
only where there is not. `knob-classes.json` says which of those fields a search
may move at all (identity, switch, set by measurement, stage enable, compensating
group), and `load_knob_classes` is its reader.
"""

from __future__ import annotations

import fnmatch
import json
import math
import re
import sys
from collections import defaultdict
from collections.abc import Collection
from dataclasses import dataclass, field
from pathlib import Path

from _repo import REPO_ROOT
from catalogue import (
    DRUM_PATCH_KEY,
    Catalogue,
    TunableDef,
    drum_patch_key,
    resolve_knob_name,
    scan_tunables,
)
from toneclass import played_vibrato

HERE = Path(__file__).resolve().parent
KNOB_CLASSES_PATH = HERE / "knob-classes.json"
TUNABLE_BOUNDS_PATH = HERE / "tunable-bounds.json"


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
    #: The shipped value, when the search starts elsewhere (`--start-overrides`); None = start.
    head_value: float | None = None
    tunable: str | None = None
    file: Path | None = None
    pattern: str | None = None
    span_start: int = 0  # group(1) offsets in the pristine file text
    span_end: int = 0


def head_of(knob) -> float:
    """The value HEAD ships for a knob: what an unchanged knob is compared against."""
    head = getattr(knob, "head_value", None)
    return knob.start_value if head is None else head


def _spec_document(spec_path: Path) -> dict:
    """Read a spec in either of its two forms and return the object one.

    A spec was originally a bare array of knobs, and most still are. The object
    form adds a `weights` block beside them:

        { "weights": {"harm": 1, "tail": 2, "crest": 2}, "knobs": [ ... ] }

    It sets what the instrument's class defaults got wrong, not the whole
    vector: a term it omits keeps the class's own weight. A spec knows what
    evidence its knobs are supposed to be answering to; the command line should
    not have to carry it.
    """
    data = json.loads(spec_path.read_text())
    if isinstance(data, list):
        return {"knobs": data}
    if isinstance(data, dict) and isinstance(data.get("knobs"), list):
        return data
    raise ValueError(
        f"spec {spec_path} must be a JSON array of knobs, or an object with a 'knobs' array "
        f"(and optionally a 'weights' object)"
    )


def load_spec(spec_path: Path) -> list[dict]:
    """Read and shape-check a hand-written knob spec."""
    knobs = _spec_document(spec_path)["knobs"]
    if not knobs:
        raise ValueError(f"spec {spec_path} lists no knobs")
    return knobs


def load_spec_weights(spec_path: Path) -> dict[str, float]:
    """The term weights a spec carries, or an empty dict when it carries none."""
    weights = _spec_document(spec_path).get("weights", {})
    if not isinstance(weights, dict):
        raise ValueError(  # noqa: TRY004 -- one error class per document
            f"spec {spec_path}: 'weights' must be an object of term -> number"
        )
    out = {}
    for term, value in weights.items():
        try:
            out[term] = float(value)
        except (TypeError, ValueError):
            raise ValueError(
                f"spec {spec_path}: weight for {term!r} is {value!r}, not a number"
            ) from None
    return out


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
                knobs.append(
                    Knob(
                        label=name,
                        lo=lo,
                        hi=hi,
                        log=log,
                        start_value=_clamp_start(float(start), lo, hi, name),
                        tunable=name,
                    )
                )
                continue
            if found.file not in pristine:
                pristine[found.file] = found.file.read_text()
            knobs.append(
                Knob(
                    label=name,
                    lo=lo,
                    hi=hi,
                    log=log,
                    start_value=_clamp_start(found.value, lo, hi, name),
                    tunable=name,
                    file=found.file,
                    span_start=found.span_start,
                    span_end=found.span_end,
                )
            )
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
            raise ValueError(f"knob #{i}: captured text {m.group(1)!r} is not a number") from None
        label = f"{Path(rel).name}:{pattern}"
        knobs.append(
            Knob(
                label=label,
                lo=lo,
                hi=hi,
                log=log,
                start_value=_clamp_start(start_value, lo, hi, label),
                file=path,
                pattern=pattern,
                span_start=m.start(1),
                span_end=m.end(1),
            )
        )
    return knobs


# Patch fields that are structural rather than voicing: moving them changes
# what the engine *is* rather than how it sounds, and a fitter given them
# wanders into configurations no instrument occupies.
AUTO_SKIP_SUFFIXES = (
    ".filter_env.delay_ms",
    ".filter_env.hold_ms",
    ".amp_env.delay_ms",
    ".amp_env.hold_ms",
    ".glide_ms",
    ".pitch_offset_cents",
    ".key_track",
    ".lfo2_rate_hz",
)

# Fields whose meaning is a normalized amount; auto ranges are clamped to [0,1]
# rather than scaled off the default, since 2x a 0.9 is not a thing the engine
# accepts and 0 is always a legal end of the range.
AUTO_UNIT_HINTS = (
    "brightness",
    "damping",
    "breath",
    "chiff",
    "reed",
    "radiation",
    "keytrack",
    "swell",
    "sustain",
    "mix",
    "spread",
    "drive",
    "rosin",
    "stribeck",
    "polarization",
    "sympathetic",
    "slap",
    "buzz",
    "nail",
    "dispersion",
    "overblow",
    "vortex",
    "growl",
    "tonehole",
    "mute",
    "brassiness",
    "half_valve",
    "dynamic_lip",
    "cuivre_dynamics",
    "noise",
    "click",
    "depth",
    "position",
    "opening",
    "stiffness",
    "tension",
    "gain",
    "level",
    "wind_sag",
    "vent",
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

    A patch field's range is mostly not a guess: `clamp_synth_patch` bounds all
    but thirteen of them, and the library reports those bounds in the knob dump,
    so `bound` is the interval the engine actually accepts. Searching it
    directly beats any heuristic in both directions — a heuristic range is too
    wide where the clamp flattens the loss outside the space, and too narrow
    whenever the best value is several times the default.

    Only the two ends are widened from the raw bound: a knob whose interval
    spans orders of magnitude (`release_ms` at 1..5000) is searched
    logarithmically, and a bound that reaches 0 keeps a linear scale since a log
    one cannot represent it.

    The name-and-default heuristics below are the fallback for the knobs with no
    reported bound — every engine calibration constant (a `SONARE_TUNABLE` is a
    bare float with no clamp anywhere) and the thirteen patch fields
    `clamp_synth_patch` leaves open (the eight Karplus-Strong extensions, three
    bowed-string ones and `pipe_organ.keytrack`, all clamped by their own voice
    at start() instead, plus the free angle `percussion.strike_theta`).
    Deliberately narrow, since a hand-written spec is always available when a
    knob needs a different range.

    Returns None for a knob that should not be fitted automatically.
    """
    leaf = key.rsplit(".", 1)[-1]
    if any(key.endswith(s) for s in AUTO_SKIP_SUFFIXES):
        return None

    if leaf.startswith("mode_ratios"):
        # Ahead of the reported bound, which is the clamp's 0..64 and is what
        # lets the window ratchet a round at a time.
        floor, ceiling = MODE_RATIO_RANGE
        if value <= 0.0:
            return None
        lo = max(floor, value / MODE_RATIO_SPAN)
        hi = min(ceiling, value * MODE_RATIO_SPAN)
        return (lo, hi, True) if hi > lo else (floor, ceiling, True)

    if bound is not None:
        lo, hi = bound
        if hi <= lo:
            return None
        if leaf.endswith("_db"):
            # Decibels are already logarithmic and 0 dB is a real setting, so the
            # whole bound is searched linearly whatever the default.
            return lo, hi, False
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


#: Patch fields no objective in this harness can identify, so `--spec auto`
#: does not offer them. `gain` is the voice's output scale, applied after the
#: nonlinearity, so it changes no shape; and every loss term is either
#: normalised by the note's own level or measured around the grid's median
#: offset, precisely so that a fit does not spend its budget on an output gain.
#: A search handed one anyway uses it to absorb whatever level its other
#: choices cost, which is how a hi-hat fit ends 31 dB down with a better score.
#: Measured: restoring `gain` on such a fit moves the peak by 31 dB and leaves
#: every band of the profile bit-identical.
UNIDENTIFIABLE_FIELDS = frozenset({"gain"})

#: Patch fields whose value is fixed by the model's geometry rather than by an
#: instrument's voicing, so `--spec auto` does not offer them either. A
#: percussion mode's `mode_alpha` is the Bessel zero alpha_mn: it scales the
#: spatial argument the strike weighting J_m(alpha * strike_r) is evaluated at,
#: and `percussion_voice.h` records that `mode_ratios[k] == mode_alpha[k] /
#: mode_alpha[0]` for the ideal membrane. A fit allowed to move it independently
#: does not retune a drum, it uses a documented physical relation as a per-mode
#: gain — measured on a kick, which took alpha0 to its bound at eight times the
#: first zero of J0 while dropping alpha1 below alpha0, leaving a mode set no
#: membrane has. The mode *ratios* stay fittable: a real head genuinely detunes
#: away from the ideal set, and that is what a ratio is for.
#:
#: `base_freq_hz` and `shell_freq_hz` are the same case one level up: they are
#: what the instrument IS rather than how it is voiced. The drum table sets them
#: where each patch is built, from the spec or from a measurement — "GM pins one
#: head frequency per key" for the fixed-pitch membranes, the TR-808's 587 and
#: 845 Hz for the cowbell, the sampled kit's measured 315 Hz peak for the closed
#: hat — and the C++ voice tests assert several of them by frequency. Offered,
#: they become a free spectral-shaping parameter: a whole-kit fit moved 34 of
#: them, taking the low timbale's head from 200 Hz to 6682, the cowbell's from
#: 587 to 4487 and the open hi-hat's from 315 to 16, which is not a retuned drum
#: but a different object that happens to match the reference's band profile.
#:
#: `mode_ratios0` is `base_freq_hz` under another name and has to be pinned with
#: it: the first mode sounds at `base_hz * mode_ratios[0]`, so a fit denied the
#: base frequency moves this instead. 36 kit notes carry one fitted away from 1,
#: from 0.0251 to 41.0 — five octaves either side of the pitch the drum table
#: states. The ratios above it stay fittable, over the window below.
#:
#: `head_diameter_m` and `shell_depth_m` are the drum's dimensions in metres — a
#: 12-inch rack tom is 0.30, a 14x5.5 snare is 0.36 by 0.14 — so they are a data
#: edit off the instrument, not a search. Offered, the head diameter is a tilt
#: control: it weights every m >= 1 mode by (ka)^m, which is monotone in
#: frequency and capped at 1, so a fit reaches for it wherever the model is too
#: bright and lands on a drum of whatever size that needed. `air_spring` is
#: deliberately NOT here: it has a geometric estimate to check a value against
#: but the estimate runs 30-40% high, so it is a fitted scalar by design.
STRUCTURAL_FIELD_PREFIXES = (
    "mode_alpha",
    "base_freq_hz",
    "shell_freq_hz",
    "mode_ratios0",
    "head_diameter_m",
    "shell_depth_m",
)

#: How far either side of its compiled default a mode ratio above the
#: fundamental is searched, and the interval no number of rounds leaves.
#:
#: The clamp accepts 0..64 and a ratio is offered as a magnitude, so a fit that
#: writes back re-anchors the window on what it just wrote: each round multiplies
#: the reach, and the table now holds ratios of 44.9 and 41.4. Those are not
#: detunings. A ratio scales the mode's decay as well as its frequency
#: (`mode.r` is taken at `mode_decay_s / ratio`), so a ratio far above the set is
#: how a search switches a mode off while every amplitude knob stays where the
#: voicing put it. The absolute window is what a struck head or plate can put
#: above its fundamental — twelve membrane modes end at 4.06, a bell's octave
#: partial at 4 — and a ratio outside it is re-searched over the whole of it
#: rather than held, since holding it would keep the escape and change nothing.
MODE_RATIO_SPAN = 2.0
MODE_RATIO_RANGE = (0.2, 8.0)


#: Engine fields a search is never given on any patch: mechanisms the bank does
#: not ship, which a C++ test holds at 0 on every bowed voice. A fit offered them
#: turns one on wherever it lowers a term, and the bank then ships it unjudged.
UNSHIPPED_FIELDS = frozenset({"bowed_string.attack_noise", "bowed_string.bow_accel_ms"})

#: Patch fields fixed by what one instrument is, each held by a C++ test, so a
#: fit is not given them. Keys are fnmatch patterns; the value is the invariant.
PINNED_KEYS = {
    "bag_pipe.reed.vel_to_breath": "the bag, not the player, sets the pressure",
    "electric_grand.amp_env.attack_ms": "programs 0-3 keep the grand's onset peak",
    "marimba.modal.num_modes": "a marimba bar is tuned 1:4:10 over three modes",
    "marimba.modal.modes*.ratio": "a marimba bar is tuned 1:4:10 over three modes",
    "d056.percussion.mode_ratios*": "the cowbell's two partials are 587 and 845 Hz",
}


#: Where a played vibrato's rate lies. The engine accepts 0-40 Hz for its synth
#: patches; on a played instrument a fit past this band lands on a rate no
#: player produces, and against a still reference nothing pulls it back.
VIBRATO_RATE_RANGE_HZ = (3.0, 8.0)


def _offered(key: str) -> bool:
    """Whether `--spec auto` puts this patch field (`patch.section.field`) before a search."""
    field = key.rsplit(".", 1)[-1]
    if field in UNIDENTIFIABLE_FIELDS or field.startswith(STRUCTURAL_FIELD_PREFIXES):
        return False
    if key.partition(".")[2] in UNSHIPPED_FIELDS:
        return False
    return not any(fnmatch.fnmatchcase(key, pattern) for pattern in PINNED_KEYS)


#: The classes a `knob-classes.json` entry may carry. An entry without one only
#: records a note about the field (a gate), and the field is searched as usual.
KNOB_CLASS_NAMES = (
    "identity",
    "switch",
    "set_by_measurement",
    "stage_enable",
    "compensating_group",
)

#: Classes `--spec auto` never offers. A hand spec may still name one, with a warning.
NOT_SEARCHED = ("identity", "switch", "set_by_measurement")

#: How far above its off value a clipped stage-enable range starts, as a fraction
#: of the range it had: the smallest step that is still a stage that is on.
STAGE_ENABLE_STEP = 1e-3

# The catalogue spells an array element `modes0`, `ops1`, `ranks3`; the table spells it `[*]`.
_INDEX = re.compile(r"(?<=[A-Za-z_])\d+(?=\.|$)")
_PATTERN_TOKENS = re.compile(r"(\[\*\]|\*)")


def _normalize_tail(tail: str) -> str:
    return _INDEX.sub("[*]", tail)


def _spellings(key: str) -> list[str]:
    """The forms a catalogue key can be named by: whole, without its patch, and indexed."""
    tail = key.partition(".")[2]
    return [key, tail, _normalize_tail(tail)] if tail else [key]


class _Patterns:
    """A set of key patterns: a literal, `[*]` for an array index, `*` for a glob.

    A pattern names a catalogue key whole (an engine constant, `piano_voice.kX`) or
    without the patch prefix a patch field carries (`piano.strings`).
    """

    def __init__(self, patterns):
        self.exact: set[str] = set()
        self.globs: list[tuple[re.Pattern, str]] = []
        for p in patterns:
            if "*" in p.replace("[*]", ""):
                regex = "".join(
                    ".*" if t == "*" else re.escape(t) for t in _PATTERN_TOKENS.split(p) if t
                )
                self.globs.append((re.compile(regex), p))
            else:
                self.exact.add(p)

    def find(self, key: str) -> str | None:
        """The pattern naming @p key, or None."""
        spellings = _spellings(key)
        for s in spellings:
            if s in self.exact:
                return s
        for regex, p in self.globs:
            if any(regex.fullmatch(s) for s in spellings):
                return p
        return None

    def unresolved(self, keys) -> list[str]:
        """The patterns no key in @p keys is named by, sorted."""
        seen = {s for k in keys for s in _spellings(k)}
        missing = {p for p in self.exact if p not in seen}
        missing |= {p for regex, p in self.globs if not any(regex.fullmatch(s) for s in seen)}
        return sorted(missing)


@dataclass(frozen=True)
class KnobClass:
    """One `knob-classes.json` entry.

    `off_value` is what a stage-enable field holds while its stage is absent, and
    `off_within` widens that to a band (a field absent at or below -119 dB is
    `off_value=-120`, `off_within=1`). `gates` records a field another field
    switches off. `evidence` says what a compensating-group member's membership rests on.
    """

    key: str
    cls: str | None
    lane: str = ""
    basis: str = ""
    group: str | None = None
    evidence: str = ""
    off_value: float = 0.0
    off_within: float = 0.0
    gates: str = ""
    type: str = ""

    def is_off(self, value: float) -> bool:
        """Whether a shipped @p value leaves this stage-enable field's stage absent."""
        return abs(value - self.off_value) <= self.off_within


@dataclass(frozen=True)
class KnobReach:
    """One `reach` entry: the gated dimensions a field needs before `--spec auto` opens it.

    A knob whose movement no gated dimension can judge is searched with nothing to
    hold it, so it is offered only where every dimension in `requires` is gated.
    """

    pattern: str
    requires: tuple[str, ...]
    basis: str = ""


@dataclass
class KnobClasses:
    """The classification of the catalogue's fields, as `knob-classes.json` states it.

    `entries` are keyed by pattern; `notes` explain a field the table does not
    classify (a field no override reaches); `pending_landing` holds entries for keys
    the library does not report yet; `switch_candidates` lists float-typed fields
    whose shipped values are all integers, for the lanes to judge. `reach` maps
    a field pattern to the gated dimensions it needs (see `KnobReach`).
    """

    entries: dict[str, KnobClass]
    notes: dict[str, str] = field(default_factory=dict)
    pending_landing: dict[str, dict] = field(default_factory=dict)
    switch_candidates: dict[str, dict] = field(default_factory=dict)
    reach: dict[str, KnobReach] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self._entry_patterns = _Patterns(self.entries)
        self._reach_patterns = _Patterns(self.reach)
        self._note_patterns = _Patterns(self.notes)
        self._memo: dict[str, KnobClass | None] = {}
        self.groups: dict[str, list[str]] = defaultdict(list)
        for key, entry in self.entries.items():
            if entry.group:
                self.groups[entry.group].append(key)

    def classify(self, key: str) -> KnobClass | None:
        """The entry naming a catalogue @p key, or None for a field the table leaves alone."""
        if key not in self._memo:
            pattern = self._entry_patterns.find(key)
            self._memo[key] = self.entries[pattern] if pattern is not None else None
        return self._memo[key]

    def class_of(self, key: str) -> str | None:
        """The class of a catalogue @p key, or None when it has none."""
        entry = self.classify(key)
        return entry.cls if entry is not None else None

    def note_for(self, key: str) -> str | None:
        """The note recorded for a catalogue @p key, if any."""
        pattern = self._note_patterns.find(key)
        return self.notes[pattern] if pattern is not None else None

    def reach_for(self, key: str) -> KnobReach | None:
        """The reach entry naming a catalogue @p key, or None for a field any gate may hold."""
        pattern = self._reach_patterns.find(key)
        return self.reach[pattern] if pattern is not None else None

    def unjudged_by(self, key: str, gated) -> list[str]:
        """The dimensions @p key needs that @p gated lacks, in the entry's order."""
        entry = self.reach_for(key)
        return [d for d in entry.requires if d not in gated] if entry is not None else []

    def group_members(self, group: str, keys) -> list[str]:
        """The keys among @p keys that belong to @p group."""
        return [k for k in keys if (e := self.classify(k)) is not None and e.group == group]


def load_knob_classes(path: Path, catalogue: Catalogue) -> KnobClasses:
    """Read `knob-classes.json` and check every entry against the library's catalogue.

    An entry that names no key the catalogue reports raises, naming it: a field
    that was renamed or removed must not quietly lose its class and become
    searchable. A `reach` pattern is held to the same. `pending_landing` and
    `notes` are exempt.
    """
    data = json.loads(Path(path).read_text())
    raw = data.get("entries")
    if not isinstance(raw, dict):
        raise ValueError(f"{path}: no 'entries' object")  # noqa: TRY004 -- one error class per document
    entries: dict[str, KnobClass] = {}
    for key, e in raw.items():
        cls = e.get("class")
        if cls is not None and cls not in KNOB_CLASS_NAMES:
            raise ValueError(
                f"{path}: entry {key!r} has class {cls!r}, not one of {KNOB_CLASS_NAMES}"
            )
        if (cls == "compensating_group") != bool(e.get("group")):
            raise ValueError(
                f"{path}: entry {key!r} must carry 'group' exactly when its class is compensating_group"
            )
        entries[key] = KnobClass(
            key=key,
            cls=cls,
            lane=e.get("lane", ""),
            basis=e.get("basis", ""),
            group=e.get("group"),
            evidence=e.get("evidence", ""),
            off_value=float(e.get("off_value", 0.0)),
            off_within=float(e.get("off_within", 0.0)),
            gates=e.get("gates", ""),
            type=e.get("type", ""),
        )
    reach: dict[str, KnobReach] = {}
    for pattern, e in data.get("reach", {}).items():
        if pattern.startswith("_"):
            continue
        requires = e.get("requires")
        if (
            not isinstance(requires, list)
            or not requires
            or not all(isinstance(d, str) and d for d in requires)
        ):
            raise ValueError(
                f"{path}: reach {pattern!r} needs a non-empty 'requires' list of dimension names"
            )
        reach[pattern] = KnobReach(pattern, tuple(requires), e.get("basis", ""))
    classes = KnobClasses(
        entries,
        notes=data.get("notes", {}),
        pending_landing=data.get("pending_landing", {}),
        switch_candidates=data.get("switch_candidates", {}),
        reach=reach,
    )
    unreached = classes._reach_patterns.unresolved(catalogue.defaults)
    if unreached:
        raise ValueError(
            f"{Path(path).name}: {len(unreached)} reach pattern(s) name no key the catalogue "
            f"reports: {', '.join(repr(p) for p in unreached)}. The field was renamed or "
            f"removed; rename or remove the pattern, or it stops holding its knobs out."
        )
    missing = classes._entry_patterns.unresolved(catalogue.defaults)
    if missing:
        named = "; ".join(f"{k!r} (lane {entries[k].lane or '?'})" for k in missing)
        raise ValueError(
            f"{Path(path).name}: {len(missing)} entr{'y' if len(missing) == 1 else 'ies'} "
            f"name no key the catalogue reports: {named}. The field was renamed or removed; "
            f"rename or remove the entry, or it loses its class."
        )
    return classes


def _patch_heads(catalogue: Catalogue) -> set[str]:
    return set(catalogue.programs.values()) | set(catalogue.modes)


def _is_patch_head(head: str, patches: set[str]) -> bool:
    return (
        head in patches or bool(DRUM_PATCH_KEY.match(head)) or bool(re.fullmatch(r"fam\d+", head))
    )


def switch_candidates(catalogue: Catalogue, classes: KnobClasses) -> dict[str, dict]:
    """Patch fields typed as floats whose shipped values, bank-wide, are all integers.

    Such a field may be a selector in float clothing, which a search would move
    as a continuum. It is a list for the lanes to judge, not a classification: a
    field the table already holds as identity, switch or set by measurement is
    left out, and so is one that takes a single value everywhere.
    """
    patches = _patch_heads(catalogue)
    values: dict[str, set[float]] = defaultdict(set)
    sample: dict[str, str] = {}
    integral: dict[str, bool] = {}
    for key, v in catalogue.defaults.items():
        head, _, tail = key.partition(".")
        if not tail or not _is_patch_head(head, patches):
            continue
        norm = _normalize_tail(tail)
        sample.setdefault(norm, key)
        integral[norm] = integral.get(norm, True) and math.isfinite(v) and v == int(v)
        values[norm].add(v)
    out: dict[str, dict] = {}
    for norm in sorted(values):
        if not integral[norm] or len(values[norm]) < 2:
            continue
        entry = classes.classify(sample[norm])
        if entry is not None and (entry.cls in NOT_SEARCHED or entry.type):
            continue
        out[norm] = {
            "class": entry.cls if entry is not None else None,
            "values": sorted(values[norm]),
        }
    return out


@dataclass
class AutoReport:
    """What `auto_spec_report` left out of the knob list, and why.

    `excluded` maps a class to the keys it kept out; `stage_off` holds the stage
    enables whose patch ships the stage absent; `clipped` the stage enables whose
    range was cut so it cannot reach its off value; `groups_closed` the
    compensating groups no knob was offered for, by the number of members present;
    `unjudged` the knobs left out because a dimension they need is not gated, by that
    dimension, and `no_gate` that no gate was found, so none was taken as gated.
    """

    excluded: dict[str, list[str]] = field(default_factory=dict)
    stage_off: list[str] = field(default_factory=list)
    clipped: list[str] = field(default_factory=list)
    groups_closed: dict[str, int] = field(default_factory=dict)
    groups_open: list[str] = field(default_factory=list)
    unjudged: dict[str, list[str]] = field(default_factory=dict)
    no_gate: bool = False

    def reach_lines(self) -> list[str]:
        """The knobs held out for want of a gated dimension, and the gate that was read."""
        out = []
        if self.unjudged:
            counts = ", ".join(f"{d} {len(ks)}" for d, ks in sorted(self.unjudged.items()))
            out.append(
                f"--spec auto leaves out {sum(len(k) for k in self.unjudged.values())} "
                f"knob(s), no gated dimension can judge them (missing dimension): {counts}"
            )
        if self.no_gate:
            out.append("no gate found for this voice: no dimension is taken as gated")
        return out

    def lines(self) -> list[str]:
        """The start-up summary: counts by class, then one line per closed group."""
        counts = [f"{c} {len(self.excluded[c])}" for c in NOT_SEARCHED if self.excluded.get(c)]
        if self.stage_off:
            counts.append(f"stage_enable (stage absent here) {len(self.stage_off)}")
        if self.groups_closed:
            counts.append(
                f"compensating_group {sum(self.groups_closed.values())} "
                f"({len(self.groups_closed)} groups)"
            )
        out = []
        if counts:
            out.append("--spec auto leaves out by class: " + ", ".join(counts))
        if self.clipped:
            out.append(
                f"--spec auto cut the range of {len(self.clipped)} stage_enable knob(s) "
                f"so they cannot reach their off value"
            )
        out.extend(
            f"group {g} not opened (open it with --open-group {g})" for g in self.groups_closed
        )
        out.extend(self.reach_lines())
        return out


def _clip_off(
    lo: float, hi: float, log: bool, value: float, entry: KnobClass
) -> tuple[float, float, bool] | None:
    """Cut a stage-enable range so it stays on the side of the off value the patch ships.

    Returns None when nothing is left.
    """
    step = (hi - lo) * STAGE_ENABLE_STEP
    near = entry.off_value + entry.off_within
    far = entry.off_value - entry.off_within
    if value > entry.off_value:
        if lo <= near:
            lo = near + step
    elif hi >= far:
        hi = far - step
    if hi <= lo:
        return None
    return lo, hi, log and lo > 0.0


def auto_spec(
    program: int,
    catalogue: Catalogue,
    *,
    drum_note: int | None = None,
    bank: int = 0,
    patch_only: bool = False,
    classes: KnobClasses | None = None,
    open_groups: tuple[str, ...] = (),
    gated: Collection[str] | None = None,
) -> list[dict]:
    """The knob list of `auto_spec_report`, without the account of what it left out."""
    return auto_spec_report(
        program,
        catalogue,
        drum_note=drum_note,
        bank=bank,
        patch_only=patch_only,
        classes=classes,
        open_groups=open_groups,
        gated=gated,
    )[0]


def auto_spec_report(
    program: int,
    catalogue: Catalogue,
    *,
    drum_note: int | None = None,
    bank: int = 0,
    patch_only: bool = False,
    classes: KnobClasses | None = None,
    open_groups: tuple[str, ...] = (),
    gated: Collection[str] | None = None,
) -> tuple[list[dict], AutoReport]:
    """Build a knob spec for `program` from the library's own catalogue.

    Two groups are offered: the patch fields of whichever patch voices this
    program (its voicing — what makes a viola a viola rather than a violin),
    and the calibration constants of the engine that patch runs on (its
    physics — shared with every other program on that engine, so moving one
    moves them all).

    `patch_only` drops the second group. A fit over one voice cannot attribute
    a shared constant to it — that is why the bank registry gives the engines
    their own units — and a run over a grid of voices in turn moves the ground
    under the ones already done. The values it writes are real either way; what
    is missing is any voice that would have objected.

    `drum_note` selects a drum-note patch instead. A drum note is not a GM
    program — the program selects the kit and the note selects the instrument —
    so its patch is addressed directly by note number rather than looked up in
    the program map, which has no entry for it.

    `bank` is the GS variation. It has to be here rather than only on the render
    side: a variation is its own patch, so offering the capital tone's knobs
    against a render of bank 8 fits a patch nothing played.

    `classes` is the field classification (`knob-classes.json`). With it,
    identity, switch and set-by-measurement fields are not offered; a stage-enable
    field is decided from the value this patch ships (left out where the stage is
    absent, its range cut clear of the off value where it is present); and a
    compensating group is offered whole when named in `open_groups`, and not at
    all otherwise. Without it every field the heuristics can range is offered.

    `gated` is the dimensions the capture's gate holds a bound for. A field whose
    `reach` entry needs a dimension that is not among them is left out, since
    nothing would hold what the fit moves; `None` means no gate was found, which
    is read as no dimension gated and said so in the report.
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
        key = catalogue.programs.get((program, bank))
        if key is None and bank:
            raise ValueError(
                f"the library voices no variation of program {program} at bank {bank}; "
                f"it has {catalogue.banks_for(program) or [0]}"
            )
        if key is None:
            raise ValueError(
                f"the library did not report a patch for program {program}; "
                f"rebuild with BUILD_TUNING=ON so the catalogue is written"
            )
    patch_keys = sorted(k for k in defaults if k.startswith(key + ".") and _offered(k))
    if not patch_keys:
        raise ValueError(f"no patch fields under {key!r} in the catalogue")

    # The engine section is the second path element of a patch key that names
    # one (`violin.bowed_string.bow_force` -> bowed_string), which is also the
    # stem prefix of the engine's own source file.
    sections = sorted({k.split(".")[1] for k in patch_keys if k.count(".") >= 2})
    engine = next((s for s in sections if any(c.startswith(f"{s}_voice.") for c in defaults)), None)
    engine_keys = (
        []
        if patch_only
        else sorted(k for k in defaults if engine and k.startswith(f"{engine}_voice."))
    )

    report = AutoReport(no_gate=gated is None)
    gated = frozenset(gated or ())
    candidates = patch_keys + engine_keys
    if open_groups and classes is None:
        raise ValueError("--open-group needs the field classes (knob-classes.json)")
    if classes is not None:
        known = sorted(classes.groups)
        for group in open_groups:
            if group not in classes.groups:
                raise ValueError(
                    f"--open-group {group!r}: no such compensating group (known: {known})"
                )
            gone = [m for m in classes.groups[group] if _Patterns([m]).unresolved(candidates)]
            if len(gone) == len(classes.groups[group]):
                raise ValueError(
                    f"--open-group {group!r}: none of its members is among this "
                    f"{'patch' if patch_only else 'patch or engine'}'s knobs"
                )
            if gone:
                raise ValueError(
                    f"--open-group {group!r}: a group is opened whole or not at all, and "
                    f"{', '.join(gone)} is not among this "
                    f"{'patch' if patch_only else 'patch or engine'}'s knobs"
                    f"{' (drop --program-only)' if patch_only else ''}"
                )

    vibrato = played_vibrato(program, drum_note=drum_note)
    spec: list[dict] = []
    offered_group: dict[str, set[str]] = defaultdict(set)
    seen_group: dict[str, set[str]] = defaultdict(set)
    for k in candidates:
        rng = _auto_range(k, defaults[k], catalogue.bound_for(k))
        entry = classes.classify(k) if classes is not None else None
        if entry is not None and entry.group:
            seen_group[entry.group].add(k)
        if rng is None:
            continue
        if entry is not None and entry.cls in NOT_SEARCHED:
            report.excluded.setdefault(entry.cls, []).append(k)
            continue
        if entry is not None and entry.cls == "stage_enable":
            if entry.is_off(defaults[k]):
                report.stage_off.append(k)
                continue
            clipped = _clip_off(*rng[:2], rng[2], defaults[k], entry)
            if clipped is None:
                report.stage_off.append(k)
                continue
            if clipped != rng:
                report.clipped.append(k)
            rng = clipped
        if (
            entry is not None
            and entry.cls == "compensating_group"
            and entry.group not in open_groups
        ):
            continue
        if classes is not None:
            missing = classes.unjudged_by(k, gated)
            if missing:
                for dim in missing:
                    report.unjudged.setdefault(dim, []).append(k)
                continue
        if entry is not None and entry.cls == "compensating_group":
            offered_group[entry.group].add(k)
        if vibrato and k.endswith(".lfo_rate_hz"):
            rng = (*VIBRATO_RATE_RANGE_HZ, True)
        lo, hi, log = rng
        spec.append(
            {
                "tunable": k,
                "min": round(lo, 6),
                "max": round(hi, 6),
                "scale": "log" if log else "linear",
                "start": min(max(defaults[k], lo), hi),
            }
        )
    for group, keys in seen_group.items():
        if group in open_groups:
            left = sorted(keys - offered_group[group])
            if left:
                raise ValueError(
                    f"--open-group {group!r}: a group is opened whole or not at all, and "
                    f"{', '.join(left)} has no search range here"
                )
            report.groups_open.append(group)
        else:
            report.groups_closed[group] = len(keys)
    return spec, report


def hand_spec_warnings(
    knobs: list[Knob],
    classes: KnobClasses | None,
    gated: Collection[str] | None = None,
) -> list[str]:
    """One line per runtime knob a hand spec names that `--spec auto` would never offer.

    The spec is explicit, so the knob stays; the line says the table holds it
    out of a search and why. A knob needing a dimension that is not in @p gated
    (None: no gate found) is named too, with the dimension.
    """
    if classes is None:
        return []
    out = []
    for k in knobs:
        if k.tunable is None:
            continue
        missing = classes.unjudged_by(k.tunable, gated or ())
        if missing:
            reach = classes.reach_for(k.tunable)
            out.append(
                f"{k.label}: no gated dimension can judge it (needs {', '.join(missing)}"
                f"{'; no gate found' if gated is None else ''}); "
                f"searched here because the spec names it"
                + (f" ({reach.basis})" if reach.basis else "")
            )
        entry = classes.classify(k.tunable)
        if entry is None or entry.cls not in NOT_SEARCHED:
            continue
        what = (
            "set by measurement, not searched"
            if entry.cls == "set_by_measurement"
            else f"a{'n' if entry.cls == 'identity' else ''} {entry.cls} knob, not searched"
        )
        out.append(
            f"{k.label}: {what} by --spec auto; searched here because the spec names it"
            + (f" ({entry.basis})" if entry.basis else "")
        )
    return out


#: How a range other than the engine's own clamp is named in a clamp check.
DECLARED_FILE = "declared range (file)"
DECLARED_MACRO = "declared range (macro)"


def load_declared_bounds(path: Path) -> dict[str, tuple[float, float]]:
    """The engine-constant ranges `tunable-bounds.json` declares, by `<file stem>.<kName>`.

    Only its `bounds` block: `patch_fields_pending_clamp` proposes ranges for patch
    fields that `patch_clamp.h` does not carry yet, and are not search bounds.
    """
    data = json.loads(Path(path).read_text())
    out: dict[str, tuple[float, float]] = {}
    for key, e in data.get("bounds", {}).items():
        lo, hi = float(e["lo"]), float(e["hi"])
        if lo >= hi:
            raise ValueError(f"{Path(path).name}: {key} has lo {lo} >= hi {hi}")
        out[key] = (lo, hi)
    return out


def declared_ranges(
    knobs: list[Knob],
    catalogue: Catalogue | None,
    declared: dict[str, tuple[float, float]],
) -> tuple[dict[str, tuple[float, float, str]], list[str]]:
    """The declared range of each knob that addresses an engine constant, by label.

    Two places declare one: `tunable-bounds.json` and a macro in the source, which
    the library reports as a `#tunable_bound` line. Each is labelled with where it
    came from. A key in both with different intervals is a mismatch, reported in
    the second return value, and the macro is the one used because it is the code.
    """
    out: dict[str, tuple[float, float, str]] = {}
    mismatches: list[str] = []
    patches = _patch_heads(catalogue) if catalogue is not None else set()
    for k in knobs:
        if k.tunable is None:
            continue
        head = k.tunable.partition(".")[0]
        if head in patches or DRUM_PATCH_KEY.match(head):
            continue
        from_file = declared.get(k.tunable)
        from_macro = catalogue.tunable_bounds.get(k.tunable) if catalogue is not None else None
        if from_file and from_macro and from_file != from_macro:
            mismatches.append(
                f"{k.label}: {DECLARED_FILE} [{from_file[0]:g}, {from_file[1]:g}] differs from "
                f"{DECLARED_MACRO} [{from_macro[0]:g}, {from_macro[1]:g}]; the macro is checked"
            )
        if from_macro:
            out[k.label] = (*from_macro, DECLARED_MACRO)
        elif from_file:
            out[k.label] = (*from_file, DECLARED_FILE)
    return out, mismatches


def knob_bounds(knobs: list[Knob], catalogue: Catalogue | None) -> dict[str, tuple[float, float]]:
    """The clamp interval of each knob that addresses a bounded patch field, by label.

    Only a key whose first segment names a patch (or a drum note's `dNNN`) is a
    patch field; an engine constant shares the dotted form but carries no bound,
    so it is left out rather than matched against a same-named field path.
    """
    if catalogue is None:
        return {}
    patches = set(catalogue.programs.values()) | set(catalogue.modes)
    out: dict[str, tuple[float, float]] = {}
    for k in knobs:
        if k.tunable is None:
            continue
        head = k.tunable.partition(".")[0]
        if head not in patches and not DRUM_PATCH_KEY.match(head):
            continue
        bound = catalogue.bound_for(k.tunable)
        if bound is not None:
            out[k.label] = bound
    return out


def clamp_check(
    knobs: list[Knob],
    bounds: dict[str, tuple[float, float]],
    declared: dict[str, tuple[float, float, str]] | None = None,
) -> tuple[list[dict], list[str]]:
    """Compare each knob's search range with its field's clamp before any render.

    Returns (entries, refusals). A range wholly outside the clamp (or touching it
    at one point) is inert everywhere and goes to `refusals`. A range partly
    outside, or a start outside the clamp, gets an entry naming the inert part. A
    runtime knob with no bound gets an `unbounded` entry: no clamp was reported,
    which is not the same as the field being safe. A knob inside its clamp has none.

    `declared` holds the ranges an engine constant is declared to have
    (`declared_ranges`). Nothing clamps such a constant, so a range outside it is
    not inert; it is a range the declaration rules out, and the entry says which
    declaration. The clamp wins where a label has both.
    """
    declared = declared or {}
    entries: list[dict] = []
    refusals: list[str] = []
    for k in knobs:
        if k.tunable is None:
            continue
        bound = bounds.get(k.label)
        source = ""
        if bound is None and k.label in declared:
            dlo, dhi, source = declared[k.label]
            bound = (dlo, dhi)
        if bound is None:
            entries.append({"key": k.label, "status": "unbounded", "range": [k.lo, k.hi]})
            continue
        clo, chi = bound
        if k.hi <= clo or k.lo >= chi:
            what = (
                f"the {source} [{clo:g}, {chi:g}]"
                if source
                else f"the clamp [{clo:g}, {chi:g}]; every value renders the same, so the knob is inert"
            )
            refusals.append(f"{k.label}: range [{k.lo:g}, {k.hi:g}] is wholly outside {what}")
            continue
        inert = []
        if k.lo < clo:
            inert.append([k.lo, clo])
        if k.hi > chi:
            inert.append([chi, k.hi])
        start_out = not clo <= k.start_value <= chi
        if inert or start_out:
            entry = {
                "key": k.label,
                "status": "partly_outside",
                "range": [k.lo, k.hi],
                "clamp": [clo, chi],
                "inert": inert,
                "start": k.start_value,
                "start_outside_clamp": start_out,
            }
            if source:
                entry["source"] = source
            entries.append(entry)
    return entries, refusals


def clamp_warning(entry: dict) -> str:
    """One stderr line for a `clamp_check` entry."""
    key = entry["key"]
    if entry["status"] == "unbounded":
        return f"{key}: no clamp bound reported (unbounded)"
    source = entry.get("source")
    outside = ", ".join(f"[{a:g}, {b:g}]" for a, b in entry["inert"])
    parts = []
    if outside:
        parts.append(
            f"{outside} of its range [{entry['range'][0]:g}, {entry['range'][1]:g}] "
            + ("is outside it" if source else "is inert")
        )
    if entry["start_outside_clamp"]:
        parts.append(f"start {entry['start']:g} is outside " + ("it" if source else "the clamp"))
    return (
        f"{key}: {source or 'clamp'} [{entry['clamp'][0]:g}, {entry['clamp'][1]:g}]; "
        + "; ".join(parts)
    )


def stored_vs_effective(
    knobs: list[Knob], values: list[float], bounds: dict[str, tuple[float, float]]
) -> list[dict]:
    """Knobs whose final value differs from what the engine will use (the value clamped)."""
    out = []
    for k, v in zip(knobs, values):
        bound = bounds.get(k.label)
        if bound is None:
            continue
        eff = min(max(v, bound[0]), bound[1])
        if eff != v:
            out.append({"key": k.label, "stored": v, "effective": eff, "clamp": list(bound)})
    return out


def at_bound(knob: Knob, value: float) -> str | None:
    """ "minimum" or "maximum" when a value sits on an end of its range, else None.

    One rule, used both by the warning a run ends with and by the per-knob
    annotation in its report, so the two cannot disagree about what counts.

    The tolerance is a fraction of the span rather than an absolute epsilon: a
    knob searched over [900, 2600] that comes back at 2599.9 is pinned in every
    sense that matters, and testing exact equality would say it is not.
    """
    span = knob.hi - knob.lo
    if span <= 0.0:
        return None
    tol = span * 1e-3
    if value <= knob.lo + tol:
        return "minimum"
    if value >= knob.hi - tol:
        return "maximum"
    return None


def format_value(v: float) -> str:
    """Render a float as a C++ literal fragment that always carries a decimal.

    The captured group excludes any `f` suffix, so the substituted text must be
    a valid float literal on its own (e.g. `22` would become an invalid `22f`).
    """
    s = f"{v:.6g}"
    if "e" not in s and "E" not in s and "." not in s:
        s += ".0"
    return s


def tunable_overrides(knobs: list[Knob], values: list[float], *, changed_only: bool = False) -> str:
    """Format the runtime knobs as a `SONARE_TUNING_OVERRIDES` value.

    `changed_only` drops the knobs still at HEAD's value, which is
    what a reported result wants: a hundred-knob spec produces a string too long
    to read, and every entry equal to the default in it says nothing.
    """
    return ",".join(
        f"{knob.tunable}={format_value(value)}"
        for knob, value in zip(knobs, values)
        if knob.tunable is not None
        and not (changed_only and format_value(value) == format_value(head_of(knob)))
    )


def parse_overrides(text: str) -> list[tuple[str, str]]:
    """Split a `SONARE_TUNING_OVERRIDES` string into (key, value) pairs, in order."""
    pairs = []
    for entry in (e.strip() for e in text.split(",")):
        if not entry:
            continue
        key, eq, value = entry.partition("=")
        if not eq or not key.strip():
            raise ValueError(f"override {entry!r} is not key=value")
        pairs.append((key.strip(), value.strip()))
    return pairs


def merge_overrides(base: str, candidate: str) -> str:
    """The base overrides followed by the candidate's; a key the candidate sets wins.

    Every evaluation's override string comes through here, so a fit run on top of
    fixed engine constants cannot reach the library with a different merge.
    """
    if not base:
        return candidate
    taken = {k for k, _ in parse_overrides(candidate)}
    kept = [f"{k}={v}" for k, v in parse_overrides(base) if k not in taken]
    return ",".join([*kept, *([candidate] if candidate else [])])


def _load_overrides(arg: str, flag: str, noun: str) -> str:
    if not arg:
        return ""
    if arg.startswith("@"):
        path = Path(arg[1:]).expanduser()
        if not path.is_file():
            raise ValueError(f"{flag} {arg!r}: {path} is not a file")
        arg = ",".join(line.split("#", 1)[0].strip() for line in path.read_text().splitlines())
    pairs = parse_overrides(arg)
    for key, value in pairs:
        try:
            float(value)
        except ValueError:
            raise ValueError(
                f"{noun} override {key}={value!r}: the value is not a number"
            ) from None
    return ",".join(f"{k}={v}" for k, v in pairs)


def load_base_overrides(arg: str) -> str:
    """An inline override string, or `@file` holding one (commas or newlines between entries)."""
    return _load_overrides(arg, "--base-overrides", "base")


def load_start_overrides(arg: str) -> str:
    """`--start-overrides` read the way `--base-overrides` is."""
    return _load_overrides(arg, "--start-overrides", "start")


def check_base_overrides(
    base: str, knobs: list[Knob], catalogue: Catalogue | None, tunables: dict | None = None
) -> str:
    """Refuse a base key a search knob owns or one that resolves to nothing; return it scoped.

    A key resolves like a search knob's does: a `SONARE_TUNABLE` in src/ or a key
    the library reported. An override that resolves to neither is accepted
    silently by the library and changes nothing.
    """
    if not base:
        return ""
    tunables = scan_tunables() if tunables is None else tunables
    owned = {k.tunable for k in knobs if k.tunable is not None}
    known = catalogue.defaults if catalogue is not None else {}
    out, seen, unresolved, clashing = [], set(), [], []
    for key, value in parse_overrides(base):
        name = resolve_knob_name(key, tunables)
        if name in owned:
            clashing.append(name)
        elif name not in tunables and name not in known:
            unresolved.append(key)
        if name in seen:
            raise ValueError(f"base override {name!r} is given twice")
        seen.add(name)
        out.append(f"{name}={value}")
    if clashing:
        raise ValueError(
            f"base override key(s) also searched as a knob: {', '.join(clashing)}; "
            f"one owner per key — drop it from the spec or from --base-overrides"
        )
    if unresolved:
        raise ValueError(
            f"base override key(s) the library does not know: {', '.join(unresolved)}. "
            f"Run --dump-knobs to list what exists."
        )
    return ",".join(out)


def apply_start_overrides(
    knobs: list[Knob], given: str, base: str = "", tunables: dict | None = None
) -> str:
    """Move named search knobs' starting points without pinning them; return what was applied.

    Every key must be a search knob and none may also be a base override. A start is
    clamped into its knob's range like a spec start is. The knob keeps HEAD's value
    in `head_value`, so write-back and the paste-ready overrides still compare against
    what ships. The returned string carries the clamped values, scoped to knob labels.
    """
    if not given:
        return ""
    tunables = scan_tunables() if tunables is None else tunables
    in_base = {resolve_knob_name(k, tunables) for k, _ in parse_overrides(base)}
    by_name = {}
    for knob in knobs:
        by_name[knob.tunable or knob.label] = knob
        by_name.setdefault(knob.label, knob)
    out, seen, in_both, not_knobs = [], set(), [], []
    for key, value in parse_overrides(given):
        name = resolve_knob_name(key, tunables)
        if name in seen:
            raise ValueError(f"start override {name!r} is given twice")
        seen.add(name)
        if name in in_base:
            in_both.append(name)
        elif name not in by_name:
            not_knobs.append(key)
        else:
            out.append((by_name[name], float(value)))
    if in_both:
        raise ValueError(
            f"start override key(s) also in --base-overrides: {', '.join(in_both)}; "
            f"a base value is fixed, a start value is searched from"
        )
    if not_knobs:
        raise ValueError(
            f"start override key(s) that are not search knobs: {', '.join(not_knobs)}. "
            f"Only a knob the spec searches has a starting point."
        )
    applied = []
    for knob, value in out:
        if knob.head_value is None:
            knob.head_value = knob.start_value
        knob.start_value = _clamp_start(
            value, knob.lo, knob.hi, f"{knob.label} (--start-overrides)"
        )
        applied.append(f"{knob.label}={format_value(knob.start_value)}")
    return ",".join(applied)
