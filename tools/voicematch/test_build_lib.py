"""The shared-library name and lookup follow the platform the harness runs on."""

import pytest
from _repo import shared_library_file, shared_library_name
from build_lib import library_path


@pytest.mark.parametrize(
    ("platform", "name"),
    [("darwin", "libsonare.dylib"), ("linux", "libsonare.so"), ("linux2", "libsonare.so")],
)
def test_the_library_name_follows_the_platform(monkeypatch, tmp_path, platform, name):
    monkeypatch.setattr("sys.platform", platform)
    assert shared_library_name() == name
    assert shared_library_file(tmp_path) == tmp_path / "lib" / name


def test_an_unknown_platform_is_refused(monkeypatch):
    monkeypatch.setattr("sys.platform", "win32")
    with pytest.raises(RuntimeError, match="win32"):
        shared_library_name()


@pytest.mark.parametrize(
    ("platform", "name", "other"),
    [("darwin", "libsonare.dylib", "libsonare.so"), ("linux", "libsonare.so", "libsonare.dylib")],
)
def test_the_lookup_finds_only_this_platforms_library(monkeypatch, tmp_path, platform, name, other):
    monkeypatch.setattr("sys.platform", platform)
    (tmp_path / "lib").mkdir()
    (tmp_path / "lib" / other).write_bytes(b"x")
    assert library_path(tmp_path) is None
    (tmp_path / "lib" / name).write_bytes(b"x")
    assert library_path(tmp_path) == tmp_path / "lib" / name


def test_the_lookup_falls_back_to_a_search_below_the_build_dir(monkeypatch, tmp_path):
    monkeypatch.setattr("sys.platform", "linux")
    (tmp_path / "out").mkdir()
    (tmp_path / "out" / "libsonare.so").write_bytes(b"x")
    assert library_path(tmp_path) == tmp_path / "out" / "libsonare.so"
