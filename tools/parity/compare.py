"""Build the cross-surface parity matrix and the drift categories.

Comparison is always anchored on the C API (the canonical ABI). For each
canonical key we line up the C signature against each language surface and emit:

1. coverage   — a free-function DSP C function missing from a surface, OR a
                surface-only symbol with no C counterpart. Handle/class C funcs
                (audio_*, eq_*, engine_*, ...) are bucketed separately as
                informational, and CLI coverage gaps are informational (the CLI
                is a curated subset).
2. default    — Node / WASM / Python disagree on a parameter's default (C has
                none, so this is a facade-vs-facade comparison).
3. order      — a surface's CONFIG param list (after stripping the leading
                audio-input role group from both sides) diverges from the C
                canonical order / count / names.
4. input      — the audio-input params are named inconsistently across facades
                or vs C (e.g. C `sr` vs python `sample_rate`).
5. enum       — the accepted enum/string-union value set for a param differs
                across surfaces.

A finding may be marked ``informational``: it is reported but does not count
toward the non-zero exit code (CI gate). Allowlisted findings are suppressed
entirely (but counted).
"""

from __future__ import annotations

from dataclasses import dataclass, field

from allowlist import Allowlist
from core_defaults import CoreConfig
from model import Extraction, FunctionSig
from normalize import canonical_default, is_empty_collection_default

# Language facade surfaces that share defaults (C carries none).
_FACADE_SURFACES = ("python", "node", "wasm")

# Default audio-input role names (overridable via allowlist [input_roles]).
# A leading run (first 1-2 params) of these is the audio-input group, stripped
# from both sides before the config-order comparison.
DEFAULT_INPUT_ROLES = (
    "samples",
    "sample_rate",
    "sr",
    "left",
    "right",
    "mono",
    "audio",
    "x",
    "y",
    "buffer",
    "data",
    "length",
    # Primary feature-buffer inputs to reconstruction / tempo functions. These
    # play the same leading-input role as ``samples`` (the array the function
    # operates on); facades may rename them for clarity (``mel`` -> ``mel_power``,
    # ``mfcc`` -> ``mfcc_coefficients``). The input-naming check compares the
    # spellings; the order check strips them as the leading input group.
    "mel",
    "mel_power",
    "mfcc",
    "mfcc_coefficients",
    "mfcc_coeffs",
    "onset_envelope",
    "tempogram_data",
    "values",
)

# C prefixes that denote handle / class-based APIs (create/destroy/handle ops
# exposed as class methods on the facades, not free functions). Overridable via
# allowlist [handle_prefixes].
DEFAULT_HANDLE_PREFIXES = (
    "audio",
    "eq",
    "mixer",
    "engine",
    "strip",
    "stream",
    "streaming",
    "realtime",
    "voice",
    "free",
    "master",  # master_audio etc. handled by free-fn detection below if needed
)

# Free-function keys that share a handle prefix but ARE plain DSP free functions
# and must stay in the free-function coverage bucket.
HANDLE_PREFIX_FREEFN_EXCEPTIONS = (
    "master_audio",
    "master_audio_stereo",
    "voice_change",
)

# Full handle-instance prefixes. A C handle key (e.g. ``mixer_add_bus``) carries
# the handle type as a leading token group; the facades expose the same op as a
# bare class method (``Mixer.add_bus`` -> key ``add_bus``). To credit coverage we
# strip the longest matching prefix and retry the tail against the facade's
# method/free keys. Ordered LONGEST-FIRST so multi-token prefixes win over their
# single-token shadows (``stream_analyzer_`` before ``stream_``-style tokens).
_HANDLE_FULL_PREFIXES = (
    "streaming_mastering_chain_",
    "stream_analyzer_",
    "realtime_voice_changer_",
    "engine_",
    "mixer_",
    "strip_",
    "eq_",
    "audio_",
)


@dataclass
class Finding:
    category: str  # coverage | default | order | input | enum
    key: str
    surface: str  # surface the finding is attributed to (or 'cross')
    message: str
    detail: dict = field(default_factory=dict)
    location: str = ""
    allowlisted: bool = False
    informational: bool = False


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    matrix: dict[str, dict[str, bool]] = field(default_factory=dict)
    surfaces: list[str] = field(default_factory=list)
    unparsed: dict[str, int] = field(default_factory=dict)
    unparsed_notes: dict[str, list[str]] = field(default_factory=dict)
    surface_only: dict[str, list[str]] = field(default_factory=dict)
    handle_keys: list[str] = field(default_factory=list)

    def active(self) -> list[Finding]:
        """Findings that count toward failure (non-allowlisted, non-informational)."""
        return [f for f in self.findings if not f.allowlisted and not f.informational]

    def reported(self) -> list[Finding]:
        """All non-allowlisted findings (including informational)."""
        return [f for f in self.findings if not f.allowlisted]


def _index(ex: Extraction) -> dict[str, FunctionSig]:
    # First definition wins for a key on a surface.
    out: dict[str, FunctionSig] = {}
    for f in ex.functions:
        out.setdefault(f.key, f)
    return out


def _method_keys(ex: Extraction | None) -> set[str]:
    """Canonical keys that a surface exposes as a CLASS METHOD (raw_name has a '.')."""
    if ex is None:
        return set()
    return {f.key for f in ex.functions if "." in f.raw_name}


def _is_handle_key(key: str, prefixes: tuple[str, ...]) -> bool:
    if key in HANDLE_PREFIX_FREEFN_EXCEPTIONS:
        return False
    head = key.split("_", 1)[0]
    return head in prefixes


def _leading_input_group(names: list[str], roles: set[str]) -> int:
    """Return the count (0-2) of leading params that form the audio-input group."""
    n = 0
    for nm in names[:2]:
        if nm in roles:
            n += 1
        else:
            break
    return n


def _strip_leading_input(names: list[str], roles: set[str]) -> list[str]:
    return names[_leading_input_group(names, roles) :]


def build_report(
    extractions: dict[str, Extraction],
    allow: Allowlist,
    selected: list[str],
    core_configs: dict[str, CoreConfig] | None = None,
) -> Report:
    rep = Report(surfaces=selected)
    indexed = {s: _index(ex) for s, ex in extractions.items()}
    for s, ex in extractions.items():
        rep.unparsed[s] = ex.unparsed
        rep.unparsed_notes[s] = ex.unparsed_notes
        rep.surface_only[s] = []

    roles = set(allow.input_roles or DEFAULT_INPUT_ROLES)
    handle_prefixes = tuple(allow.handle_prefixes or DEFAULT_HANDLE_PREFIXES)

    c_index = indexed.get("c", {})
    all_keys = set()
    for idx in indexed.values():
        all_keys.update(idx.keys())

    # Class-method keys per facade (for handle/class matching).
    method_keys = {s: _method_keys(extractions.get(s)) for s in selected}

    rep.handle_keys = sorted(k for k in c_index if _is_handle_key(k, handle_prefixes))

    # --- Coverage matrix ---
    for key in sorted(all_keys):
        rep.matrix[key] = {s: (key in indexed.get(s, {})) for s in selected}

    # --- 1. Coverage gaps ---
    handle_set = set(rep.handle_keys)
    for key in sorted(c_index):
        is_handle = key in handle_set
        for s in selected:
            if s == "c":
                continue
            present_free = key in indexed.get(s, {})
            present_method = key in method_keys.get(s, set())
            if present_free or present_method:
                continue
            # Handle-instance C key (``mixer_add_bus``): the facade exposes the
            # same op as a bare class method (``Mixer.add_bus`` -> key
            # ``add_bus``), so the handle prefix is stripped there. Strip the
            # longest matching handle prefix and retry the tail against this
            # surface's method-keys AND free-function keys; a match means the op
            # IS exposed -- covered, no finding.
            stripped = None
            for prefix in _HANDLE_FULL_PREFIXES:
                if key.startswith(prefix) and len(key) > len(prefix):
                    stripped = key[len(prefix) :]
                    break
            if stripped is not None and (
                stripped in method_keys.get(s, set()) or stripped in indexed.get(s, {})
            ):
                continue
            if allow.coverage_ok(key, s):
                rep.findings.append(Finding("coverage", key, s, "", allowlisted=True))
                continue
            # C memory-management helper (``free_floats`` / ``free_*_result`` /
            # ``free_stream_*``): a heap-result / buffer release helper with NO
            # facade counterpart by design (the facades free via GC / RAII).
            # Report it as informational, never an active gap.
            is_free_helper = key.startswith("free_")
            # Informational when: handle/class API (matched as methods elsewhere),
            # a memory-management helper, or the CLI surface (curated subset).
            informational = is_handle or is_free_helper or s == "cli"
            if is_free_helper:
                msg = (
                    f"C memory-management helper '{key}' not exposed in {s} "
                    "(facade uses GC/RAII)"
                )
            elif is_handle:
                msg = f"C handle/class function '{key}' not exposed as {s} method/function"
            elif s == "cli":
                msg = f"C function '{key}' not exposed by the (curated) CLI"
            else:
                msg = f"C function '{key}' is not exposed in {s}"
            rep.findings.append(
                Finding(
                    category="coverage",
                    key=key,
                    surface=s,
                    message=msg,
                    location=f"{c_index[key].file}:{c_index[key].line}",
                    informational=informational,
                )
            )

    # Surface-only symbols (no C counterpart).
    for s in selected:
        if s == "c":
            continue
        for key, sig in indexed.get(s, {}).items():
            if key in c_index:
                continue
            rep.surface_only[s].append(key)
            allowlisted = allow.surface_only_ok(key, s)
            # A surface-only symbol whose raw_name carries a '.' is a facade
            # CLASS METHOD (e.g. "Mixer.add_bus", "Audio.fromBuffer"): an
            # ergonomic handle/instance method with no C free-function
            # counterpart. Keep it visible but informational (out of the
            # CI-failing set). CLI-only commands are likewise an expected
            # curated surface -> informational. Free-function surface-only
            # symbols stay active (real coverage signal worth triaging).
            is_method = "." in sig.raw_name
            informational = is_method or s == "cli"
            rep.findings.append(
                Finding(
                    category="coverage",
                    key=key,
                    surface=s,
                    message=f"{s}-only symbol '{key}' has no C-API counterpart",
                    location=f"{sig.file}:{sig.line}",
                    detail={"raw_name": sig.raw_name},
                    allowlisted=allowlisted,
                    informational=informational and not allowlisted,
                )
            )

    # --- 2. Default drift (facade vs facade) ---
    _default_drift(indexed, allow, rep, roles)

    # --- 3. Arg order / count / name vs C canonical (config params only) ---
    _order_drift(c_index, indexed, allow, rep, selected, roles)

    # --- 4. Input-param naming consistency ---
    _input_naming(c_index, indexed, allow, rep, selected, roles)

    # --- 5. Enum value-set drift ---
    _enum_drift(indexed, allow, rep)

    # --- 6. Core-default drift (facade vs C++ core struct design default) ---
    if core_configs:
        _core_default_drift(indexed, core_configs, allow, rep, roles)

    return rep


def _config_names(sig: FunctionSig, roles: set[str]) -> list[str]:
    """Core (non-structural) config param names.

    The leading audio-input group is removed, and any audio-input ROLE name
    (buffers like ``left`` / ``right``, the sample-rate alias ``sr`` /
    ``sample_rate``, the buffer-companion ``length``, ...) is dropped wherever it
    appears -- a non-leading buffer arg (e.g. the ``left`` / ``right`` channels of
    ``master_audio_stereo``, which sit AFTER ``preset_name`` in the C signature)
    is still an input the facades surface positionally, so it carries no
    config-order signal. Progress-callback params the facades add for streaming
    reporting (``on_progress``; Node already types it as a function and skips it)
    are dropped too.
    """
    names = [p.name for p in sig.core_params()]
    names = _strip_leading_input(names, roles)
    return [n for n in names if n not in roles and n not in _CALLBACK_NAMES]


# Progress / streaming callback params the facades add for ergonomic reporting.
# They have no C config counterpart (C uses a separate ``_with_progress`` entry)
# and carry no config-order signal.
_CALLBACK_NAMES = {"on_progress", "onprogress"}


def _default_drift(indexed, allow, rep: Report, roles: set[str]) -> None:
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    keys = set()
    for s in facades:
        keys.update(indexed[s].keys())
    for key in sorted(keys):
        sigs = {s: indexed[s][key] for s in facades if key in indexed[s]}
        if len(sigs) < 2:
            continue
        param_names: list[str] = []
        for sig in sigs.values():
            for p in sig.core_params():
                if p.name in roles:
                    continue
                if p.name not in param_names:
                    param_names.append(p.name)
        for pname in param_names:
            if allow.default_ok(key, pname):
                continue
            declared: dict[str, str] = {}
            for s, sig in sigs.items():
                match = next((p for p in sig.core_params() if p.name == pname), None)
                if match is None or match.default is None:
                    continue
                declared[s] = match.default
            # Canonicalize for the distinctness test (enum-member vs camelCase
            # string-union literals fold to a common form). The raw surface
            # spelling is still shown in the message.
            canon = {s: canonical_default(v) for s, v in declared.items()}
            # Collection-sentinel equivalence: when at least one facade spells
            # the default as an empty collection (``[]`` / ``{}``) and another
            # spells it ``None``, both mean "no value" -- fold ``none`` to the
            # empty-collection form so they compare equal.
            if any(is_empty_collection_default(v) for v in declared.values()):
                for s, v in declared.items():
                    if canon[s] == "none" or is_empty_collection_default(v):
                        canon[s] = "\0empty-collection\0"
            distinct = set(canon.values())
            if len(distinct) > 1:
                rep.findings.append(
                    Finding(
                        category="default",
                        key=key,
                        surface="cross",
                        message=(
                            f"default drift for '{key}.{pname}': "
                            + ", ".join(f"{s}={declared[s]}" for s in sorted(declared))
                        ),
                        detail={"param": pname, "defaults": declared},
                        location="; ".join(
                            f"{s}={sigs[s].file}:{sigs[s].line}"
                            for s in sorted(declared)
                        ),
                    )
                )


# C config param names that are opaque struct/options pointers the facades
# legitimately flatten into individual fields (or fold into an options bag).
_STRUCT_BAG_NAMES = {"config", "params", "options", "overrides", "opts"}

# Sample-rate spellings. In several C functions (tempogram/mel/mfcc/pcen
# families) ``sr`` / ``sample_rate`` is a POSITIONAL C argument that sits inside
# what the facades expose as a leading config field rather than in the
# audio-input group. The facades consistently spell it ``sample_rate`` /
# ``sampleRate`` and place it just after the primary buffer. Treat it as an
# alias of the C positional ``sr`` and drop it from the config-order comparison
# (it is an intentional, consistent facade convention, not order drift).
_SAMPLE_RATE_NAMES = {"sample_rate", "sr"}

# Buffer-companion length params (the count that accompanies a ``const float*``
# buffer). C carries them as a positional arg; facades derive length from the
# array, so they have no facade counterpart. Stripped before the audio-input
# naming check so the buffer names line up.
_BUFFER_COMPANION_NAMES = {"length", "size", "len"}

# detect_chords is exposed by the facades in its EXTENDED form: the facade
# config is the 7 base params of C ``sonare_detect_chords`` plus the extra
# fields of ``SonareChordDetectionOptions`` (the struct used by the
# ``sonare_detect_chords_ex`` variant). These trailing extended fields are
# legitimate, so the order check accepts the base C order as a prefix.
_CHORD_EXTENDED_FIELDS = (
    "use_hmm",
    "hmm_beam_width",
    "use_key_context",
    "key_root",
    "key_mode",
    "detect_inversions",
    "chroma_method",
)


def _order_drift(
    c_index, indexed, allow, rep: Report, selected, roles: set[str]
) -> None:
    for key, csig in c_index.items():
        c_cfg = _config_names(csig, roles)
        # When C itemizes no config params, or its only config param is an opaque
        # struct/options pointer, the facade flattening it is the bag convention,
        # not order drift. (The itemized C order lives in a `_with_options`
        # variant, compared on its own key.)
        if not c_cfg or all(n in _STRUCT_BAG_NAMES for n in c_cfg):
            continue
        for s in _FACADE_SURFACES:
            if s not in selected or key not in indexed.get(s, {}):
                continue
            if allow.order_ok(key, s):
                continue
            ssig = indexed[s][key]
            s_cfg = _config_names(ssig, roles)
            # Facade may also flatten/fold into a bag: a facade whose config is a
            # subset bag (all names are struct/bag names) carries no order signal.
            if all(n in _STRUCT_BAG_NAMES for n in s_cfg):
                continue
            # Facades fold variadic C scalar tails into an options bag, so a
            # facade exposing a STRICT PREFIX of the C config order is fine.
            if s_cfg == c_cfg[: len(s_cfg)]:
                continue
            # detect_chords is exposed in its EXTENDED (``_ex``) form: C base
            # order followed by the SonareChordDetectionOptions extra fields.
            if (
                key == "detect_chords"
                and s_cfg[: len(c_cfg)] == c_cfg
                and tuple(s_cfg[len(c_cfg) :]) == _CHORD_EXTENDED_FIELDS
            ):
                continue
            rep.findings.append(
                Finding(
                    category="order",
                    key=key,
                    surface=s,
                    message=(
                        f"config param order/name mismatch vs C for '{key}': "
                        f"C={c_cfg} {s}={s_cfg}"
                    ),
                    detail={"c": c_cfg, s: s_cfg},
                    location=f"{ssig.file}:{ssig.line} (C {csig.file}:{csig.line})",
                )
            )


# Audio-input role names that carry no naming signal across surfaces: the
# buffer-companion length count (C-only, facades derive it from the array) and
# the sample-rate alias (a consistent facade convention, see _SAMPLE_RATE_NAMES).
_INPUT_NOISE_NAMES = _BUFFER_COMPANION_NAMES | _SAMPLE_RATE_NAMES


def _input_names(sig: FunctionSig, roles: set[str]) -> list[str]:
    """Leading audio-input buffer param names of a signature.

    The buffer-companion length and sample-rate alias are dropped so only the
    actual buffer parameter names (``samples`` / ``left`` / ``right`` / ...) are
    compared for naming consistency.
    """
    names = [p.name for p in sig.params]
    n = _leading_input_group(names, roles)
    return [nm for nm in names[:n] if nm not in _INPUT_NOISE_NAMES]


def _input_naming(
    c_index, indexed, allow, rep: Report, selected, roles: set[str]
) -> None:
    """Flag when the audio-input params are named inconsistently across surfaces."""
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    keys = set()
    for s in facades:
        keys.update(indexed[s].keys())
    for key in sorted(keys):
        if allow.input_naming_ok(key):
            continue
        groups: dict[str, list[str]] = {}
        for s in facades:
            if key in indexed[s]:
                g = _input_names(indexed[s][key], roles)
                if g:
                    groups[s] = g
        # Include C's input naming where C declares input-role names. Drop the
        # buffer-companion length and the sample-rate alias so only the buffer
        # parameter names are compared.
        if key in c_index:
            cg = [
                p.name
                for p in c_index[key].params
                if p.name in roles and p.name not in _INPUT_NOISE_NAMES
            ]
            if cg:
                groups["c"] = cg
        if len(groups) < 2:
            continue
        distinct = {tuple(v) for v in groups.values()}
        if len(distinct) > 1:
            rep.findings.append(
                Finding(
                    category="input",
                    key=key,
                    surface="cross",
                    message=(
                        f"audio-input naming differs for '{key}': "
                        + "; ".join(f"{s}={groups[s]}" for s in sorted(groups))
                    ),
                    detail={"groups": groups},
                    location="; ".join(
                        f"{s}={indexed[s][key].file}:{indexed[s][key].line}"
                        for s in sorted(groups)
                        if s in indexed and key in indexed[s]
                    ),
                )
            )


def _enum_drift(indexed, allow, rep: Report) -> None:
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    keys = set()
    for s in facades:
        keys.update(indexed[s].keys())
    for key in sorted(keys):
        sigs = {s: indexed[s][key] for s in facades if key in indexed[s]}
        if len(sigs) < 2:
            continue
        param_names: list[str] = []
        for sig in sigs.values():
            for p in sig.params:
                if p.enum_values and p.name not in param_names:
                    param_names.append(p.name)
        for pname in param_names:
            if allow.enum_ok(key, pname):
                continue
            sets: dict[str, tuple[str, ...]] = {}
            for s, sig in sigs.items():
                match = next(
                    (p for p in sig.params if p.name == pname and p.enum_values), None
                )
                if match is not None:
                    sets[s] = match.enum_values
            distinct = {frozenset(v) for v in sets.values()}
            if len(distinct) > 1:
                rep.findings.append(
                    Finding(
                        category="enum",
                        key=key,
                        surface="cross",
                        message=(
                            f"enum value-set drift for '{key}.{pname}': "
                            + "; ".join(f"{s}={sorted(sets[s])}" for s in sorted(sets))
                        ),
                        detail={
                            "param": pname,
                            "sets": {s: list(v) for s, v in sets.items()},
                        },
                    )
                )


def _core_default_drift(
    indexed,
    core_configs: dict[str, CoreConfig],
    allow,
    rep: Report,
    roles: set[str],
) -> None:
    """Flag a facade default that diverges from its C++ core struct initializer.

    The facade-vs-facade default check (``_default_drift``) only sees the three
    facades; it is blind to the case where every facade AGREES on a value the
    C++ core design never intended. This check anchors each mapped facade param
    on the numeric / boolean field initializer of its C++ core config struct
    (see ``core_map.toml`` + ``extractors/cpp_struct.py``) and reports any
    divergence. Only params a facade actually declares a default for, and only
    struct fields with a literal initializer, are compared.
    """
    facades = [s for s in _FACADE_SURFACES if s in indexed]
    for key in sorted(core_configs):
        cfg = core_configs[key]
        if not cfg.fields:
            continue
        for s in facades:
            sig = indexed[s].get(key)
            if sig is None:
                continue
            for p in sig.core_params():
                if p.name in roles or p.default is None:
                    continue
                core_def = cfg.core_default_for(p.name)
                if core_def is None:
                    continue
                if allow.core_default_ok(key, p.name):
                    continue
                if canonical_default(p.default) == canonical_default(core_def):
                    continue
                field_name = cfg.rename.get(p.name, p.name)
                rep.findings.append(
                    Finding(
                        category="core_default",
                        key=key,
                        surface=s,
                        message=(
                            f"core-default drift for '{key}.{p.name}': "
                            f"{s}={p.default} vs C++ {cfg.struct}.{field_name}={core_def}"
                        ),
                        detail={
                            "param": p.name,
                            "facade": p.default,
                            "core": core_def,
                            "struct": cfg.struct,
                        },
                        location=f"{sig.file}:{sig.line} (core {cfg.header})",
                    )
                )
