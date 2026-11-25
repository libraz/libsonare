"""Compare the shipped stubs with the Python runtime."""

import subprocess
import sys
from pathlib import Path

import pytest

from .conftest import _find_dev_lib_path


def test_shipped_stubs() -> None:
    library = _find_dev_lib_path()
    if library is None:
        pytest.skip("stubtest skipped: no in-tree libsonare shared library; run make build-shared")

    from libsonare._ffi import load_library

    # Importing libsonare is lazy. Check the same library the binding will use.
    load_library(library)
    subprocess.run(
        [
            sys.executable,
            "-m",
            "mypy.stubtest",
            "--mypy-config-file",
            "tests/stubtest.ini",
            "--allowlist",
            "tests/stubtest-allowlist.txt",
            "libsonare",
        ],
        cwd=Path(__file__).resolve().parents[1],
        check=True,
    )
