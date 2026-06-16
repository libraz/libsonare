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

    for field in expected_fields:
        descriptor = getattr(cls, field["name"])
        assert descriptor.offset == field["offset"], (
            f"{name}.{field['name']}: ctypes offset {descriptor.offset} != C "
            f"offset {field['offset']}"
        )
        assert descriptor.size == field["size"], (
            f"{name}.{field['name']}: ctypes size {descriptor.size} != C size {field['size']}"
        )
