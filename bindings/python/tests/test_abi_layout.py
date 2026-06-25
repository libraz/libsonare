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
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
LAYOUT_JSON = REPO_ROOT / "tools" / "abi" / "abi-layout.json"

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
    """Collect every mirrored ctypes Structure by class name (dedup re-exports)."""
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
                found.setdefault(attr.__name__, attr)
    return found


MIRROR_STRUCTS = _mirror_structs()


@pytest.fixture(scope="module")
def layout() -> dict[str, dict]:
    if not LAYOUT_JSON.exists():
        pytest.skip(f"{LAYOUT_JSON} missing; run `make abi-layout`")
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
