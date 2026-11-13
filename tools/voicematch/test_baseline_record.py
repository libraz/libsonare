"""The baseline publish replaces build-tuning's library by rename and records the HEAD it was built from."""

import json
import os

import baseline_record as br


def _built(root, payload):
    lib = root / "lib"
    lib.mkdir(parents=True)
    (lib / "libsonare.1.10.0.dylib").write_bytes(payload)
    os.symlink("libsonare.1.10.0.dylib", lib / "libsonare.1.dylib")
    os.symlink("libsonare.1.dylib", lib / "libsonare.dylib")
    return root


def test_publish_replaces_the_library_and_keeps_an_open_handle_on_the_old_one(tmp_path):
    dest = _built(tmp_path / "dest", b"old")
    built = _built(tmp_path / "built", b"new")
    with open(dest / "lib" / "libsonare.dylib", "rb") as held:  # a run that already loaded it
        dylib = br.publish(built, dest)
        assert held.read() == b"old"
    assert dylib.read_bytes() == b"new"
    assert (dest / "lib" / "libsonare.dylib").read_bytes() == b"new"
    assert os.readlink(dest / "lib" / "libsonare.dylib") == "libsonare.1.dylib"
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
