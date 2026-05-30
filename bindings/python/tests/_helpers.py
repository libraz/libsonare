"""Shared helpers for libsonare Python binding tests."""

from __future__ import annotations


def is_lib_available() -> bool:
    """Returns True if the libsonare shared library is loadable.

    The pytest conftest already short-circuits collection via ``pytest.skip``
    when the .dylib / .so cannot be found on disk, but loading can still fail
    at runtime (e.g. missing symbols, ABI drift). Tests that rely on the C
    layer use this as a final guard so a stale shared library degrades to
    skipped tests instead of import errors.
    """
    try:
        from libsonare._runtime import _get_lib

        _get_lib()
        return True
    except Exception:
        return False


LIB_AVAILABLE: bool = is_lib_available()
