"""Guard the ctypes ``argtypes`` / ``restype`` declarations against the C headers.

``test_abi_layout`` covers struct *interiors*; nothing covered the *call
signatures*. A parameter declared ``ctypes.c_uint32`` where the C function takes
``size_t`` hands the callee a 4-byte argument slot for an 8-byte read, and every
Python test still passes -- libffi happens to zero-extend, so the defect is
silent until a value, a platform, or a calling convention stops being forgiving.

This test reads the real signature out of ``include/sonare/*.h`` and compares it,
positionally, against every ctypes declaration the binding makes. The comparison
is by *ABI shape* (width, signedness, pointer depth, aggregate identity) rather
than by ctypes class identity, so genuinely interchangeable spellings -- ``int``
vs ``c_int32``, ``c_char_p`` vs ``POINTER(c_char)`` -- agree, while a width or
sign change fails.

The declarations are collected by running the ``configure_*_signatures``
functions against a recording stub instead of a real ``CDLL``. That reaches the
declarations made inside ``hasattr`` guards and ``for name in (...)`` loops,
which a static read of the source misses.
"""

from __future__ import annotations

import ctypes
import importlib
import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
INCLUDE_DIR = REPO_ROOT / "include" / "sonare"

# Each signature module and the entry point that populates a CDLL from it.
SIGNATURE_MODULES = (
    ("libsonare._ffi_signatures_core", "configure_core_signatures"),
    ("libsonare._ffi_signatures_effects_engine", "configure_effects_engine_signatures"),
    ("libsonare._ffi_signatures_extra", "configure_extra_signatures"),
    ("libsonare._ffi_signatures_features", "configure_features_signatures"),
    ("libsonare._ffi_signatures_mastering", "configure_mastering_signatures"),
    ("libsonare._ffi_signatures_mixing", "configure_mixing_signatures"),
    ("libsonare._ffi_signatures_project", "configure_project_signatures"),
    ("libsonare._ffi_signatures_repair_dynamics", "configure_repair_dynamics_signatures"),
)

# Pure-Python ctypes modules (no dlopen) holding the struct mirrors.
FFI_TYPE_MODULES = (
    "libsonare._ffi_types_core",
    "libsonare._ffi_types_analysis",
    "libsonare._ffi_types_mastering_project",
    "libsonare._ffi_types_repair",
    "libsonare._ffi_types_streaming",
)

# Non-vacuity floors. A parser that silently stops matching would otherwise turn
# this whole file green by comparing nothing.
MIN_FUNCTIONS = 600
MIN_ARGUMENTS = 2500
MIN_C_DECLARATIONS = 600
MIN_STRUCT_TYPEDEFS = 140


def _integer_shape(sign: str, ctypes_type: type) -> str:
    """Shape token for a platform integer whose width follows the build target."""
    return f"{sign}{ctypes.sizeof(ctypes_type) * 8}"


# C scalar -> ABI shape. Fixed-width and platform types both resolve through
# ctypes so the expectation follows the build target rather than an assumption.
_C_SCALAR_SHAPES = {
    "void": "void",
    "char": "char",
    "signed char": "i8",
    "unsigned char": "u8",
    "float": "f32",
    "double": "f64",
    "bool": "u8",
    "int": _integer_shape("i", ctypes.c_int),
    "unsigned int": _integer_shape("u", ctypes.c_uint),
    "unsigned": _integer_shape("u", ctypes.c_uint),
    "long": _integer_shape("i", ctypes.c_long),
    "unsigned long": _integer_shape("u", ctypes.c_ulong),
    "size_t": _integer_shape("u", ctypes.c_size_t),
    "ptrdiff_t": _integer_shape("i", ctypes.c_ssize_t),
    "int8_t": "i8",
    "uint8_t": "u8",
    "int16_t": "i16",
    "uint16_t": "u16",
    "int32_t": "i32",
    "uint32_t": "u32",
    "int64_t": "i64",
    "uint64_t": "u64",
}

# An enum's underlying type is implementation-defined; every supported target
# gives it int width, and the binding spells that as either c_int32 or c_uint32
# depending on how the ordinals are consumed. Both occupy the same slot, so the
# expectation is the width, not the sign.
_ENUM_SHAPES = frozenset({"i32", "u32"})

_STRUCT_FORMAT_SHAPES = {
    "f": "f",
    "d": "f",
    "g": "f",
    "b": "i",
    "h": "i",
    "i": "i",
    "l": "i",
    "q": "i",
    "n": "i",
    "B": "u",
    "H": "u",
    "I": "u",
    "L": "u",
    "Q": "u",
    "N": "u",
    "?": "u",
}


def _flattened_headers() -> str:
    """Public headers with comments and preprocessor lines removed, on one line."""
    text = "\n".join(p.read_text(encoding="utf-8") for p in sorted(INCLUDE_DIR.rglob("*.h")))
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    # Directives (and their line continuations) would otherwise leak macro
    # bodies into the return type of the declaration that follows them.
    text = re.sub(r"^[ \t]*#.*(?:\\\n.*)*$", "", text, flags=re.M)
    return re.sub(r"\s+", " ", text)


_FLAT_HEADERS = _flattened_headers()

# `typedef struct X X;` with no body is an opaque handle: the binding only ever
# holds its address, so it is a void* on the Python side.
OPAQUE_HANDLES = frozenset(re.findall(r"typedef struct (\w+) \1\s*;", _FLAT_HEADERS))
ENUM_TYPEDEFS = frozenset(re.findall(r"typedef enum \{[^}]*\}\s*(\w+)\s*;", _FLAT_HEADERS))
STRUCT_TYPEDEFS = frozenset(
    re.findall(r"typedef struct (?:\w+ )?\{[^}]*\}\s*(\w+)\s*;", _FLAT_HEADERS)
)
CALLBACK_TYPEDEFS = frozenset(re.findall(r"typedef [\w ]+\s*\(\s*\*\s*(\w+)\s*\)", _FLAT_HEADERS))


def _c_declarations() -> dict[str, tuple[str, list[str]]]:
    """C function name -> (return type, parameter declarations) from the headers."""
    found: dict[str, tuple[str, list[str]]] = {}
    for ret, name, params in re.findall(
        r"([A-Za-z_][\w\s\*]*?)\b(sonare_\w+)\s*\(([^;{]*?)\)\s*;", _FLAT_HEADERS
    ):
        stripped = params.strip()
        parsed = [] if stripped in ("", "void") else [p.strip() for p in stripped.split(",")]
        found[name] = (ret.strip(), parsed)
    return found


C_DECLARATIONS = _c_declarations()


def _struct_mirrors() -> dict[str, type[ctypes.Structure]]:
    """C struct name -> the ctypes Structure the binding passes for it."""
    found: dict[str, type[ctypes.Structure]] = {}
    for module_name in FFI_TYPE_MODULES:
        module = importlib.import_module(module_name)
        for attr in vars(module).values():
            if (
                isinstance(attr, type)
                and issubclass(attr, ctypes.Structure)
                and getattr(attr, "_fields_", None)
            ):
                for c_name in (attr.__name__, *getattr(attr, "_c_aliases_", ())):
                    found.setdefault(c_name, attr)
    return found


STRUCT_MIRRORS = _struct_mirrors()


def _strip_parameter_name(declaration: str) -> str:
    """Drop a parameter's identifier, keeping its type.

    ``const SonareProjectClipTake* takes`` -> ``const SonareProjectClipTake*``.
    A declaration ending in ``*`` (``const char* const*``) has no identifier.
    """
    text = declaration.strip()
    if text.endswith("*"):
        return text
    return re.sub(r"\s*\b\w+$", "", text)


def _expected_shapes(c_type: str) -> frozenset[str]:
    """ABI shapes a C type may legitimately be declared as on the ctypes side."""
    pointer_depth = c_type.count("*")
    base = re.sub(r"\bconst\b", " ", c_type).replace("*", " ")
    base = re.sub(r"\s+", " ", base).strip()
    suffix = "*" * pointer_depth

    if base in OPAQUE_HANDLES:
        # A handle is only ever reached through a pointer; one level collapses
        # into void*, so the remaining depth rides on top of it.
        return frozenset({"void" + suffix}) if pointer_depth else frozenset()
    if base in CALLBACK_TYPEDEFS:
        return frozenset({"callback" + suffix})
    if base in ENUM_TYPEDEFS:
        return frozenset(shape + suffix for shape in _ENUM_SHAPES)
    if base in STRUCT_TYPEDEFS:
        mirror = STRUCT_MIRRORS.get(base)
        return frozenset({(mirror.__name__ if mirror else base) + suffix})
    if base in _C_SCALAR_SHAPES:
        return frozenset({_C_SCALAR_SHAPES[base] + suffix})
    return frozenset()


def _actual_shape(ctypes_type: object) -> str:
    """ABI shape of a ctypes type, in the same vocabulary as `_expected_shapes`."""
    if ctypes_type is None:
        return "void"
    if ctypes_type is ctypes.c_void_p:
        return "void*"
    # c_char_p and POINTER(c_char) are the same pointer-to-char slot.
    if ctypes_type is ctypes.c_char_p:
        return "char*"
    if ctypes_type is ctypes.c_char:
        return "char"
    if not isinstance(ctypes_type, type):
        return f"?{ctypes_type!r}"
    if issubclass(ctypes_type, ctypes._Pointer):
        return _actual_shape(ctypes_type._type_) + "*"
    if issubclass(ctypes_type, ctypes._CFuncPtr):
        return "callback"
    if issubclass(ctypes_type, (ctypes.Structure, ctypes.Union)):
        return ctypes_type.__name__
    if issubclass(ctypes_type, ctypes.Array):
        return _actual_shape(ctypes_type._type_) + "[]"
    fmt = getattr(ctypes_type, "_type_", None)
    kind = _STRUCT_FORMAT_SHAPES.get(fmt) if isinstance(fmt, str) else None
    if kind is None:
        return f"?{ctypes_type.__name__}"
    return f"{kind}{ctypes.sizeof(ctypes_type) * 8}"


class _SignatureRecorder:
    """Stands in for a ctypes function pointer, capturing what is assigned to it."""

    def __init__(self) -> None:
        object.__setattr__(self, "declared", {})

    def __setattr__(self, name: str, value: object) -> None:
        self.declared[name] = value


class _RecordingLib:
    """Stands in for the loaded CDLL.

    Every attribute resolves, so ``hasattr``-guarded blocks all run and the
    declarations they contain are captured even when the real build omits the
    symbol.
    """

    def __init__(self) -> None:
        object.__setattr__(self, "functions", {})

    def __getattr__(self, name: str) -> _SignatureRecorder:
        if name.startswith("__"):
            raise AttributeError(name)
        return self.functions.setdefault(name, _SignatureRecorder())


def _declared_signatures() -> dict[str, dict[str, dict[str, object]]]:
    """Signature module -> C function name -> the declared argtypes / restype."""
    per_module: dict[str, dict[str, dict[str, object]]] = {}
    for module_name, entry_point in SIGNATURE_MODULES:
        lib = _RecordingLib()
        getattr(importlib.import_module(module_name), entry_point)(lib)
        per_module[module_name] = {
            name: recorder.declared
            for name, recorder in lib.functions.items()
            if "argtypes" in recorder.declared
        }
    return per_module


DECLARED_SIGNATURES = _declared_signatures()


def _compare(name: str, declared: dict[str, object]) -> list[str]:
    """Mismatches between one ctypes declaration and its C prototype."""
    return_type, parameters = C_DECLARATIONS[name]
    problems: list[str] = []

    argtypes = list(declared["argtypes"])
    if len(argtypes) != len(parameters):
        return [
            f"{name}: ctypes declares {len(argtypes)} arguments, C declares "
            f"{len(parameters)} ({', '.join(parameters) or 'void'})"
        ]

    for index, (declared_type, parameter) in enumerate(zip(argtypes, parameters, strict=True)):
        c_type = _strip_parameter_name(parameter)
        expected = _expected_shapes(c_type)
        actual = _actual_shape(declared_type)
        if not expected:
            problems.append(f"{name} arg {index}: C type {c_type!r} is not in the shape table")
        elif actual not in expected:
            problems.append(
                f"{name} arg {index} ({parameter}): ctypes shape {actual!r} != C shape "
                f"{'/'.join(sorted(expected))!r}"
            )

    expected_return = _expected_shapes(return_type)
    actual_return = _actual_shape(declared.get("restype"))
    if not expected_return:
        problems.append(f"{name} restype: C type {return_type!r} is not in the shape table")
    elif actual_return not in expected_return:
        problems.append(
            f"{name} restype: ctypes shape {actual_return!r} != C shape "
            f"{'/'.join(sorted(expected_return))!r}"
        )
    return problems


def test_header_scan_is_not_vacuous() -> None:
    """The header parse must stay productive, or every comparison below is empty."""
    assert len(C_DECLARATIONS) >= MIN_C_DECLARATIONS, (
        f"only {len(C_DECLARATIONS)} sonare_* prototypes parsed from {INCLUDE_DIR}; "
        "the header scan broke rather than the C ABI shrinking"
    )
    assert len(STRUCT_TYPEDEFS) >= MIN_STRUCT_TYPEDEFS, (
        f"only {len(STRUCT_TYPEDEFS)} struct typedefs parsed from {INCLUDE_DIR}; "
        "the header scan broke rather than the C ABI shrinking"
    )
    assert OPAQUE_HANDLES and ENUM_TYPEDEFS and CALLBACK_TYPEDEFS


def test_signature_scan_is_not_vacuous() -> None:
    """Every signature module must contribute, and the totals must stay plausible."""
    empty = sorted(name for name, declared in DECLARED_SIGNATURES.items() if not declared)
    assert not empty, f"signature modules that declared nothing: {empty}"

    functions = {name for declared in DECLARED_SIGNATURES.values() for name in declared}
    assert len(functions) >= MIN_FUNCTIONS, (
        f"only {len(functions)} ctypes signatures captured; the recorder stopped "
        "reaching the declarations rather than the binding shrinking"
    )

    arguments = sum(
        len(signature["argtypes"])
        for declared in DECLARED_SIGNATURES.values()
        for signature in declared.values()
    )
    assert arguments >= MIN_ARGUMENTS, (
        f"only {arguments} arguments captured across {len(functions)} functions; "
        "the recorder is seeing truncated argtypes"
    )


def test_every_declared_symbol_exists_in_the_headers() -> None:
    """A ctypes declaration for a name the C ABI does not export is dead or typoed."""
    unknown = sorted(
        {
            name
            for declared in DECLARED_SIGNATURES.values()
            for name in declared
            if name not in C_DECLARATIONS
        }
    )
    assert not unknown, (
        f"ctypes signatures declared for symbols absent from {INCLUDE_DIR}: {unknown}. "
        "Either the C prototype moved out of the public headers or the name is misspelled; "
        "a misspelled name silently configures a function that is never called."
    )


def test_every_declared_function_has_a_restype() -> None:
    """An unset restype defaults to c_int, which silently truncates a pointer return."""
    missing = sorted(
        {
            name
            for declared in DECLARED_SIGNATURES.values()
            for name, signature in declared.items()
            if "restype" not in signature
        }
    )
    assert not missing, f"ctypes signatures with argtypes but no restype: {missing}"


@pytest.mark.parametrize("module_name", [name for name, _ in SIGNATURE_MODULES])
def test_ctypes_signatures_match_the_c_headers(module_name: str) -> None:
    """Every argtype and restype must have the C prototype's ABI shape.

    A width mismatch here (the classic one is c_uint32 against a size_t
    parameter) passes every functional test on x86-64 and arm64 because libffi
    widens the slot for you. Nothing else in the suite compares these two
    sources, so a drift stays invisible until it reaches a platform or a value
    that does not forgive it.
    """
    problems: list[str] = []
    for name, declared in sorted(DECLARED_SIGNATURES[module_name].items()):
        if name in C_DECLARATIONS:
            problems.extend(_compare(name, declared))

    assert not problems, (
        f"{len(problems)} ctypes/C signature mismatches in {module_name}:\n  "
        + "\n  ".join(problems)
    )
