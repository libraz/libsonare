"""Test configuration for libsonare Python binding tests."""

import os
import sys
from pathlib import Path

import pytest


def _find_dev_lib_path() -> str | None:
    """Return a development build of libsonare, or None if the checkout has none.

    None is not a failure: an installed wheel carries its own repaired library
    next to the package and the binding's own loader finds it. Pinning
    SONARE_LIB_PATH at a build tree whenever one happens to exist would make the
    wheel job test something other than the artifact it just built.
    """
    env_path = os.environ.get("SONARE_LIB_PATH")
    if env_path and Path(env_path).exists():
        return env_path

    project_root = Path(__file__).parent.parent.parent.parent
    lib_name = "libsonare.dylib" if sys.platform == "darwin" else "libsonare.so"
    for build_dir in ("build", "build-mastering-api", "build-mastering"):
        build_path = project_root / build_dir / "lib" / lib_name
        if build_path.exists():
            return str(build_path)

    return None


_dev_lib_path = _find_dev_lib_path()
if _dev_lib_path is not None:
    os.environ.setdefault("SONARE_LIB_PATH", _dev_lib_path)


@pytest.fixture()
def lib_path() -> str:
    """Provide the path to the shared library the binding will load."""
    from libsonare._ffi import resolved_library_path

    return resolved_library_path()
