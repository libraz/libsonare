"""The baseline publish replaces build-tuning's library by rename and records the HEAD it was built from."""

import json
import os

import baseline_record as br
import pytest
from _repo import shared_library_name


def _chain(name):
    """The platform's (full, soname, link) names for a 1.10.0 library with SOVERSION 1."""
    stem, ext = name.rsplit(".", 1)
    if ext == "dylib":
        return f"{stem}.1.10.0.dylib", f"{stem}.1.dylib", name
    return f"{name}.1.10.0", f"{name}.1", name


def _built(root, payload):
    lib = root / "lib"
    lib.mkdir(parents=True)
    full, soname, link = _chain(shared_library_name())
    (lib / full).write_bytes(payload)
    os.symlink(full, lib / soname)
    os.symlink(soname, lib / link)
    return root


def test_publish_replaces_the_library_and_keeps_an_open_handle_on_the_old_one(tmp_path):
    dest = _built(tmp_path / "dest", b"old")
    built = _built(tmp_path / "built", b"new")
    with open(dest / "lib" / shared_library_name(), "rb") as held:  # a run that already loaded it
        dylib = br.publish(built, dest)
        assert held.read() == b"old"
    assert dylib.read_bytes() == b"new"
    assert (dest / "lib" / shared_library_name()).read_bytes() == b"new"
    assert os.readlink(dest / "lib" / shared_library_name()) == _chain(shared_library_name())[1]
    assert not list((dest / "lib").glob(".*.tmp"))


def test_main_records_the_mirror_head(tmp_path, monkeypatch):
    built = _built(tmp_path / "built", b"lib")
    mirror = tmp_path / "mirror"
    mirror.mkdir()
    (mirror / ".HEAD").write_text("abc1234\n")
    monkeypatch.setattr(br, "REPO", tmp_path)
    assert br.main([str(built), str(mirror), "build-tuning"]) == 0
    rec = json.loads((tmp_path / "build-tuning" / "BASELINE").read_text())
    assert rec["head"] == "abc1234"
    assert rec["source"] == "head mirror"
    assert br.read(tmp_path / "build-tuning") == rec


@pytest.mark.parametrize(
    ("platform", "name"), [("darwin", "libsonare.dylib"), ("linux", "libsonare.so")]
)
def test_publish_carries_the_platforms_version_chain(tmp_path, monkeypatch, platform, name):
    monkeypatch.setattr("sys.platform", platform)
    built = _built(tmp_path / "built", b"lib")
    dest = tmp_path / "dest"
    br.publish(built, dest)
    full, soname, link = _chain(name)
    assert (dest / "lib" / full).read_bytes() == b"lib"
    assert os.readlink(dest / "lib" / soname) == full
    assert os.readlink(dest / "lib" / link) == soname
