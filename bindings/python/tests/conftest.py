"""Test configuration for libsonare Python binding tests."""

import os
import sys
from pathlib import Path

import pytest


def _find_lib_path() -> str:
    """Find libsonare for testing."""
    env_path = os.environ.get("SONARE_LIB_PATH")
    if env_path and Path(env_path).exists():
        return env_path

    project_root = Path(__file__).parent.parent.parent.parent
    lib_name = "libsonare.dylib" if sys.platform == "darwin" else "libsonare.so"
    build_path = project_root / "build" / "lib" / lib_name
    if build_path.exists():
        return str(build_path)

    pytest.skip(f"libsonare not found at {build_path}")
    return ""  # unreachable


@pytest.fixture()
def lib_path() -> str:
    """Provide the path to the libsonare shared library."""
    return _find_lib_path()
