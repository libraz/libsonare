"""The loader resolves the shared library in a fixed order of locations."""

from __future__ import annotations

from pathlib import Path

import pytest

from libsonare import _ffi

LIB_NAME = _ffi._shared_library_name()


@pytest.fixture()
def pkg_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    monkeypatch.delenv("SONARE_LIB_PATH", raising=False)
    package = tmp_path / "bindings" / "python" / "src" / "libsonare"
    package.mkdir(parents=True)
    return package


def _place(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")
    return path


def _root(pkg_dir: Path) -> Path:
    return pkg_dir.parent.parent.parent.parent


def test_python_shared_build_wins_over_the_build_tree(pkg_dir: Path) -> None:
    shared = _place(_root(pkg_dir) / "build-python-shared" / "lib" / LIB_NAME)
    _place(_root(pkg_dir) / "build" / "lib" / LIB_NAME)
    _place(pkg_dir / LIB_NAME)
    assert _ffi._find_library(pkg_dir) == str(shared)


def test_build_tree_is_used_without_a_python_shared_build(pkg_dir: Path) -> None:
    build = _place(_root(pkg_dir) / "build" / "lib" / LIB_NAME)
    _place(pkg_dir / LIB_NAME)
    assert _ffi._find_library(pkg_dir) == str(build)


def test_package_adjacent_library_is_used_outside_a_build_tree(pkg_dir: Path) -> None:
    adjacent = _place(pkg_dir / LIB_NAME)
    assert _ffi._find_library(pkg_dir) == str(adjacent)


def test_environment_variable_wins_over_every_tree(
    pkg_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _place(_root(pkg_dir) / "build-python-shared" / "lib" / LIB_NAME)
    chosen = _place(tmp_path / "elsewhere" / LIB_NAME)
    monkeypatch.setenv("SONARE_LIB_PATH", str(chosen))
    assert _ffi._find_library(pkg_dir) == str(chosen)


def test_in_tree_lookup_matches_the_loader(pkg_dir: Path) -> None:
    # The test session's stale-library guard judges in_tree_library's answer, so
    # it has to be the library the loader resolves.
    _place(_root(pkg_dir) / "build" / "lib" / LIB_NAME)
    _place(_root(pkg_dir) / "build-python-shared" / "lib" / LIB_NAME)
    assert _ffi.in_tree_library(_root(pkg_dir)) == _ffi._find_library(pkg_dir)


def test_a_named_library_that_does_not_exist_is_refused(
    pkg_dir: Path, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    _place(_root(pkg_dir) / "build-python-shared" / "lib" / LIB_NAME)
    missing = tmp_path / "typo" / LIB_NAME
    monkeypatch.setenv("SONARE_LIB_PATH", str(missing))
    with pytest.raises(OSError, match="does not exist") as raised:
        _ffi._find_library(pkg_dir)
    assert str(missing) in str(raised.value)


def test_an_empty_variable_counts_as_unset(pkg_dir: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    shared = _place(_root(pkg_dir) / "build-python-shared" / "lib" / LIB_NAME)
    monkeypatch.setenv("SONARE_LIB_PATH", "")
    assert _ffi._find_library(pkg_dir) == str(shared)
