"""Guard the ctypes struct mirror against C-ABI layout drift.

The C++ core protects its structs with ``static_assert(sizeof(...))``; the
hand-written ctypes mirror in ``libsonare._ffi_types_*`` has no such guard, so a
field added, removed, reordered, or retyped on the C side desyncs it silently --
classically surfacing as an import-time segfault rather than a clean error.

``tools/abi/abi-layout.json`` is the authoritative snapshot of the C layout
(``sizeof`` / ``alignof`` / ``offsetof`` straight from the headers, produced by
``tools/abi/gen_abi_layout.py`` / ``make abi-layout``). This test compares each
ctypes ``Structure`` against that snapshot, so any drift fails as a red test
instead of a crash. Regenerate the snapshot with ``make abi-layout`` whenever a C
struct legitimately changes; the diff is then reviewable alongside the C edit.
"""

from __future__ import annotations

import ctypes
import importlib
import json
import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
LAYOUT_JSON = REPO_ROOT / "tools" / "abi" / "abi-layout.json"
INCLUDE_DIR = REPO_ROOT / "include" / "sonare"
SIGNATURE_GLOB = "bindings/python/src/libsonare/_ffi_signatures_*.py"

# Pure-Python ctypes modules (no dlopen) that declare the mirror structs.
FFI_MODULES = (
    "libsonare._ffi_types_core",
    "libsonare._ffi_types_analysis",
    "libsonare._ffi_types_mastering_project",
    "libsonare._ffi_types_repair",
    "libsonare._ffi_types_streaming",
)


# struct-format char -> coarse type category, matching gen_abi_layout.py's
# C++ field_kind(). Catches a same-width type swap (e.g. c_uint32 -> c_int32, or
# c_int32 -> c_float) that sizeof/offset comparison alone cannot see.
_FORMAT_KIND = {
    "f": "float",
    "d": "float",
    "g": "float",
    "b": "signed",
    "h": "signed",
    "i": "signed",
    "l": "signed",
    "q": "signed",
    "n": "signed",
    "c": "signed",
    "B": "unsigned",
    "H": "unsigned",
    "I": "unsigned",
    "L": "unsigned",
    "Q": "unsigned",
    "N": "unsigned",
    "?": "unsigned",
    "P": "pointer",
    "z": "pointer",
    "Z": "pointer",
}


def _ctypes_kind(field_type: type) -> str:
    """Map a ctypes field type to the same coarse category the C probe emits."""
    # Data pointers and function pointers both map to the C probe's "pointer".
    if (
        field_type is ctypes.c_void_p
        or issubclass(field_type, ctypes._Pointer)
        or issubclass(field_type, ctypes._CFuncPtr)
    ):
        return "pointer"
    if issubclass(field_type, (ctypes.Structure, ctypes.Union, ctypes.Array)):
        return "aggregate"
    fmt = getattr(field_type, "_type_", None)
    if isinstance(fmt, str) and len(fmt) == 1:
        return _FORMAT_KIND.get(fmt, "aggregate")
    return "aggregate"


def _mirror_structs() -> dict[str, type[ctypes.Structure]]:
    """Collect every mirrored ctypes Structure keyed by C struct name.

    A mirror is keyed under its own class name AND under every name in its
    ``_c_aliases_`` -- the engine-side instrument configs are separate C
    typedefs that the binding passes this same mirror to. Keying on the class
    name alone left those aliases outside the guard entirely: the layouts
    happen to match today, so nothing failed, but a field added to only one
    side would have desynced silently on a live Python call path.
    """
    found: dict[str, type[ctypes.Structure]] = {}
    for mod_name in FFI_MODULES:
        module = importlib.import_module(mod_name)
        for attr in vars(module).values():
            if (
                isinstance(attr, type)
                and issubclass(attr, ctypes.Structure)
                and attr is not ctypes.Structure
                and getattr(attr, "_fields_", None)
            ):
                for c_name in (attr.__name__, *getattr(attr, "_c_aliases_", ())):
                    found.setdefault(c_name, attr)
    return found


MIRROR_STRUCTS = _mirror_structs()


@pytest.fixture(scope="module")
def layout() -> dict[str, dict]:
    # The snapshot is tracked, so its absence is a broken checkout rather than a
    # build configuration. Skipping here would silently disable the whole
    # layout guard in exactly the situation it is needed, so this fails.
    assert LAYOUT_JSON.exists(), (
        f"{LAYOUT_JSON} is missing. It is a tracked file: restore it, or run "
        "`make abi-layout` and commit the result. Without it the ctypes mirrors "
        "have nothing to compare against and a layout desync segfaults instead."
    )
    return json.loads(LAYOUT_JSON.read_text())


def test_snapshot_is_not_empty(layout: dict[str, dict]) -> None:
    assert layout, "abi-layout.json is empty; run `make abi-layout`"


def test_every_mirror_is_snapshotted(layout: dict[str, dict]) -> None:
    """A ctypes struct absent from the snapshot means the snapshot is stale."""
    missing = sorted(set(MIRROR_STRUCTS) - set(layout))
    assert not missing, (
        f"ctypes mirrors absent from abi-layout.json: {missing}. "
        "Run `make abi-layout` and commit the result."
    )


def _headers_text() -> str:
    """Concatenated public headers with comments stripped and runs collapsed."""
    text = "\n".join(h.read_text(encoding="utf-8") for h in sorted(INCLUDE_DIR.rglob("*.h")))
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r"\s+", " ", text)


def _declared_c_functions() -> set[str]:
    """C function names the Python binding configures argtypes for."""
    names: set[str] = set()
    for path in sorted(REPO_ROOT.glob(SIGNATURE_GLOB)):
        text = path.read_text(encoding="utf-8")
        names |= set(re.findall(r"\blib\.(sonare_\w+)", text))
        names |= set(re.findall(r"""["'](sonare_\w+)["']""", text))
    return names


def test_no_unverified_struct_on_a_python_call_path(layout: dict[str, dict]) -> None:
    """Every C struct Python can pass must have its layout snapshotted.

    The mirror-side checks above only see structs the ctypes modules name, so a
    C struct reached through an *aliased* mirror was invisible to them -- which
    is exactly how the two engine-side instrument configs sat outside the guard.
    This works the other way round: start from the C functions the binding
    declares, read their parameter types out of the headers, and require each
    struct typedef to be snapshotted. A new C struct on a Python call path fails
    here even if no one remembers to name it in ctypes.
    """
    flat = _headers_text()
    struct_typedefs = set(re.findall(r"typedef struct\s*\{[^}]*\}\s*(Sonare\w+)\s*;", flat))
    assert len(struct_typedefs) > 100, (
        f"only {len(struct_typedefs)} struct typedefs parsed from {INCLUDE_DIR}; "
        "the header scan broke rather than the tree shrinking"
    )
    declared = _declared_c_functions()
    assert len(declared) > 400, (
        f"only {len(declared)} C functions parsed from {SIGNATURE_GLOB}; "
        "the signature scan broke rather than the binding shrinking"
    )

    unverified: dict[str, str] = {}
    for fn in sorted(declared):
        match = re.search(rf"\b{re.escape(fn)}\s*\(([^;]*?)\)\s*;", flat)
        if match is None:
            continue
        for name in sorted(set(re.findall(r"\b(Sonare\w+)\b", match.group(1)))):
            if name in struct_typedefs and name not in layout:
                unverified.setdefault(name, fn)

    assert not unverified, (
        "C structs on a Python call path with no layout snapshot: "
        + ", ".join(f"{name} (via {fn})" for name, fn in sorted(unverified.items()))
        + ". Add the C name to the mirroring ctypes class's _c_aliases_ (or give it "
        "its own mirror), then run `make abi-layout`. Left unsnapshotted, a field "
        "added on one side desyncs the ctypes mirror and segfaults instead of "
        "failing here."
    )


@pytest.mark.parametrize("name", sorted(MIRROR_STRUCTS))
def test_struct_layout_matches_c(name: str, layout: dict[str, dict]) -> None:
    """ctypes sizeof / alignment / field offsets must match the C ABI snapshot."""
    cls = MIRROR_STRUCTS[name]
    expected = layout.get(name)
    assert expected is not None, f"{name} not in abi-layout.json; run `make abi-layout`"

    assert ctypes.sizeof(cls) == expected["size"], (
        f"{name}: ctypes sizeof {ctypes.sizeof(cls)} != C sizeof "
        f"{expected['size']} -- field set or types drifted from the C struct"
    )
    assert ctypes.alignment(cls) == expected["align"], (
        f"{name}: ctypes alignment {ctypes.alignment(cls)} != C alignment {expected['align']}"
    )

    expected_fields = expected["fields"]
    ctypes_names = [field_name for field_name, *_ in cls._fields_]
    assert ctypes_names == [f["name"] for f in expected_fields], (
        f"{name}: ctypes field names/order {ctypes_names} != C "
        f"{[f['name'] for f in expected_fields]}"
    )

    field_types = {field_name: field_type for field_name, field_type, *_ in cls._fields_}
    for field in expected_fields:
        descriptor = getattr(cls, field["name"])
        assert descriptor.offset == field["offset"], (
            f"{name}.{field['name']}: ctypes offset {descriptor.offset} != C "
            f"offset {field['offset']}"
        )
        assert descriptor.size == field["size"], (
            f"{name}.{field['name']}: ctypes size {descriptor.size} != C size {field['size']}"
        )
        # Same-width type swaps (sign flips, int<->float) keep offset and size
        # identical, so guard the coarse type category too.
        expected_kind = field.get("kind")
        if expected_kind is not None:
            actual_kind = _ctypes_kind(field_types[field["name"]])
            assert actual_kind == expected_kind, (
                f"{name}.{field['name']}: ctypes kind {actual_kind!r} != C kind "
                f"{expected_kind!r} -- a same-width type swap drifted from the C struct"
            )
