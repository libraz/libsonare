"""Tests for the re-extraction control — the diff, not the render.

None of this touches the cached corpus under `.cache/voicematch/`, which is
untracked and absent on a fresh clone: `reextract_one` is exercised only
against monkeypatched capture/reference directories that declare no cache, so
what these cases cover is the comparison logic `reextract_check.py` adds on
top of `profile.py measure` -- that `measured_utc` is the one field allowed to
differ, that a row-level mismatch is reported with its timbre/note/velocity
rather than a bare index, and that a missing reference or cache is a distinct
status rather than a silent skip.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_reextract_check.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import reextract_check


def test_shipped_ids_matches_committed_captures():
    ids = reextract_check.shipped_ids()
    assert ids
    assert ids == sorted(set(ids))
    assert all(not i.endswith(".local") for i in ids)


def test_compare_ignores_measured_utc_alone():
    old = {"id": "x", "measured_utc": "2020-01-01T00:00:00Z", "rows": []}
    new = {"id": "x", "measured_utc": "2026-09-20T00:00:00Z", "rows": []}
    assert reextract_check.compare(old, new) == []


def test_compare_reports_a_changed_scalar_field():
    old = {"summary": {"decay_db_s": -4.0}}
    new = {"summary": {"decay_db_s": -4.5}}
    diffs = reextract_check.compare(old, new)
    assert len(diffs) == 1
    assert ".summary.decay_db_s" in diffs[0]
    assert "-4.0" in diffs[0] and "-4.5" in diffs[0]


def test_compare_names_the_row_a_mismatch_lives_in():
    old = {"rows": [{"timbre": "t1", "note": 60, "velocity": 80, "decay_db_s": -4.0}]}
    new = {"rows": [{"timbre": "t1", "note": 60, "velocity": 80, "decay_db_s": -9.9}]}
    diffs = reextract_check.compare(old, new)
    assert len(diffs) == 1
    assert "timbre='t1'" in diffs[0]
    assert "note=60" in diffs[0]
    assert "velocity=80" in diffs[0]


def test_compare_caps_the_diff_count_rather_than_flooding():
    old = {"rows": [{"timbre": "t", "note": n, "velocity": 80, "v": 0.0} for n in range(60, 90)]}
    new = {"rows": [{"timbre": "t", "note": n, "velocity": 80, "v": 1.0} for n in range(60, 90)]}
    diffs = reextract_check.compare(old, new)
    assert len(diffs) == reextract_check.MAX_DIFFS


def test_compare_treats_two_nans_as_the_same_unmeasurable_answer():
    """`nan != nan`, so a plain `!=` would flag every unscorable partial as drift."""
    old = {
        "rows": [{"timbre": "t", "note": 60, "velocity": 80, "partial_decay_db_s": [float("nan")]}]
    }
    new = {
        "rows": [{"timbre": "t", "note": 60, "velocity": 80, "partial_decay_db_s": [float("nan")]}]
    }
    assert reextract_check.compare(old, new) == []


def test_compare_still_catches_nan_becoming_a_number():
    old = {"rows": [{"timbre": "t", "note": 60, "velocity": 80, "v": float("nan")}]}
    new = {"rows": [{"timbre": "t", "note": 60, "velocity": 80, "v": 1.0}]}
    diffs = reextract_check.compare(old, new)
    assert len(diffs) == 1 and "nan" in diffs[0] and "1.0" in diffs[0]


def test_compare_reads_zero_for_a_profile_against_itself():
    profile = {
        "id": "x",
        "measured_utc": "now",
        "capture": {"a": 1},
        "rows": [{"timbre": "t", "note": 60, "velocity": 80, "v": 1.0}],
        "summary": {"s": 1.0},
    }
    assert reextract_check.compare(profile, json.loads(json.dumps(profile))) == []


def test_reextract_one_reports_no_reference(tmp_path, monkeypatch):
    reference_dir = tmp_path / "reference"
    reference_dir.mkdir()
    monkeypatch.setattr(reextract_check, "REFERENCE_DIR", reference_dir)
    result = reextract_check.reextract_one("does-not-exist", tmp_path / "scratch")
    assert result["status"] == "no-reference"
    assert result["diffs"] == []


def test_reextract_one_reports_no_cache(tmp_path, monkeypatch):
    """A capture with a reference but no cached corpus -- never asked to measure."""
    capture_dir, reference_dir = tmp_path / "capture", tmp_path / "reference"
    capture_dir.mkdir()
    reference_dir.mkdir()
    (capture_dir / "fake.json").write_text(json.dumps({"id": "fake", "timbres": []}))
    (reference_dir / "fake.json").write_text(json.dumps({"id": "fake", "rows": []}))
    monkeypatch.setattr(reextract_check, "CAPTURE_DIR", capture_dir)
    monkeypatch.setattr(reextract_check, "REFERENCE_DIR", reference_dir)

    result = reextract_check.reextract_one("fake", tmp_path / "scratch")
    assert result["status"] == "no-cache"
    assert result["diffs"] == []


def test_main_exits_zero_on_a_capture_with_no_cache(tmp_path, monkeypatch):
    """Read-only and never a gate, on the same terms as `substitution.py`.

    Named explicitly rather than left to fall back to `shipped_ids()`: on this
    machine the corpus cache exists and an empty `--ids` would re-run the whole
    129-capture check inside a unit test. A fresh clone has no cache either way,
    so `main` over one uncached fake id exercises the same "no-cache" path this
    module falls back on everywhere else, in constant time.
    """
    capture_dir, reference_dir = tmp_path / "capture", tmp_path / "reference"
    capture_dir.mkdir()
    reference_dir.mkdir()
    (capture_dir / "fake.json").write_text(json.dumps({"id": "fake", "timbres": []}))
    (reference_dir / "fake.json").write_text(json.dumps({"id": "fake", "rows": []}))
    monkeypatch.setattr(reextract_check, "CAPTURE_DIR", capture_dir)
    monkeypatch.setattr(reextract_check, "REFERENCE_DIR", reference_dir)

    assert reextract_check.main(["--ids", "fake", "--out", str(tmp_path / "scratch")]) == 0
