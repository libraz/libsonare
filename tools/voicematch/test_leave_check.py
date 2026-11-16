"""The leave-check refuses audio, arrays and unlisted cache fields before a sync."""

from __future__ import annotations

import gzip
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import autofit
import leave_check
from test_oracle_cache import _args, _mini


def _cache(tmp_path) -> Path:
    cache_dir = tmp_path / "caches"
    autofit.oracle_reference(_args(_mini(tmp_path), cache_dir))
    return next(cache_dir.glob("*.json.gz"))


def _rewrite(path: Path, edit) -> None:
    body = json.loads(gzip.decompress(path.read_bytes()))
    edit(body)
    path.write_bytes(gzip.compress(json.dumps(body).encode()))


def test_a_clean_tree_and_a_written_cache_pass(tmp_path, capsys):
    tree = tmp_path / "tree"
    (tree / "src").mkdir(parents=True)
    (tree / "src" / "a.py").write_text("x = 1\n")
    cache = _cache(tmp_path)
    assert leave_check.main(["--tree", str(tree), "--cache", str(cache.parent)]) == 0
    assert capsys.readouterr().out.startswith("leave-check PASS: 1 tree(s)")


def test_audio_is_refused_by_extension_and_by_its_bytes_under_any_name(tmp_path, capsys):
    tree = tmp_path / "tree"
    (tree / "deep").mkdir(parents=True)
    (tree / "take.WAV").write_bytes(b"\0" * 16)
    (tree / "deep" / "blob.bin").write_bytes(b"RIFF\0\0\0\0WAVEfmt ")
    (tree / "deep" / "x.dat").write_bytes(b"\x93NUMPY\x01\x00")
    (tree / ".git").mkdir()
    (tree / ".git" / "skip.wav").write_bytes(b"")
    assert leave_check.main(["--tree", str(tree)]) == 1
    out = capsys.readouterr().out
    assert "REFUSED: 3 item(s)" in out
    assert "take.WAV: .WAV container" in out and "blob.bin: WAVE bytes" in out
    assert "x.dat: npy bytes" in out and "skip.wav" not in out


def test_the_repo_s_own_synthetic_fixture_is_not_refused(tmp_path):
    tree = tmp_path / "tree"
    fixture = tree / "tests" / "fixtures" / "sf2" / "minimal_gs.sf2"
    fixture.parent.mkdir(parents=True)
    fixture.write_bytes(b"RIFF\0\0\0\0sfbk")
    assert leave_check.main(["--tree", str(tree)]) == 0


def test_a_cache_with_an_unlisted_field_or_a_long_list_is_refused(tmp_path, capsys):
    cache = _cache(tmp_path)
    _rewrite(cache, lambda b: b.setdefault("extra", 1))
    assert leave_check.main(["--cache", str(cache)]) == 1
    assert "extra" in capsys.readouterr().out
    cache = _cache(tmp_path / "b")
    _rewrite(cache, lambda b: b["oracle"]["rows"][0].update(harm=[0.1] * 4096))
    assert leave_check.main(["--cache", str(cache)]) == 1
    assert "REFUSED" in capsys.readouterr().out


def test_a_cache_s_attack_peaks_pass_to_their_own_bound_and_not_past_it(tmp_path, capsys):
    cache = _cache(tmp_path)
    _rewrite(cache, lambda b: b["oracle"]["rows"][0].update(attack_peaks=[[100.0, 20.0]] * 130))
    assert leave_check.main(["--cache", str(cache)]) == 0
    _rewrite(cache, lambda b: b["oracle"]["rows"][0].update(attack_peaks=[[100.0, 20.0]] * 257))
    assert leave_check.main(["--cache", str(cache)]) == 1
    assert "257 entries, the cap is 256" in capsys.readouterr().out


def test_a_file_that_is_not_a_gzip_cache_is_refused(tmp_path, capsys):
    other = tmp_path / "rows.json"
    other.write_text("{}")
    assert leave_check.main(["--cache", str(other)]) == 1
    assert "not a .json.gz oracle cache" in capsys.readouterr().out


def test_a_parent_of_the_cache_directories_is_searched_through(tmp_path, capsys):
    cache = _cache(tmp_path)
    nested = tmp_path / "out" / "voice" / cache.name
    nested.parent.mkdir(parents=True)
    nested.write_bytes(cache.read_bytes())
    assert leave_check.main(["--cache", str(tmp_path / "out")]) == 0
    assert "1 cache file(s) pass" in capsys.readouterr().out


def test_a_cache_argument_holding_no_cache_is_refused(tmp_path, capsys):
    (tmp_path / "empty").mkdir()
    assert leave_check.main(["--cache", str(tmp_path / "empty")]) == 1
    assert "no cache file to check" in capsys.readouterr().out
