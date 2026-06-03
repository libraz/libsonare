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
from ._ffi_types import __all__ as _type_exports

# --- Library discovery ---


def _find_library() -> str:
    """Find the libsonare shared library.

    Search order:
        1. SONARE_LIB_PATH environment variable
        2. Package-adjacent (wheel distribution)
        3. Build directory (development)
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

    configure_core_signatures(lib)
    configure_effects_engine_signatures(lib)
    configure_repair_dynamics_signatures(lib)
    configure_features_signatures(lib)
    configure_mastering_signatures(lib)
    configure_mixing_signatures(lib)
    configure_extra_signatures(lib)
    configure_project_signatures(lib)

    return lib


__all__ = [*_type_exports, "load_library"]
