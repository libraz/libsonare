"""Test configuration for libsonare Python binding tests.

This file resolves the shared library's location and refuses to start against a
stale in-tree build. It installs no collection hook and skips nothing: a suite
that needs the C layer declares its own
``pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, ...)`` from ``_helpers``.
That is deliberate -- a conftest-level "skip everything when the
library will not load" cannot tell an absent library from a broken one, so a
genuine build or ABI break would report as a green run full of skips instead of
a failure.
"""

import os
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
    if env_path:
        return env_path

    from libsonare._ffi import in_tree_library

    return in_tree_library(Path(__file__).parent.parent.parent.parent)


_dev_lib_path = _find_dev_lib_path()
if _dev_lib_path is not None:
    os.environ["SONARE_LIB_PATH"] = _dev_lib_path


def pytest_sessionstart(session: pytest.Session) -> None:
    """Refuse to run against an in-tree library older than the core sources."""
    if _dev_lib_path is None:
        return
    # A named library that is missing would make every C-layer module skip,
    # which reads as a green run.
    if not Path(_dev_lib_path).exists():
        pytest.exit(f"SONARE_LIB_PATH={_dev_lib_path!r} does not exist", returncode=3)
    from ._lib_freshness import stale_library_message

    message = stale_library_message(_dev_lib_path, Path(__file__).parent.parent.parent.parent)
    if message is not None:
        pytest.exit(message, returncode=3)


@pytest.fixture()
def lib_path() -> str:
    """Provide the path to the shared library the binding will load."""
    from libsonare._ffi import resolved_library_path

    return resolved_library_path()
