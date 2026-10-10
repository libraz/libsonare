"""The stale-build guard flags only an in-tree library older than a core source."""

from __future__ import annotations

import os
from pathlib import Path

import pytest
from _lib_freshness import ALLOW_STALE_ENV, stale_library_message


def _touch(path: Path, mtime: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("")
    os.utime(path, (mtime, mtime))


@pytest.fixture()
def tree(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.delenv(ALLOW_STALE_ENV, raising=False)
    _touch(tmp_path / "src" / "core.cpp", 1000.0)
    _touch(tmp_path / "include" / "sonare" / "api.h", 900.0)
    return tmp_path


def test_library_newer_than_sources_passes(tree: Path) -> None:
    lib = tree / "build" / "lib" / "libsonare.so"
    _touch(lib, 2000.0)
    assert stale_library_message(str(lib), tree) is None


def test_library_older_than_a_source_is_named(tree: Path) -> None:
    lib = tree / "build" / "lib" / "libsonare.so"
    _touch(lib, 500.0)
    message = stale_library_message(str(lib), tree)
    assert message is not None
    assert "core.cpp" in message
    assert "SONARE_LIB_PATH" in message


def test_header_newer_than_library_is_detected(tree: Path) -> None:
    lib = tree / "build" / "lib" / "libsonare.so"
    _touch(lib, 950.0)
    _touch(tree / "include" / "sonare" / "api.h", 1100.0)
    message = stale_library_message(str(lib), tree)
    assert message is not None
    assert "api.h" in message


def test_wasm_glue_is_ignored(tree: Path) -> None:
    lib = tree / "build" / "lib" / "libsonare.so"
    _touch(lib, 1500.0)
    _touch(tree / "src" / "wasm" / "bindings.cpp", 3000.0)
    assert stale_library_message(str(lib), tree) is None


def test_library_outside_the_tree_is_not_judged(
    tree: Path, tmp_path_factory: pytest.TempPathFactory
) -> None:
    lib = tmp_path_factory.mktemp("elsewhere") / "libsonare.so"
    _touch(lib, 1.0)
    assert stale_library_message(str(lib), tree) is None


def test_opt_out_env_disables_the_check(tree: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    lib = tree / "build" / "lib" / "libsonare.so"
    _touch(lib, 500.0)
    monkeypatch.setenv(ALLOW_STALE_ENV, "1")
    assert stale_library_message(str(lib), tree) is None
