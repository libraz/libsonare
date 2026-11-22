"""Tests for the objective-equivalence record and its two proofs.

No bank run: the re-extract comparison is exercised on synthetic profiles and the
terms proof on a one-tone, one-hit signal set over two copied trees.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_equivalence.py -q
"""

from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import equivalence
import fits

HERE = Path(__file__).resolve().parent
SLOPE_LINE = '_absent_or(a, b, 30.0, cells["slope"], 0.1) / 10.0'


def _passed(**over):
    result = {
        "a_commit": "a" * 40,
        "b_commit": "b" * 40,
        "a_digest": "d1",
        "b_digest": "d2",
        "reextract": {"passed": True, "captures": 1},
        "terms": {"passed": True, "cases": 1},
    }
    result.update(over)
    return result


def _tree(root: Path) -> Path:
    skip = shutil.ignore_patterns(
        "__pycache__", "assets", "out", "reference", "capture", "excerpts", "docs", "t"
    )
    shutil.copytree(HERE, root / "tools" / "voicematch", ignore=skip)
    shutil.copy(HERE.parent / "_repo.py", root / "tools" / "_repo.py")
    return root


def _edit_loss(tree: Path, old: str, new: str) -> None:
    path = tree / "tools" / "voicematch" / "loss.py"
    text = path.read_text()
    assert old in text
    path.write_text(text.replace(old, new, 1))


# (a) record


@pytest.mark.parametrize("failed", ["reextract", "terms"])
def test_record_refuses_a_failed_proof(tmp_path, failed):
    result = _passed(**{failed: {"passed": False}})
    path = tmp_path / "eq.json"
    with pytest.raises(ValueError, match=failed):
        equivalence.record(result, path)
    assert not path.exists()


def test_record_refuses_equal_digests(tmp_path):
    with pytest.raises(ValueError, match="nothing to record"):
        equivalence.record(_passed(b_digest="d1"), tmp_path / "eq.json")


def test_record_appends_a_passing_pair(tmp_path):
    path = tmp_path / "eq.json"
    entry = equivalence.record(_passed(), path)
    assert (entry["a"], entry["b"]) == ("d1", "d2")
    assert json.loads(path.read_text())["equivalences"] == [entry]


# (b) terms proof


def test_terms_proof_sees_a_value_changing_edit(tmp_path):
    a = _tree(tmp_path / "a")
    b = _tree(tmp_path / "b")
    _edit_loss(b, SLOPE_LINE, SLOPE_LINE.replace("/ 10.0", "/ 11.0"))
    out = equivalence.prove_terms(a, b, tmp_path / "scratch", "small")
    assert not out["passed"]
    assert any(".slope" in d for d in out["differences"])


def test_terms_proof_passes_a_comment_only_edit(tmp_path):
    a = _tree(tmp_path / "a")
    b = _tree(tmp_path / "b")
    _edit_loss(b, "def loss_terms(", "# a comment that changes no value\ndef loss_terms(")
    out = equivalence.prove_terms(a, b, tmp_path / "scratch", "small")
    assert out["passed"], out["differences"]
    assert out["a_digest"] == out["b_digest"]
    assert out["cases"] == 2


# (c) re-extract comparison


def test_an_added_field_is_an_addition_not_a_difference():
    a = {"measured_utc": "x", "rows": [{"note": 60, "v": 1.0}]}
    b = {"measured_utc": "y", "rows": [{"note": 60, "v": 1.0, "new": 2.0}], "extra": 1}
    got = equivalence.compare_profiles(a, b)
    assert got["differences"] == []
    assert got["additions"] == [".extra", ".rows[timbre=None note=60 velocity=None].new"]


def test_a_changed_field_is_a_difference_and_additions_join_it():
    a = {"rows": [{"note": 60, "v": 1.0}]}
    b = {"rows": [{"note": 60, "v": 1.5, "new": 2.0}]}
    got = equivalence.compare_profiles(a, b)
    assert len(got["differences"]) == 2 and got["additions"] == []
    assert any("1.0 -> 1.5" in d for d in got["differences"])


def test_a_removed_field_and_a_type_change_are_differences():
    assert equivalence.compare_profiles({"a": 1, "b": 2}, {"a": 1})["differences"]
    assert equivalence.compare_profiles({"a": 1}, {"a": 1.0})["differences"]


def test_nan_matches_nan_and_stamps_are_ignored():
    a = {"v": float("nan"), "field_measured_utc": {"v": "1"}}
    b = {"v": float("nan"), "field_measured_utc": {"v": "2"}}
    got = equivalence.compare_profiles(a, b)
    assert got == {"differences": [], "additions": []}


# (d) closure


def test_equivalent_digests_is_transitive_and_symmetric(tmp_path):
    path = tmp_path / "eq.json"
    equivalence.record(_passed(a_digest="d1", b_digest="d2"), path)
    equivalence.record(_passed(a_digest="d3", b_digest="d2"), path)
    equivalence.record(_passed(a_digest="x1", b_digest="x2"), path)
    assert equivalence.equivalent_digests("d1", path) == {"d2", "d3"}
    assert equivalence.equivalent_digests("d3", path) == {"d1", "d2"}
    assert equivalence.equivalent_digests("zz", path) == set()


# (e) fits.axis


def _fit(digest):
    return {"objective": digest, "date": "2026-11-01", "outcome": "adopted"}


def test_axis_reports_equivalent_for_a_fit_under_an_equivalent_digest():
    records = {"piano": _fit("old"), "organ": _fit("cur")}
    eq = {"old"}
    got = fits.axis(["piano"], records, "cur", eq)
    assert got["state"] == fits.EQUIVALENT and got["fitted_under"] == ["old"]
    assert fits.axis(["organ"], records, "cur", eq)["state"] == fits.CURRENT
    assert fits.axis(["piano", "organ"], records, "cur", eq)["state"] == fits.EQUIVALENT
    assert fits.axis(["piano"], records, "cur", set())["state"] == fits.STALE
    assert fits.axis(["piano", "gone"], records, "cur", eq)["state"] == fits.NONE


def test_axis_reads_the_recorded_equivalences_by_default(tmp_path, monkeypatch):
    path = tmp_path / "eq.json"
    equivalence.record(_passed(a_digest="old", b_digest="cur"), path)
    real = equivalence.equivalent_digests
    monkeypatch.setattr(equivalence, "equivalent_digests", lambda d: real(d, path))
    assert fits.axis(["piano"], {"piano": _fit("old")}, "cur")["state"] == fits.EQUIVALENT


def test_an_unweighted_key_the_later_tree_adds_is_an_addition():
    a = {"tone": {"harm": 1.0, "tnr": 2.0}}
    b = {"tone": {"harm": 1.0, "tnr": 2.0, "harm_uncapped": 1.5}}
    got = equivalence.compare_terms(a, b)
    assert got["passed"] and got["additions"] == ["tone.harm_uncapped"]
    # A weighted key appearing changes the loss, so it is a difference.
    assert not equivalence.compare_terms(a, {"tone": {**b["tone"], "bright": 3.0}})["passed"]
    # An addition beside a moved value is not excused.
    moved = {"tone": {"harm": 1.1, "tnr": 2.0, "harm_uncapped": 1.5}}
    assert not equivalence.compare_terms(a, moved)["passed"]
    # A key the later tree drops is a difference.
    assert not equivalence.compare_terms(b, a)["passed"]


def test_a_fit_weighing_an_added_key_is_not_covered(monkeypatch):
    """The addition allowance holds only for fits that put no weight on the added key."""
    import fits

    pairs = [{"a": "old", "b": "new", "added_keys": ["harm_uncapped"]}]
    monkeypatch.setattr(equivalence, "load", lambda path=None: pairs)
    assert equivalence.equivalent_digests("new") == {"old"}
    assert equivalence.equivalent_digests("new", weights={"harm": 1.0}) == {"old"}
    assert equivalence.equivalent_digests("new", weights={"harm_uncapped": 1.0}) == set()
    plain = {"objective": "old", "date": "2026-11-01", "outcome": "kept"}
    weighing = {**plain, "weights": {"harm_uncapped": 0.5}}
    assert fits.axis(["u"], {"u": plain}, "new")["state"] == fits.EQUIVALENT
    assert fits.axis(["u"], {"u": weighing}, "new")["state"] == fits.STALE


def test_a_recorded_fit_keeps_the_weights_it_ran_under():
    import fits

    report = {
        "objective": "d",
        "knobs": [{"key": "piano.x"}],
        "fit_profile": {"weights": {"harm": 1.0, "lf": 0.0}},
    }
    _unit, entry = fits.record_fit(report, capture="c", timbre="t", outcome="kept", today="x")
    # An explicit zero is recorded: equivalence reads only weights above zero, so it covers the same.
    assert entry["weights"] == {"harm": 1.0, "lf": 0.0}


def test_the_recorded_digest_is_the_one_the_process_loaded(tmp_path):
    """A loss file edited on disk mid-run does not move the digest a run records."""
    import shutil

    import objective

    tree = tmp_path / "tree"
    shutil.copytree(
        Path(objective.__file__).resolve().parent,
        tree,
        ignore=shutil.ignore_patterns("reference", "capture", "__pycache__", "*.json"),
    )
    first = objective.loaded_digest(tree)
    loss = tree / "loss.py"
    loss.write_text(loss.read_text() + "\nEDITED_MID_RUN = 1\n")
    assert objective.objective_digest(tree) != first
    assert objective.loaded_digest(tree) == first
