"""Low-level ctypes wrapper for libsonare."""

from __future__ import annotations

import ctypes
import ctypes.util
import os
import platform
from pathlib import Path

from ._ffi_signatures_core import configure_core_signatures
from ._ffi_signatures_effects_engine import configure_effects_engine_signatures
from ._ffi_signatures_extra import configure_extra_signatures
from ._ffi_signatures_features import configure_features_signatures
from ._ffi_signatures_mastering import configure_mastering_signatures
from ._ffi_signatures_mixing import configure_mixing_signatures
from ._ffi_signatures_project import configure_project_signatures
from ._ffi_signatures_repair_dynamics import configure_repair_dynamics_signatures
from ._ffi_types import *  # noqa: F403

_type_exports = [name for name in globals() if name.startswith(("Sonare", "SONARE_"))]

EXPECTED_ABI_VERSION = 0x04020105

# --- Library discovery ---


def _find_library() -> str:
    """Find the libsonare shared library.

    Search order:
        1. SONARE_LIB_PATH environment variable
        2. Build directory (development) -- checked before the package-adjacent
           copy so an editable/source checkout always picks up a freshly built
           library instead of a stale copy an older build may have left behind
        3. Package-adjacent (wheel distribution)
        4. System library path
    """
    env_path = os.environ.get("SONARE_LIB_PATH")
    if env_path and Path(env_path).exists():
        return env_path

    pkg_dir = Path(__file__).parent
    # In editable/source checkouts, prefer the freshly built shared library over
    # any package-adjacent copy that may have been left by an older build.
    project_root = pkg_dir.parent.parent.parent.parent
    lib_name = "libsonare.dylib" if platform.system() == "Darwin" else "libsonare.so"
    build_path = project_root / "build" / "lib" / lib_name
    if build_path.exists():
        return str(build_path)

    for name in ("libsonare.dylib", "libsonare.so", "sonare.dll"):
        candidate = pkg_dir / name
        if candidate.exists():
            return str(candidate)

    path = ctypes.util.find_library("sonare")
    if path:
        return path

    raise OSError(
        "libsonare shared library not found. "
        "Set SONARE_LIB_PATH or build with: cmake --build build --parallel"
    )


def resolved_library_path() -> str:
    """Return the resolved filesystem path of the shared libsonare library."""
    path = _find_library()
    candidate = Path(path)
    return str(candidate.resolve()) if candidate.exists() else path


def load_library(lib_path: str | None = None) -> ctypes.CDLL:
    """Load libsonare and configure function signatures.

    Args:
        lib_path: Explicit path to the shared library. If None, searches
            standard locations.

    Returns:
        Loaded ctypes.CDLL with typed function signatures.

    Raises:
        OSError: If the library cannot be found or loaded.
    """
    path = lib_path or _find_library()
    lib = ctypes.CDLL(path)

    # Establish the aggregate ABI contract before configuring the hundreds of
    # individual symbols below. An older dylib may not export newer entry
    # points, and should produce this actionable mismatch rather than an
    # unrelated AttributeError while assigning a later ``.restype``.
    if not hasattr(lib, "sonare_abi_version"):
        raise RuntimeError(
            "libsonare ABI mismatch: native binary does not expose sonare_abi_version"
        )
    lib.sonare_abi_version.restype = ctypes.c_uint32
    lib.sonare_abi_version.argtypes = []
    abi = int(lib.sonare_abi_version())
    if abi != EXPECTED_ABI_VERSION:
        raise RuntimeError(
            f"libsonare ABI mismatch: native binary reports {abi}, "
            f"expected {EXPECTED_ABI_VERSION}. The installed shared library is "
            "incompatible with this Python binding."
        )

    configure_core_signatures(lib)
    configure_effects_engine_signatures(lib)
    configure_repair_dynamics_signatures(lib)
    configure_features_signatures(lib)
    configure_mastering_signatures(lib)
    configure_mixing_signatures(lib)
    configure_extra_signatures(lib)
    configure_project_signatures(lib)

    return lib
