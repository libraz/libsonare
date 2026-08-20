"""Extract the Python surface from .pyi stubs / .py modules via stdlib ``ast``.

We parse the type stubs (``analyzer.pyi``, ``audio.pyi``, ``engine.pyi``) which
carry the richest signatures (Literal enums, documented defaults), then parse the
matching implementation modules (``analyzer.py``, ``audio.py``, ``engine.py``)
and the facade modules that have no ``.pyi`` at all (``streaming.py``,
``_effects.py``, ``_mixing.py``, ``_mastering.py``, ``_analysis.py``,
``_features.py``, ``_acoustic.py``, ``_conversions.py``, ``_project.py``).

Order matters: stubs are listed FIRST so their signatures win the index for keys
they cover, and the ``.py`` implementations are listed AFTER to backfill members
the stubs lag on (the hand-written stubs drift from the inline-typed
implementations — e.g. ``engine.pyi`` long missed ``push_midi_cc`` /
``clear_parameters``). ``ast`` reads signatures only (bodies are ignored), so
reading a ``.py`` yields the same surface a stub would; without these modules the
checker was blind to whole facade classes (the StreamAnalyzer / mixing / effects
method sets), reporting them as phantom handle-coverage gaps.

Methods on classes (``Audio``, ``Mixer``, ``Project``, ...) are emitted too;
their ``self`` parameter is marked structural.

Which modules get parsed cannot be derived here the way the TS facades derive
theirs from an ``export *`` graph: this package composes its public surface two
ways, star re-export from a split sibling into its parent AND mixin inheritance
into the exported handle classes, and only the first is visible to an import
walk. The file set is therefore enumerated -- but every module must be either in
:data:`_SCANNED` or in a :data:`_NOT_SCANNED` family, and :func:`_require_classified`
raises when one is in neither, so a module added later cannot go unnoticed.

:data:`_NOT_SCANNED` decides WHAT IS PARSED, not what counts as exposed. It is
not an allowlist: excluding a module asserts it carries no public signature to
compare, never that findings from it are accepted. Suppressing a real finding is
``allowlist.toml``'s job, under review.
"""

from __future__ import annotations

import ast
from pathlib import Path

from model import Extraction, FunctionSig, Param
from normalize import canonical_key, normalize_default, normalize_param_name

from . import python_ctypes


def _ann_to_str(node: ast.expr | None) -> str:
    if node is None:
        return ""
    try:
        return ast.unparse(node)
    except Exception:  # noqa: BLE001
        return ""


def _default_to_str(node: ast.expr | None) -> str | None:
    if node is None:
        return None
    try:
        return ast.unparse(node)
    except Exception:  # noqa: BLE001
        return None


def _enum_values_from_ann(ann: str) -> tuple[str, ...]:
    """Pull string-literal members out of a ``Literal[...]`` / union annotation."""
    import re

    vals = re.findall(r"""['"]([^'"]+)['"]""", ann)
    return tuple(sorted(set(vals)))


def _params_from_args(args: ast.arguments) -> list[Param]:
    params: list[Param] = []
    positional = list(args.posonlyargs) + list(args.args)
    # Align defaults to the tail of positional args.
    n_def = len(args.defaults)
    pad = [None] * (len(positional) - n_def)
    pos_defaults = pad + list(args.defaults)
    for a, d in zip(positional, pos_defaults):
        ann = _ann_to_str(a.annotation)
        structural = a.arg in ("self", "cls")
        params.append(
            Param(
                name=normalize_param_name(a.arg),
                raw_name=a.arg,
                default=normalize_default(_default_to_str(d)),
                type=ann,
                enum_values=_enum_values_from_ann(ann),
                structural=structural,
            )
        )
    # Keyword-only args (after ``*``): e.g. ``*, validate: bool = True``.
    for a, d in zip(args.kwonlyargs, args.kw_defaults):
        ann = _ann_to_str(a.annotation)
        structural = a.arg in ("validate",)
        params.append(
            Param(
                name=normalize_param_name(a.arg),
                raw_name=a.arg,
                default=normalize_default(_default_to_str(d)),
                type=ann,
                enum_values=_enum_values_from_ann(ann),
                structural=structural,
            )
        )
    return params


def _walk(
    node: ast.AST,
    ex: Extraction,
    file: str,
    class_bases: dict[str, list[str]],
    class_prefix: str = "",
) -> None:
    for child in ast.iter_child_nodes(node):
        if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
            name = child.name
            if name.startswith("_"):
                continue
            key = canonical_key(name, "python")
            ex.functions.append(
                FunctionSig(
                    key=key,
                    surface="python",
                    raw_name=(class_prefix + "." + name) if class_prefix else name,
                    params=_params_from_args(child.args),
                    returns=_ann_to_str(child.returns),
                    file=file,
                    line=child.lineno,
                )
            )
        elif isinstance(child, ast.ClassDef):
            # Record simple-name bases so ``extract`` can flatten mixin methods
            # (e.g. ``Project(_ProjectEditMixin, ...)``) onto the concrete class.
            bases = [b.id for b in child.bases if isinstance(b, ast.Name)]
            if bases:
                class_bases.setdefault(child.name, []).extend(bases)
            _walk(child, ex, file, class_bases, class_prefix=child.name)


def _flatten_mixin_methods(ex: Extraction, class_bases: dict[str, list[str]]) -> None:
    """Re-emit inherited methods under each concrete subclass name.

    The Python facades compose their handle classes from split mixins
    (``Project(_ProjectEditMixin, _ProjectMidiMixin, ...)``), so a method is
    physically defined on the mixin, not on ``Project``. Handle-op parity keys
    on the concrete class name, so without this pass every mixin method reads as
    "not exposed on the python facade". Resolve the bases transitively and add a
    ``Subclass.method`` alias for every method a subclass inherits but does not
    override.
    """
    own_methods: dict[str, dict[str, FunctionSig]] = {}
    for f in ex.functions:
        cls, sep, meth = f.raw_name.partition(".")
        if sep:
            own_methods.setdefault(cls, {})[meth] = f

    def inherited(cls: str, seen: set[str]) -> dict[str, FunctionSig]:
        acc: dict[str, FunctionSig] = {}
        for parent in class_bases.get(cls, []):
            if parent in seen:
                continue
            seen.add(parent)
            acc.update(inherited(parent, seen))
            acc.update(own_methods.get(parent, {}))
        return acc

    for cls in list(class_bases):
        own = own_methods.get(cls, {})
        for meth, f in inherited(cls, set()).items():
            if meth in own:
                continue
            ex.functions.append(
                FunctionSig(
                    key=f.key,
                    surface="python",
                    raw_name=f"{cls}.{meth}",
                    params=f.params,
                    returns=f.returns,
                    file=f.file,
                    line=f.line,
                )
            )


_PACKAGE_REL = "bindings/python/src/libsonare"

_SCANNED: tuple[str, ...] = (
    # .pyi stubs are parsed FIRST so their richer signatures (Literal enums,
    # documented defaults) win the index for shared keys; the matching .py
    # implementations are parsed AFTER to backfill methods the stubs lag on
    # (the stubs drift from the inline-typed implementations over time).
    "analyzer.pyi",
    "audio.pyi",
    "engine.pyi",
    "analyzer.py",
    "audio.py",
    "engine.py",
    # The facade modules below were split out of the monolithic
    # ``_project.py`` / ``_effects.py`` / ``_features.py`` / ``_mastering.py``
    # / ``_analysis.py`` / ``engine.py`` files; each split sibling must be
    # listed here or its members read as "not exposed in python".
    "_project.py",
    "_project_synth.py",
    "_project_edit.py",
    "_project_inspection.py",
    "_project_midi.py",
    "_project_model.py",
    "_project_render.py",
    "streaming.py",
    "_effects.py",
    "_effects_editing.py",
    "_effects_mastering.py",
    "_effects_separation.py",
    "_effects_voice.py",
    "_mixing.py",
    "_mastering.py",
    "_mastering_offline.py",
    "_mastering_pair.py",
    "_mastering_streaming.py",
    "_analysis.py",
    "_analysis_detection.py",
    "_analysis_music.py",
    "_analysis_reports.py",
    "_features.py",
    "_features_core.py",
    "_features_metering.py",
    "_features_transforms.py",
    "_engine_conversions.py",
    "_engine_io.py",
    "_engine_midi.py",
    "_engine_mixing.py",
    "_engine_pages.py",
    "_acoustic.py",
    "_conversions.py",
)

# Modules deliberately NOT parsed, each family with the reason it carries no
# signature to compare. This governs WHAT IS SCANNED, not what counts as
# exposed: excluding a module asserts "there is no public signature here",
# never "the findings from here are accepted". It is not an allowlist and must
# not be used to silence one -- suppressing a real finding is what
# tools/parity/allowlist.toml is for, under review.
#
# Names are spelled out rather than matched by prefix so a new sibling in an
# excluded family still has to be classified by hand instead of being swallowed
# by a pattern.
_NOT_SCANNED: tuple[tuple[str, tuple[str, ...]], ...] = (
    (
        "ctypes plumbing: Structure mirrors and argtypes registration, no public signatures",
        (
            "_ffi.py",
            "_ffi_types.py",
            "_ffi_types_analysis.py",
            "_ffi_types_core.py",
            "_ffi_types_mastering_project.py",
            "_ffi_types_repair.py",
            "_ffi_types_streaming.py",
            "_ffi_signatures_core.py",
            "_ffi_signatures_effects_engine.py",
            "_ffi_signatures_extra.py",
            "_ffi_signatures_features.py",
            "_ffi_signatures_mastering.py",
            "_ffi_signatures_mixing.py",
            "_ffi_signatures_project.py",
            "_ffi_signatures_repair_dynamics.py",
        ),
    ),
    (
        "CLI surface: extracted by extractors/cli.py, whose own file set is being reworked",
        (
            "cli.py",
            "_cli_advanced.py",
            "_cli_analysis.py",
            "_cli_common.py",
            "_cli_effects.py",
            "_cli_inventory.py",
            "_cli_mastering.py",
            "_cli_project.py",
        ),
    ),
    (
        "result/config shapes: dataclasses and TypedDicts, no methods to compare",
        (
            "types.py",
            "types.pyi",
            "_types_analysis.py",
            "_types_engine.py",
            "_types_streaming.py",
        ),
    ),
    (
        "package re-export: no definitions of its own",
        ("__init__.py", "__init__.pyi"),
    ),
    (
        "internal helpers: process-local plumbing with no C counterpart",
        ("_runtime.py", "_cancellation.py", "_facade.py"),
    ),
)


class PythonScopeError(RuntimeError):
    """Raised when the package contains a module that is neither scanned nor excluded."""


def _require_classified(base: Path) -> None:
    """Require every package module to be either scanned or explicitly excluded.

    A hand-maintained file list stops covering what it does not name, silently:
    a facade module split out of an existing one reads as "not exposed in
    python" with no signal that anything is missing. Demanding a decision per
    module turns that into a failure at the moment the module is added.
    """
    excluded: dict[str, str] = {}
    for reason, names in _NOT_SCANNED:
        for name in names:
            excluded[name] = reason

    def _subject(names: list[str]) -> str:
        return f"{', '.join(names)} {'are' if len(names) > 1 else 'is'}"

    scanned = set(_SCANNED)
    both = sorted(scanned & set(excluded))
    if both:
        raise PythonScopeError(
            f"{_PACKAGE_REL}: {_subject(both)} in BOTH the scanned list and an "
            "exclusion family. Remove each from whichever list does not apply."
        )

    present = {p.name for p in base.iterdir() if p.suffix in (".py", ".pyi")}
    missing = sorted((scanned | set(excluded)) - present)
    if missing:
        raise PythonScopeError(
            f"{_PACKAGE_REL}: {_subject(missing)} listed but no longer exists on disk. "
            "Drop the stale listing, or restore the file if the deletion was unintended."
        )
    unclassified = sorted(present - scanned - set(excluded))
    if unclassified:
        raise PythonScopeError(
            f"{_PACKAGE_REL}: {_subject(unclassified)} neither scanned nor excluded. "
            "Add each to _SCANNED if it carries public signatures the C API should be "
            "compared against, or to a _NOT_SCANNED family with a one-line reason if it "
            "does not. Refusing to run rather than silently skipping it, because an "
            "unscanned facade module reads as 'not exposed in python'."
        )


def extract(root: Path) -> Extraction:
    """Parse the Python surface.

    @throws PythonScopeError when a package module is unclassified, classified
            twice, or listed but absent.
    """
    ex = Extraction(surface="python")
    class_bases: dict[str, list[str]] = {}
    base = root / _PACKAGE_REL
    _require_classified(base)
    for fname in _SCANNED:
        path = base / fname
        text = path.read_text(encoding="utf-8")
        try:
            tree = ast.parse(text, filename=str(path))
        except SyntaxError as e:
            ex.unparsed += 1
            ex.unparsed_notes.append(f"{fname}: parse error {e}")
            continue
        _walk(tree, ex, str(path.relative_to(root)), class_bases)
    _flatten_mixin_methods(ex, class_bases)
    python_ctypes.extract_records(root, ex)
    return ex
