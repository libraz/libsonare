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
from model import Extraction, FunctionSig

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

    rep.handle_keys = sorted(
        k for k in c_index if _is_handle_key(k, handle_prefixes)
    )

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
            if allow.coverage_ok(key, s):
                rep.findings.append(
                    Finding("coverage", key, s, "", allowlisted=True)
                )
                continue
            # Informational when: handle/class API (matched as methods elsewhere)
            # or the CLI surface (curated subset).
            informational = is_handle or s == "cli"
            if is_handle:
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
            # CLI-only commands are an expected curated surface -> informational.
            informational = s == "cli"
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

    return rep


def _config_names(sig: FunctionSig, roles: set[str]) -> list[str]:
    """Core (non-structural) param names with the leading input group removed."""
    names = [p.name for p in sig.core_params()]
    return _strip_leading_input(names, roles)


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
            distinct = set(declared.values())
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
                            f"{s}={sigs[s].file}:{sigs[s].line}" for s in sorted(declared)
                        ),
                    )
                )


# C config param names that are opaque struct/options pointers the facades
# legitimately flatten into individual fields (or fold into an options bag).
_STRUCT_BAG_NAMES = {"config", "params", "options", "overrides", "opts"}


def _order_drift(c_index, indexed, allow, rep: Report, selected, roles: set[str]) -> None:
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


def _input_names(sig: FunctionSig, roles: set[str]) -> list[str]:
    """Leading audio-input role param names of a signature (raw, includes structural)."""
    names = [p.name for p in sig.params]
    n = _leading_input_group(names, roles)
    return names[:n]


def _input_naming(c_index, indexed, allow, rep: Report, selected, roles: set[str]) -> None:
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
        # Include C's input naming where C declares input-role names.
        if key in c_index:
            cg = [p.name for p in c_index[key].params if p.name in roles]
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
                match = next((p for p in sig.params if p.name == pname and p.enum_values), None)
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
                        detail={"param": pname, "sets": {s: list(v) for s, v in sets.items()}},
                    )
                )
