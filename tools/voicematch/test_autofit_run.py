"""What a fit actually ran against: that a probe reached the code, the tree
state it compiled, and the measurements it kept across runs.

    uv run --locked --project bindings/python \\
        python -m pytest tools/voicematch/test_autofit_run.py -q
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import json
from dataclasses import replace

import autofit
import build_lib
import eval_cache
import render_rows
from _repo import shared_library_name
from autofit import Evaluator, winner_or_defaults
from autofit_test_fixtures import (
    _fit_args,
    _source_knob,
    _terms,
    _write_corpus,
)
from corpus import load_corpus
from eval_cache import open_cache
from knobs import Knob
from loss import LOSS_TERMS
from staging import _better_seed, screen_knobs


# --------------------------------------------------------------------------- #
# Proving the probe reached the code
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize(
    ("term", "velocities", "message"),
    [
        ("harm", "64,100,127", "no active loss terms"),
        (None, "64,100,127", "no active loss terms"),
        ("dyn", "100", "single velocity"),
    ],
)
def test_fit_refuses_unusable_objectives_before_building(tmp_path, term, velocities, message):
    weights = dict.fromkeys(LOSS_TERMS, 0.0)
    if term is not None:
        weights[term] = 1.0
    spec = tmp_path / "unsupported-objective.json"
    spec.write_text(
        json.dumps(
            {
                "weights": weights,
                "knobs": [{"tunable": "d045.percussion.mallet_vel_exp", "min": 0.0, "max": 1.0}],
            }
        )
    )
    build_path = tmp_path / "must-not-build"
    build_path.write_text("sentinel")
    result = subprocess.run(
        [
            sys.executable,
            str(Path(autofit.__file__)),
            "--spec",
            str(spec),
            "--program",
            "0",
            "--drum-note",
            "45",
            "--velocities",
            velocities,
            "--build-dir",
            str(build_path),
            "--dry-run",
            # The refusal under test comes after the tree check; a concurrent
            # in-flight edit under src/ must not answer first.
            "--allow-dirty-src",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode != 0
    assert message in result.stderr
    assert build_path.read_text() == "sentinel"


class _FakeEvaluator:
    """Scores a knob vector from a formula, so a probe's reach is decidable."""

    def __init__(self, response):
        self.response = response
        self.quiet = False

    def __call__(self, values):
        return self.response(values)

    def evaluate_batch(self, points):
        return [self.response(p) for p in points]


def _screen_args(threshold=0.001, max_evals=1000):
    return argparse.Namespace(screen_threshold=threshold, max_evals=max_evals)


def _reach_knob(label, lo, hi, start):
    return Knob(label=label, lo=lo, hi=hi, log=False, start_value=start, tunable=f"file.{label}")


def test_a_screen_that_moves_nothing_is_an_error_rather_than_a_full_knob_list():
    """Zero is the signature of a probe that never reached the fields it swept.

    The fallback used to restore the whole list, so a spec that moved NOTHING
    and a spec that moved everything continued into the fit with the same knob
    count and the same one-line message.
    """
    knobs = [_reach_knob("a", 0.0, 1.0, 0.5), _reach_knob("b", 0.0, 1.0, 0.5)]
    with pytest.raises(RuntimeError, match="0 of 2 knobs move the loss at all"):
        screen_knobs(_FakeEvaluator(lambda v: 1.0), knobs, _screen_args())


def test_a_stage_that_walks_somewhere_worse_hands_the_defaults_to_the_final_stage():
    """An early stage optimises under weights the answer is not judged by.

    Nothing in the staged fit compared the point it hands over against the
    defaults under the CLI weights, so a decay stage scored on `bdecay`/`env`
    alone could walk the voice 2.2x worse overall and the final stage would
    spend its whole budget climbing back. Measured on drum note 45, which ended
    at 1.15 and wrote that regression to source.
    """
    knobs = [_reach_knob("a", 0.0, 1.0, 0.5), _reach_knob("b", 0.0, 1.0, 0.5)]
    seen = []

    def response(values):
        seen.append(list(values))
        return 2.2 if values == [1.0, 1.0] else 1.0

    assert _better_seed(_FakeEvaluator(response), knobs, [1.0, 1.0]) == [0.5, 0.5]
    # Both points scored, so the defaults are in the evaluator's best either way
    # and act as a floor under whatever the final stage does next.
    assert seen == [[1.0, 1.0], [0.5, 0.5]]


def test_a_stage_that_walks_somewhere_better_keeps_it():
    knobs = [_reach_knob("a", 0.0, 1.0, 0.5), _reach_knob("b", 0.0, 1.0, 0.5)]

    def response(values):
        return 0.4 if values == [1.0, 1.0] else 1.0

    assert _better_seed(_FakeEvaluator(response), knobs, [1.0, 1.0]) == [1.0, 1.0]


def test_a_fit_that_lost_to_its_own_start_point_writes_nothing():
    """The defaults score 1.0 by construction, so above it is a lost search."""
    knobs = [_reach_knob("a", 0.0, 1.0, 0.25)]
    lost = argparse.Namespace(normalize=True, best_loss=1.1536)
    assert winner_or_defaults(knobs, [0.9], lost) == [0.25]
    won = argparse.Namespace(normalize=True, best_loss=0.5071)
    assert winner_or_defaults(knobs, [0.9], won) == [0.9]
    # --raw-loss has no reference point, so there is nothing to compare against.
    raw = argparse.Namespace(normalize=False, best_loss=1.1536)
    assert winner_or_defaults(knobs, [0.9], raw) == [0.9]


def test_a_winner_that_loses_off_the_probe_writes_nothing_either():
    """The hold-out is the same failure measured where the fit could not see.

    Refitting the kick from its own fitted values reached 0.8692 on the three
    probe velocities and 1.1924 on the three held out — values traded for the
    probe, which write-back took without asking. A wash is not a loss: an
    unchanged result improved the measured objective and is worse nowhere.
    """
    knobs = [_reach_knob("a", 0.0, 1.0, 0.25)]
    won = argparse.Namespace(normalize=True, best_loss=0.8692)
    overfit = {"axis": "velocities", "held_out": "48,88,112", "start": 1.0, "best": 1.1924}
    assert winner_or_defaults(knobs, [0.9], won, overfit) == [0.25]
    generalises = {**overfit, "best": 0.4803}
    assert winner_or_defaults(knobs, [0.9], won, generalises) == [0.9]
    wash = {**overfit, "best": 1.0043}
    assert winner_or_defaults(knobs, [0.9], won, wash) == [0.9]
    # No hold-out asked for, no verdict to act on.
    assert winner_or_defaults(knobs, [0.9], won, None) == [0.9]


def test_a_validation_fallback_pairs_selected_values_with_selected_loss():
    """A held-out refusal must not report the search loss for the defaults."""

    knobs = [_reach_knob("a", 0.0, 1.0, 0.25)]
    terms = {("0.25",): {"harm": 1.0}, ("0.9",): {"harm": 0.2}}
    losses = {("0.25",): 1.0, ("0.9",): 0.5}
    evaluator = argparse.Namespace(
        normalize=True,
        best_loss=0.5,
        terms_for=lambda values: terms.get(tuple(str(value) for value in values)),
        loss_for=lambda values: losses.get(tuple(str(value) for value in values)),
    )
    selected = winner_or_defaults(
        knobs,
        [0.9],
        evaluator,
        {"axis": "notes", "held_out": [72], "start": 1.0, "best": 1.2},
    )
    assert selected == [0.25]
    assert evaluator.selected_values == [0.25]
    assert evaluator.selected_loss == pytest.approx(1.0)


def test_a_screen_that_moves_something_still_narrows_to_it():
    """The inert-knob finding is unchanged; only the all-inert case is new."""
    knobs = [_reach_knob("a", 0.0, 1.0, 0.5), _reach_knob("b", 0.0, 1.0, 0.5)]
    kept = screen_knobs(_FakeEvaluator(lambda v: v[0]), knobs, _screen_args())
    assert kept == [0]


def test_the_fit_refuses_a_library_that_ignores_the_override_table(monkeypatch):
    """Without BUILD_TUNING every candidate renders the compiled-in defaults.

    The fit then searches nothing and reports the start point as its winner,
    which nothing else about the run would show.
    """
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    monkeypatch.setattr(
        ev, "_render_terms", lambda values, **_: {t: 1.0 for t in LOSS_TERMS}, raising=False
    )
    with pytest.raises(RuntimeError, match="not reaching the library"):
        ev.check_overrides_reach([_reach_knob("a", 0.0, 1.0, 0.5)])


def test_the_reach_check_passes_when_the_override_moves_the_render(monkeypatch):
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    seen = []

    def render(values):
        seen.append(list(values))
        return {t: float(values[0]) for t in LOSS_TERMS}

    monkeypatch.setattr(ev, "_render_terms", render, raising=False)
    ev.check_overrides_reach([_reach_knob("a", 0.0, 1.0, 0.25)])
    # Base at the start values, then the far end of the range — the end further
    # from the start, so the probe has the whole interval behind it.
    assert seen == [[0.25], [1.0]]


def test_a_knob_that_silences_the_voice_does_not_read_as_broken_plumbing(monkeypatch):
    """Several knobs' far ends are longer than a plucked instrument's whole gate.

    `amp_env.delay_ms` reaches 5 s and the attacks 20 s, so pushing every knob at
    once renders digital silence on any short probe and the check has nothing to
    compare. Silence says nothing about whether the overrides arrive, so the
    check falls back to one knob at a time and passes on the first that moves a
    measurement.
    """
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    seen = []

    def render(values):
        seen.append(list(values))
        if values[1] == 1.0:  # the silencing knob, at its far end
            return None
        return {t: float(values[0]) for t in LOSS_TERMS}

    monkeypatch.setattr(ev, "_render_terms", render, raising=False)
    ev.check_overrides_reach(
        [_reach_knob("a", 0.0, 1.0, 0.25), _reach_knob("silencer", 0.0, 1.0, 0.25)]
    )
    # Start, the unscorable all-at-once render, then the first knob on its own.
    assert seen == [[0.25, 0.25], [1.0, 1.0], [1.0, 0.25]]


def test_a_probe_that_measures_nothing_as_shipped_says_so_rather_than_blaming_overrides(
    monkeypatch,
):
    """No baseline means no fit, and the reason is the probe rather than the
    environment — a message about BUILD_TUNING would send the reader nowhere."""
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    monkeypatch.setattr(ev, "_render_terms", lambda values, **_: None, raising=False)
    with pytest.raises(RuntimeError, match="own start values"):
        ev.check_overrides_reach([_reach_knob("a", 0.0, 1.0, 0.25)])


def test_the_one_at_a_time_fallback_still_catches_a_library_that_ignores_overrides(monkeypatch):
    """The fallback must not turn the plumbing failure into a pass: if no single
    knob moves anything either, the run is still searching nothing."""
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")

    def render(values):
        return None if values == [1.0, 1.0] else {t: 1.0 for t in LOSS_TERMS}

    monkeypatch.setattr(ev, "_render_terms", render, raising=False)
    with pytest.raises(RuntimeError, match="one range at a time"):
        ev.check_overrides_reach(
            [_reach_knob("a", 0.0, 1.0, 0.25), _reach_knob("b", 0.0, 1.0, 0.25)]
        )


def test_a_source_only_spec_has_no_override_plumbing_to_check(monkeypatch):
    """A rebuilding knob does not travel through the environment at all."""
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    monkeypatch.setattr(
        ev,
        "_render_terms",
        lambda values, **_: pytest.fail("a source knob needs no reach check"),
        raising=False,
    )
    ev.check_overrides_reach(
        [
            Knob(
                label="a",
                lo=0.0,
                hi=1.0,
                log=False,
                start_value=0.5,
                file=Path("x.cpp"),
                pattern="p",
            )
        ]
    )


# --------------------------------------------------------------------------- #
# Measurements kept across runs
# --------------------------------------------------------------------------- #
def _refuse_to_render(values):
    raise AssertionError(f"rendered {values}, which the store already held")


def _stub_build(tmp_path: Path, contents: bytes = b"lib") -> Path:
    build = tmp_path / "build"
    (build / "lib").mkdir(parents=True, exist_ok=True)
    (build / "lib" / shared_library_name()).write_bytes(contents)
    return build


def _cached_evaluator(tmp_path, monkeypatch, *, corpus=None, **kwargs):
    """An `Evaluator` whose store is under `tmp_path` and whose build is a stub."""
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    knob = Knob(label="x.k", lo=0.0, hi=1.0, log=False, start_value=0.5, tunable="x.k")
    args = _fit_args(no_cache=False, **kwargs)
    return Evaluator([knob], {}, [{"note": 60}], None, args, _stub_build(tmp_path), corpus=corpus)


def test_the_store_hands_back_what_it_was_given(tmp_path):
    store = open_cache(tmp_path, "sig")
    store.put(("1.0",), {"harm": 2.0})
    store.put(("2.0",), None)  # a render that produced nothing scorable
    reopened = open_cache(tmp_path, "sig")
    assert reopened.entries == {("1.0",): {"harm": 2.0}, ("2.0",): None}
    assert reopened.loaded == 2


def test_a_later_line_wins_over_an_earlier_one_for_the_same_candidate(tmp_path):
    """The file is appended to and never rewritten, so a re-measure is a new line."""
    store = open_cache(tmp_path, "sig")
    store.put(("1.0",), {"harm": 2.0})
    store.put(("1.0",), {"harm": 9.0})
    assert open_cache(tmp_path, "sig").entries == {("1.0",): {"harm": 9.0}}


def test_a_half_written_line_does_not_cost_the_rest_of_the_store(tmp_path):
    """A run killed mid-append is the expected damage, not a reason to start cold."""
    store = open_cache(tmp_path, "sig")
    store.put(("1.0",), {"harm": 2.0})
    store.put(("2.0",), {"harm": 3.0})
    text = store.path.read_text()
    store.path.write_text(text[: len(text) - 12])
    assert open_cache(tmp_path, "sig").entries == {("1.0",): {"harm": 2.0}}


def test_an_off_store_writes_nothing_anywhere(tmp_path):
    store = open_cache(tmp_path, "sig", enabled=False)
    store.put(("1.0",), {"harm": 2.0})
    assert list(tmp_path.iterdir()) == []


@pytest.mark.parametrize(
    "change",
    ["library", "harness", "oracle", "probe", "knobs", "corpus", "loss_tail_fraction"],
)
def test_the_signature_moves_with_everything_a_stored_value_depends_on(
    tmp_path,
    monkeypatch,
    change,
):
    """A stored term is a number some particular code produced from some
    particular inputs. Anything the key leaves out is something the store will
    happily answer with after it has changed."""
    ev = _cached_evaluator(
        tmp_path,
        monkeypatch,
        corpus=load_corpus(_write_corpus(tmp_path / "corpus", notes=(60, 72))),
    )
    before = ev.cache_signature()
    if change == "library":
        (ev.build_dir / "lib" / shared_library_name()).write_bytes(b"rebuilt")
    elif change == "harness":
        monkeypatch.setattr(autofit, "source_digest", lambda d: "edited")
    elif change == "oracle":
        ev.oracle = [{"note": 61}]
    elif change == "probe":
        ev.args.notes = "48,60"
    elif change == "loss_tail_fraction":
        ev.args.loss_tail_fraction = 0.0
    elif change == "knobs":
        ev.knobs = [replace(ev.knobs[0], label="y.k", tunable="y.k")]
    elif change == "corpus":
        # By what it lays out, not by where it lives: a capture manifest is an
        # editable file and its path does not move when its gate does.
        ev.corpus = replace(ev.corpus, gate_s=ev.corpus.gate_s + 1.0)
    assert ev.cache_signature() != before


def test_a_second_run_reads_what_the_first_one_measured(tmp_path, monkeypatch):
    first = _cached_evaluator(tmp_path, monkeypatch)
    first._render_terms = lambda values, **_: _terms(harm=float(values[0]))
    first([0.25])
    first([0.75])
    assert len(first.trajectory) == 2

    second = _cached_evaluator(tmp_path, monkeypatch)
    second._render_terms = _refuse_to_render
    second([0.25])
    second([0.75])
    assert second.n_renders == 0
    assert len(second.trajectory) == 2, (
        "a candidate answered from the store is still an evaluation: the budget "
        "counts them, the report prints them, and the search is the poorer for "
        "either one losing sight of it"
    )


def test_a_run_that_starts_from_the_store_still_normalises_against_its_start(
    tmp_path,
    monkeypatch,
):
    """Every loss is a ratio of what the START point scored, and that division
    used to live on the rendering path alone. A run whose first candidate came
    back from the store therefore left the scales unset and minimised the raw
    sum instead — the same search, over a different quantity, silently.
    """
    first = _cached_evaluator(tmp_path, monkeypatch)
    first._render_terms = lambda values, **_: _terms(harm=float(values[0]))
    start, other = first([0.25]), first([0.75])

    second = _cached_evaluator(tmp_path, monkeypatch)
    second._render_terms = _refuse_to_render
    assert (second([0.25]), second([0.75])) == (start, other)
    assert start == 1.0, "the start point is what every other loss is relative to"


def test_a_run_whose_library_was_rebuilt_measures_again(tmp_path, monkeypatch):
    """The C++ moved, so every stored number describes a voice that is gone."""
    first = _cached_evaluator(tmp_path, monkeypatch)
    first._render_terms = lambda values, **_: _terms(harm=float(values[0]))
    first([0.25])

    second = _cached_evaluator(tmp_path, monkeypatch)
    (second.build_dir / "lib" / shared_library_name()).write_bytes(b"rebuilt")
    second._render_terms = lambda values, **_: _terms(harm=float(values[0]))
    second([0.25])
    assert len(second.trajectory) == 1


def test_a_rebuilding_fit_keeps_no_store(tmp_path, monkeypatch):
    """Its library is different for every candidate, so no key could repeat."""
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    path, knob = _source_knob(tmp_path, "a.cpp", "1.0")
    ev = Evaluator(
        [knob], {path: path.read_text()}, [], None, _fit_args(no_cache=False), _stub_build(tmp_path)
    )
    ev._render_terms = lambda values, **_: _terms(harm=1.0)
    ev([2.0])
    assert ev.disk.path is None
    assert not (tmp_path / "scratch").exists()


def test_the_notes_of_a_render_are_measured_at_once_only_when_nothing_else_is(
    tmp_path,
    monkeypatch,
):
    """Threads inside a render and renders beside each other are alternatives.

    Multiplied they would put `--workers` times the probe's note count of
    threads on a machine that is running other things, for no gain: the second
    level of concurrency is competing for the same interpreter lock as the first.
    """
    alone = _cached_evaluator(tmp_path, monkeypatch, workers=1)
    crowded = _cached_evaluator(tmp_path, monkeypatch, workers=4)
    assert alone.metric_threads == autofit.AUTO_METRIC_THREADS
    assert crowded.metric_threads == 1


def test_the_thread_count_reaches_the_process_that_does_the_measuring(
    tmp_path,
    monkeypatch,
):
    """It is resolved in the parent and spent in the child, and nothing in
    between would notice the flag being dropped: the rows come back correct
    either way, only slower."""
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    knob = Knob(label="x.k", lo=0.0, hi=1.0, log=False, start_value=0.5, tunable="x.k")
    ev = Evaluator([knob], {}, [], None, _fit_args(), _stub_build(tmp_path))
    seen: list[list[str]] = []

    def fake_run(cmd, **kwargs):
        seen.append(cmd)
        return subprocess.CompletedProcess(cmd, 0, stdout="[]", stderr="")

    monkeypatch.setattr(render_rows.subprocess, "run", fake_run)
    ev._render_terms([0.5])
    assert "--metric-threads" in seen[0]
    assert seen[0][seen[0].index("--metric-threads") + 1] == str(ev.metric_threads)


def test_a_store_past_its_size_limit_says_so_rather_than_just_shrinking(tmp_path):
    """Measurements disappearing is worth a line even when it is intended."""
    store = open_cache(tmp_path, "sig")
    store.put(("1.0",), {"harm": 2.0})
    store.path.write_text("x" * (eval_cache.MAX_BYTES + 1))
    reopened = open_cache(tmp_path, "sig")
    assert reopened.dropped and reopened.entries == {}


def _write_digest_sources(root: Path) -> None:
    """Create the nested render/scorer layout used by the source fingerprint."""
    files = {
        "metrics_texture.py": b"from shape import density\n",
        "render.py": b"def render():\n    return 1\n",
        "shape/render.py": b"def render_shape():\n    return 2\n",
        "shape/scorers/density.py": b"def score():\n    return 3\n",
    }
    for relative, contents in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)


def test_source_digest_includes_nested_relative_render_and_scorer_sources(tmp_path):
    root = tmp_path / "voicematch"
    _write_digest_sources(root)
    before = eval_cache.source_digest(root)

    (root / "shape/scorers/density.py").write_bytes(b"def score():\n    return 4\n")
    changed = eval_cache.source_digest(root)
    assert changed != before

    (root / "shape/scorers/density.py").rename(root / "shape/scorers/level.py")
    assert eval_cache.source_digest(root) != changed


def test_source_digest_ignores_test_fixtures_and_generated_cache_files(tmp_path):
    root = tmp_path / "voicematch"
    _write_digest_sources(root)
    before = eval_cache.source_digest(root)

    ignored = {
        "test_added.py": b"test = True\n",
        "shape/test_nested.py": b"test = True\n",
        "shape/scorers/score_test_fixtures.py": b"fixture = True\n",
        "shape/conftest.py": b"fixture = True\n",
        "tests/test_scoring.py": b"test = True\n",
        "__pycache__/module.py": b"generated = True\n",
        ".pytest_cache/generated.py": b"generated = True\n",
        ".cache/generated.py": b"generated = True\n",
    }
    for relative, contents in ignored.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)

    assert eval_cache.source_digest(root) == before


def test_source_digest_uses_relative_paths_so_checkout_location_does_not_matter(tmp_path):
    first = tmp_path / "checkout-one"
    second = tmp_path / "checkout-two"
    _write_digest_sources(first)
    _write_digest_sources(second)

    assert eval_cache.source_digest(first) == eval_cache.source_digest(second)


# --------------------------------------------------------------------------- #
# The tree-state precondition: whatever is in src/ at fit startup is what
# gets compiled and fit against, silently, and this tree routinely has several
# concurrent edits to src/ at once.
# --------------------------------------------------------------------------- #
def _init_git_repo(root: Path) -> None:
    """A scratch git repo with a tracked src/ file, for repo_tree_state's own
    git plumbing — a real `git status`/`rev-parse`, not a stand-in for one."""
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    (root / "src").mkdir()
    (root / "src" / "a.cpp").write_text("int a = 1;\n")
    subprocess.run(["git", "add", "."], cwd=root, check=True)
    subprocess.run(
        ["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-q", "-m", "init"],
        cwd=root,
        check=True,
    )


def test_repo_tree_state_reports_a_clean_tree(tmp_path, monkeypatch):
    _init_git_repo(tmp_path)
    monkeypatch.setattr(autofit, "REPO_ROOT", tmp_path)
    head, dirty = autofit.repo_tree_state()
    assert dirty == []
    assert head and len(head) == 40  # a real HEAD sha, not a placeholder


def test_repo_tree_state_names_the_dirty_path(tmp_path, monkeypatch):
    _init_git_repo(tmp_path)
    (tmp_path / "src" / "a.cpp").write_text("int a = 2;  // edited\n")
    monkeypatch.setattr(autofit, "REPO_ROOT", tmp_path)
    head, dirty = autofit.repo_tree_state()
    assert head and len(head) == 40
    assert any("a.cpp" in line for line in dirty)


def _precondition_args(**kwargs) -> argparse.Namespace:
    """The minimum a Namespace needs to reach `run()`'s own tree-state guard."""
    base = {"build_dir": "build-precondition-test", "spec": "auto"}
    base.update(kwargs)
    return argparse.Namespace(**base)


class _PastGuard(Exception):
    """Raised by a stand-in for the first call after the guard, so a test can
    tell "the guard let this through" apart from "the guard never ran"."""


def test_a_clean_tree_proceeds(monkeypatch):
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", []))
    monkeypatch.setattr(autofit, "resolve_probe", lambda args: (_ for _ in ()).throw(_PastGuard()))
    with pytest.raises(_PastGuard):
        autofit.run(_precondition_args())


def test_a_dirty_tree_refuses_and_names_the_paths(monkeypatch):
    """The message carries the actual paths, not a count — the user needs to
    tell "my own edit" from "someone else's" apart, which a count cannot say."""
    dirty = ["M src/midi/synth/bowed_string_voice.cpp", "?? src/midi/synth/new.cpp"]
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", dirty))
    monkeypatch.setattr(
        autofit, "resolve_probe", lambda args: pytest.fail("the guard let a dirty tree through")
    )
    with pytest.raises(RuntimeError) as exc:
        autofit.run(_precondition_args())
    assert "bowed_string_voice.cpp" in str(exc.value)
    assert "new.cpp" in str(exc.value)


def test_allow_dirty_src_proceeds_past_the_guard(monkeypatch):
    dirty = ["M src/midi/synth/bowed_string_voice.cpp"]
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", dirty))
    monkeypatch.setattr(autofit, "resolve_probe", lambda args: (_ for _ in ()).throw(_PastGuard()))
    with pytest.raises(_PastGuard):
        autofit.run(_precondition_args(allow_dirty_src=True))


def test_fold_tree_provenance_adds_to_an_existing_out_artifact(tmp_path):
    out = tmp_path / "result.json"
    out.write_text(json.dumps({"loss": {"best": 0.5}}))
    autofit._fold_tree_provenance(str(out), "deadbeef", ["M src/midi/synth/bowed_string_voice.cpp"])
    record = json.loads(out.read_text())
    assert record["loss"] == {"best": 0.5}  # what was already there survives
    assert record["tree"] == {
        "head": "deadbeef",
        "dirty_src": ["M src/midi/synth/bowed_string_voice.cpp"],
    }


def test_fold_tree_provenance_is_a_noop_without_an_out_path(tmp_path):
    autofit._fold_tree_provenance("", "deadbeef", [])  # must not raise


def test_a_refusal_names_itself_in_the_out_artifact(tmp_path):
    """Unchanged values have two readings and only the record can tell them apart.

    A search that found nothing worth writing and a winner refused for losing to
    its own start leave the same tree; the record names the refusal.
    """
    knobs = [_reach_knob("a", 0.0, 1.0, 0.25)]
    lost = argparse.Namespace(
        normalize=True, best_loss=1.1536, start_tnr_notes=7.0, best_tnr_notes=6.0
    )
    assert winner_or_defaults(knobs, [0.9], lost) == [0.25]
    assert lost.write_back_refusal == "lost_to_start"

    out = tmp_path / "result.json"
    out.write_text(json.dumps({"loss": {"best": 1.1536}}))
    autofit._fold_write_back_verdict(str(out), lost)
    record = json.loads(out.read_text())
    assert record["loss"] == {"best": 1.1536}  # what was already there survives
    assert record["write_back"] == {
        "refused": "lost_to_start",
        "tnr_notes": {"start": 7.0, "best": 6.0},
        "absent_charged": {"start": None, "best": None},
        "floor_dropped": None,
        "not_computed": [],
    }


def test_the_write_back_record_names_the_readings_the_floor_took_and_the_terms_it_emptied(tmp_path):
    state = argparse.Namespace(
        normalize=True,
        best_loss=1.0,
        start_tnr_notes=0.0,
        best_tnr_notes=0.0,
        floor_dropped={"tnr": 3.0, "body": 0.0},
        floor_not_computed=["tnr"],
        start_absent_charged=4.0,
        best_absent_charged=1.0,
    )
    out = tmp_path / "result.json"
    out.write_text("{}")
    autofit._fold_write_back_verdict(str(out), state)
    record = json.loads(out.read_text())["write_back"]
    assert record["floor_dropped"] == {"tnr": 3.0, "body": 0.0}
    assert record["not_computed"] == ["tnr"]
    assert record["absent_charged"] == {"start": 4.0, "best": 1.0}


def test_each_refusal_is_distinguishable_and_a_write_names_none():
    knobs = [_reach_knob("a", 0.0, 1.0, 0.25)]
    lost = argparse.Namespace(normalize=True, best_loss=1.1536)
    winner_or_defaults(knobs, [0.9], lost)
    assert lost.write_back_refusal == "lost_to_start"

    overfit = argparse.Namespace(normalize=True, best_loss=0.8692)
    winner_or_defaults(
        knobs,
        [0.9],
        overfit,
        {"axis": "velocities", "held_out": "48,88,112", "start": 1.0, "best": 1.1924},
    )
    assert overfit.write_back_refusal == "fitted_to_the_probe"

    # A run that wrote its winner must not leave a stale refusal behind it.
    won = argparse.Namespace(normalize=True, best_loss=0.5071, write_back_refusal="lost_to_start")
    assert winner_or_defaults(knobs, [0.9], won) == [0.9]
    assert won.write_back_refusal is None


def test_fold_write_back_verdict_is_a_noop_without_an_out_path():
    autofit._fold_write_back_verdict("", argparse.Namespace())  # must not raise


def test_fold_write_back_verdict_keeps_nonfinite_measurements_as_json_null(tmp_path):
    out = tmp_path / "result.json"
    out.write_text(json.dumps({"loss": {"best": None}}))
    evaluator = argparse.Namespace(
        write_back_refusal=None,
        start_tnr_notes=float("inf"),
        best_tnr_notes=float("nan"),
    )
    autofit._fold_write_back_verdict(str(out), evaluator)
    text = out.read_text()
    assert "Infinity" not in text and "NaN" not in text
    record = json.loads(text)
    assert record["write_back"]["tnr_notes"] == {"start": None, "best": None}


def test_allow_dirty_src_run_records_provenance_in_out(tmp_path, monkeypatch):
    """The opt-out path all the way through `run()`: the artifact --diagnose
    writes carries the dirty paths and the sha, not just a warning on stderr
    that a long backgrounded run's log goes unread past."""
    dirty = ["M src/midi/synth/bowed_string_voice.cpp"]
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", dirty))
    monkeypatch.setattr(autofit, "resolve_probe", lambda args: None)
    monkeypatch.setattr(autofit, "apply_spec_weights", lambda args, argv: None)
    monkeypatch.setattr(autofit, "load_spec", lambda path: [])
    monkeypatch.setattr(autofit, "configure_build", lambda *a, **k: None)
    monkeypatch.setattr(autofit, "resolve_corpus", lambda args: None)
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(
        autofit,
        "oracle_reference",
        lambda args: ([], np.zeros(4, dtype=np.float32), None, None),
    )

    def fake_diagnosis(evaluator, knobs, args, catalogue, out_path=""):
        if out_path:
            Path(out_path).write_text(json.dumps({"terms": []}))

    monkeypatch.setattr(autofit, "run_diagnosis", fake_diagnosis)

    out = tmp_path / "diag.json"
    args = _fit_args(
        spec="ignored",
        program=0,
        drum_note=None,
        dump_knobs=False,
        program_only=False,
        bank=0,
        build_dir="build-precondition-test",
        screen=False,
        stages=False,
        diagnose=True,
        grid=0,
        optimizer="coord",
        max_evals=30,
        metric_threads=0,
        out=str(out),
        dry_run=True,
        allow_dirty_src=True,
        drum_gate_ms=0,
    )
    assert autofit.run(args) == 0
    record = json.loads(out.read_text())
    assert record["terms"] == []  # what the (faked) diagnosis wrote survives
    assert record["tree"] == {"head": "deadbeef", "dirty_src": dirty}


def test_the_guard_never_runs_inside_the_rebuild_path():
    """The precondition belongs ONCE at startup and nowhere a rebuilding fit's
    own candidates pass through afterwards.

    `Evaluator.needs_rebuild` is true whenever a knob has no `SONARE_TUNABLE`
    behind it, and that fit writes every candidate's values into src/ and
    rebuilds, so src/ is dirty by design for the whole run once it starts. A
    check placed inside `build_shared` or a per-candidate call site would see
    that self-made dirt on the fit's own second candidate and refuse it for
    doing exactly what it is supposed to.

    If `repo_tree_state()` were ever added to one of these, this fails with
    e.g. "build_shared calls repo_tree_state() — that refuses a rebuilding fit
    on its own second candidate, since src/ is dirty by design once one
    starts" instead of a rebuilding fit silently refusing itself.
    """
    import inspect

    sites = {
        "build_shared": build_lib.build_shared,
        "Evaluator.__call__": Evaluator.__call__,
        "Evaluator._ensure_built": Evaluator._ensure_built,
        "Evaluator.evaluate_batch": Evaluator.evaluate_batch,
    }
    for name, fn in sites.items():
        assert "repo_tree_state" not in inspect.getsource(fn), (
            f"{name} calls repo_tree_state() — that refuses a rebuilding fit on "
            f"its own second candidate, since src/ is dirty by design once one "
            f"starts"
        )


def _stub_status(tmp_path, monkeypatch, gates):
    import refit

    row = {"slug": "v", "kit": False, "program": 90, "bank": 0, "patch": "p", "capture": "c"}
    status = tmp_path / "voice-status.json"
    status.write_text(json.dumps({"voices": [row]}))
    monkeypatch.setattr(refit, "STATUS", status)
    monkeypatch.setattr(refit, "capital_gates", lambda *a, **k: gates)
    monkeypatch.setattr(autofit.bank, "captures", list)


def _gate_args():
    return argparse.Namespace(corpus="", program=90, bank=0, drum_note=None)


def test_gated_dimensions_without_a_corpus_is_the_union_of_the_voices_gates(tmp_path, monkeypatch):
    one, two = tmp_path / "one_gate.json", tmp_path / "two_gate.json"
    one.write_text(json.dumps({"bounds": {"decay": {}, "tnr": {}}}))
    two.write_text(json.dumps({"bounds": {"tnr": {}, "centroid_pct": {}}}))
    gates = [
        {"capture": "one", "gate": str(one), "timbre": "t"},
        {"capture": "two", "gate": str(two), "timbre": "t"},
    ]
    _stub_status(tmp_path, monkeypatch, gates)
    got, why = autofit.gated_dimensions(_gate_args())
    assert got == frozenset({"decay", "tnr", "centroid_pct"})
    assert why == ""


def test_gated_dimensions_reports_nothing_gated_with_its_message(tmp_path, monkeypatch):
    _stub_status(tmp_path, monkeypatch, [])
    assert autofit.gated_dimensions(_gate_args()) == (None, "no gate resolves to this voice")


# --------------------------------------------------------------------------
# the noise-led default weight on `bright`


def _noise_led_args(monkeypatch, tmp_path, tnr_values, *, channel=1, **given):
    """Args for a single capture whose committed reference rows hold @p tnr_values."""
    import autofit_resolve

    (tmp_path / "reference").mkdir(exist_ok=True)
    rows = [{"timbre": "t0", "tnr_db": v} for v in tnr_values]
    (tmp_path / "reference" / "cap.json").write_text(json.dumps({"rows": rows}))
    monkeypatch.setattr(autofit_resolve, "HERE", tmp_path)
    corpus = argparse.Namespace(capture_id="cap", timbre="t0", channel=channel)
    monkeypatch.setattr(autofit_resolve, "load_corpus", lambda path, timbre: corpus)
    return argparse.Namespace(
        corpus="cap-dir", corpus_timbre="t0", program=0, w_bright=None, **given
    )


@pytest.mark.parametrize(("tnr", "weighted"), [(5.9, True), (6.0, True), (6.1, False)])
def test_bright_takes_a_default_weight_only_on_a_noise_led_reference(
    monkeypatch, tmp_path, capsys, tnr, weighted
):
    import autofit_resolve

    args = _noise_led_args(monkeypatch, tmp_path, [tnr, tnr, tnr])
    autofit_resolve.apply_noise_led_bright(args)
    err = capsys.readouterr().err
    if weighted:
        assert args.w_bright == autofit_resolve.PERCUSSION_WEIGHTS["bright"]
        assert f"bright weighted: cap reference tnr {tnr:.1f} dB ≤ 6 (noise-led trigger)" in err
    else:
        assert args.w_bright is None and "bright weighted" not in err


def test_a_given_bright_weight_is_never_replaced_by_the_trigger(monkeypatch, tmp_path):
    import autofit_resolve

    args = _noise_led_args(monkeypatch, tmp_path, [1.0])
    args.w_bright = 0.0
    autofit_resolve.apply_noise_led_bright(args)
    assert args.w_bright == 0.0


def test_a_kit_is_left_to_its_own_class_defaults(monkeypatch, tmp_path):
    import autofit_resolve

    args = _noise_led_args(monkeypatch, tmp_path, [1.0], channel=10)
    autofit_resolve.apply_noise_led_bright(args)
    assert args.w_bright is None


def test_the_noise_led_trigger_stays_outside_the_objective():
    import objective

    modules = set(objective.objective_modules())
    assert not modules & {"autofit_resolve", "autofit", "refit", "profile_gate", "gate_audit"}

    import autofit_resolve

    assert autofit_resolve.NOISE_LED_TNR_DB == 6.0


def _joint_noise_args(monkeypatch, tmp_path, tnr_by_capture, **given):
    """Args for a joint fit over one capture per entry, each with its committed median tnr."""
    import autofit_resolve

    (tmp_path / "reference").mkdir(exist_ok=True)
    corpora = {}
    for cap, tnr in tnr_by_capture.items():
        rows = [{"timbre": "t0", "tnr_db": tnr}]
        (tmp_path / "reference" / f"{cap}.json").write_text(json.dumps({"rows": rows}))
        corpora[f"dir-{cap}"] = argparse.Namespace(capture_id=cap, timbre="t0", channel=1)
    monkeypatch.setattr(autofit_resolve, "HERE", tmp_path)
    monkeypatch.setattr(autofit_resolve, "load_corpus", lambda path, timbre: corpora[path])
    return argparse.Namespace(
        corpus=list(corpora), corpus_timbre=["t0"] * len(corpora), program=0, w_bright=None, **given
    )


def test_one_noise_led_capture_of_a_joint_fit_weighs_bright_and_is_named(
    monkeypatch, tmp_path, capsys
):
    import autofit_resolve

    args = _joint_noise_args(monkeypatch, tmp_path, {"tonal": 25.0, "airy": 4.5, "other": 12.0})
    autofit_resolve.apply_noise_led_bright(args)
    err = capsys.readouterr().err
    assert args.w_bright == autofit_resolve.PERCUSSION_WEIGHTS["bright"]
    assert "bright weighted: airy reference tnr 4.5 dB ≤ 6 (noise-led trigger)" in err


def test_a_joint_fit_of_tonal_captures_only_is_left_alone(monkeypatch, tmp_path, capsys):
    import autofit_resolve

    args = _joint_noise_args(monkeypatch, tmp_path, {"a": 25.0, "b": 6.5})
    autofit_resolve.apply_noise_led_bright(args)
    assert args.w_bright is None and "bright weighted" not in capsys.readouterr().err


def test_refit_reads_the_same_noise_led_trigger_over_a_joint_voice(monkeypatch, tmp_path):
    import refit

    args = _joint_noise_args(monkeypatch, tmp_path, {"tonal": 25.0, "airy": 4.5})
    voice = {
        "program": 0,
        "corpus": "dir-tonal",
        "timbre": "t0",
        "joint": [
            {"corpus": "dir-tonal", "timbre": "t0"},
            {"corpus": "dir-airy", "timbre": "t0"},
        ],
    }
    assert args.corpus == ["dir-tonal", "dir-airy"]
    assert refit.profile_weights(voice)["bright"] == refit.PERCUSSION_WEIGHTS["bright"]
    voice["joint"] = voice["joint"][:1]
    assert "bright" not in refit.profile_weights(voice)


# --------------------------------------------------------------------------
# the pre-fit reading of a bare autofit


def _prefit_world(tmp_path, monkeypatch, bounds, *, spread=None, **weights):
    """A one-gate voice whose gate stores @p bounds, with the bank's far threshold at 10 floors."""
    import refit

    gate = tmp_path / "cap_gate.json"
    gate.write_text(json.dumps({"bounds": bounds, "reference_spread": spread or {}}))
    units = tmp_path / "floor-units.json"
    units.write_text(json.dumps({"p90": 10.0}))
    monkeypatch.setattr(refit, "FLOOR_UNITS_PATH", units)
    voice = {"slug": "v", "gates": [{"capture": "cap", "gate": str(gate)}]}
    monkeypatch.setattr(autofit, "gate_sources", lambda args: (voice, ""))
    args = _fit_args()
    for term, weight in weights.items():
        setattr(args, f"w_{term}", weight)
    return args


def _bound(stat, limit, measured):
    return {stat: limit, "provenance": {stat: {"measured": measured}}}


def test_a_bare_autofit_raises_a_red_dimensions_zero_weight_own_term(tmp_path, monkeypatch, capsys):
    bounds = {"body": _bound("p90", 1.0, 2.0)}
    args = _prefit_world(tmp_path, monkeypatch, bounds)
    assert "body" not in autofit.cli_weights(args)
    prefit = autofit.apply_prefit(args)
    assert args.w_body == 1.0 and autofit.cli_weights(args)["body"] == 1.0
    assert prefit["prefit_weights"] == {"body": 1.0} and prefit["readings"] == 1
    assert "v: [raised from zero weight: body]" in capsys.readouterr().err


def test_a_pre_fit_raise_reaches_every_joint_capture(tmp_path, monkeypatch):
    bounds = {"body": _bound("p90", 1.0, 2.0)}
    args = _prefit_world(tmp_path, monkeypatch, bounds)
    # As `resolve_probes` copies them, before the pre-fit reading runs.
    captures = [argparse.Namespace(**vars(args)) for _ in range(2)]
    autofit.apply_prefit(args, captures)
    for ns in (args, *captures):
        assert ns.w_body == 1.0 and autofit.cli_weights(ns)["body"] == 1.0


def test_a_bare_autofit_raises_a_far_dimension_and_labels_a_guessed_floor(
    tmp_path, monkeypatch, capsys
):
    bounds = {"centroid_pct": _bound("abs_median", 999.0, 22.0)}
    args = _prefit_world(tmp_path, monkeypatch, bounds, spread={"centroid_pct": 2.0})
    prefit = autofit.apply_prefit(args)
    assert args.w_bright == 1.0 and prefit["raised_far"] == ["bright"]
    assert "v: centroid_pct far: 11 floors (bank p90 10)" in capsys.readouterr().err
    guessed = _prefit_world(tmp_path, monkeypatch, bounds)
    autofit.apply_prefit(guessed)
    assert "guessed floors" in capsys.readouterr().err


def test_an_already_weighted_dimension_prints_why_nothing_was_raised(tmp_path, monkeypatch, capsys):
    bounds = {
        "attack_hf_swing": _bound("p90", 1.0, 2.0),
        "stretch": _bound("abs_median", 999.0, 22.0),
    }
    args = _prefit_world(tmp_path, monkeypatch, bounds, spread={"stretch": 2.0})
    weights = autofit.cli_weights(args)
    prefit = autofit.apply_prefit(args)
    err = capsys.readouterr().err
    assert prefit["prefit_weights"] == {} and autofit.cli_weights(args) == weights
    assert f"v: attack_hf_swing already weighted ({weights['hfdyn']:g}): red" in err
    assert f"v: stretch already weighted ({weights['cents']:g}): far 11 floors" in err


def test_the_users_zero_weight_wins_over_a_bare_autofits_pre_fit_raise(
    tmp_path, monkeypatch, capsys
):
    bounds = {"body": _bound("p90", 1.0, 2.0)}
    args = _prefit_world(tmp_path, monkeypatch, bounds, body=0.0)
    prefit = autofit.apply_prefit(args)
    assert args.w_body == 0.0 and prefit["prefit_weights"] == {} and prefit["user_zero"] == ["body"]
    assert "kept at the weight 0 given: body" in capsys.readouterr().err


def test_a_gate_with_no_stored_measurement_raises_nothing(tmp_path, monkeypatch, capsys):
    bounds = {"body": {"p90": 1.0, "provenance": {"p90": {"measured": None}}}}
    args = _prefit_world(tmp_path, monkeypatch, bounds)
    prefit = autofit.apply_prefit(args)
    assert prefit["readings"] == 0 and prefit["prefit_weights"] == {}
    assert capsys.readouterr().err == ""


def _refusable_pitched_own_terms():
    """(dimension, term, flag) for every pitched own term a probe's shape can refuse."""
    from loss_weights import DROP_REASONS, GATE_OWN_TERMS

    return [
        (dim, term, DROP_REASONS[term][0])
        for dim, terms in GATE_OWN_TERMS.items()
        for term in terms
        if term in DROP_REASONS
    ]


@pytest.mark.parametrize("dim,term,flag", _refusable_pitched_own_terms())
def test_a_term_the_probe_refuses_is_skipped_by_prefit_and_presence_and_named_once(
    tmp_path, monkeypatch, capsys, dim, term, flag
):
    from loss_weights import DROP_REASONS

    args = _prefit_world(tmp_path, monkeypatch, {dim: _bound("p90", 1.0, 2.0)})
    setattr(args, flag, False)
    prefit = autofit.apply_prefit(args)
    plan = autofit.apply_presence(args, [args])
    assert term not in prefit["prefit_weights"] and term not in plan["terms"]
    assert getattr(args, f"w_{term}", None) is None and term not in autofit.cli_weights(args)
    assert prefit["skipped"] == plan["skipped"] == {term: DROP_REASONS[term][1]}
    err = capsys.readouterr().err
    assert err.count(f"{term} not weighted: {DROP_REASONS[term][1]}") == 1


def _presence_bounds():
    """A gate bounding one dimension with an own term and a p90, and two none can weigh."""
    return {
        "body": {"median": 1.0, "abs_median": 1.0, "p90": 2.0},
        "decay": {"median": 1.0, "p90": 2.0},
        "vel_range": {"abs_median": 1.0},
    }


def test_every_gated_dimension_gets_its_own_term_at_the_presence_weight(
    tmp_path, monkeypatch, capsys
):
    from loss_aggregate import PRESENCE_TAIL_FRACTION
    from loss_weights import PRESENCE_WEIGHT

    args = _prefit_world(tmp_path, monkeypatch, _presence_bounds())
    args.has_velocity_spread = False
    capture = argparse.Namespace(**vars(args))
    assert "body" not in autofit.cli_weights(args)
    plan = autofit.apply_presence(args, [args, capture])
    # body's own term is added; decay has none, and velspan is dropped on a one-velocity probe.
    assert list(plan["terms"]) == ["body"] and plan["unscorable"] == ["decay", "vel_range"]
    body = plan["terms"]["body"]
    assert body["weight"] == PRESENCE_WEIGHT and body["dimensions"] == ["body"]
    assert body["aggregate"] == "tail" and body["tail_fraction"] == PRESENCE_TAIL_FRACTION
    assert body["covers"] == ["abs_median", "median", "p90"]
    for ns in (args, capture):
        assert autofit.cli_weights(ns)["body"] == PRESENCE_WEIGHT
        assert ns.term_tails == {"body": PRESENCE_TAIL_FRACTION}
    err = capsys.readouterr().err
    assert "presence: 1 term(s) weighted for gated dimensions (body<-body tail 0.1)" in err
    assert "presence: unscorable: decay, vel_range" in err


def test_a_users_zero_weight_wins_over_the_presence_weight(tmp_path, monkeypatch, capsys):
    args = _prefit_world(tmp_path, monkeypatch, _presence_bounds(), body=0.0)
    plan = autofit.apply_presence(args, [args])
    assert plan["terms"] == {} and plan["user_zero"] == {"body": ["body"]}
    assert args.w_body == 0.0 and "body" not in autofit.cli_weights(args)
    assert "presence: user zero: body (body)" in capsys.readouterr().err


def test_an_already_weighted_own_term_takes_the_p90_tail_without_a_presence_weight(
    tmp_path, monkeypatch, capsys
):
    from loss_aggregate import PRESENCE_TAIL_FRACTION

    args = _prefit_world(tmp_path, monkeypatch, {"tnr": {"median": 1.0, "p90": 2.0}})
    weight = autofit.cli_weights(args)["tnr"]
    plan = autofit.apply_presence(args, [args])
    assert plan["terms"] == {} and autofit.cli_weights(args)["tnr"] == weight
    tnr = plan["weighted"]["tnr"]
    assert tnr["weight"] == weight and tnr["aggregate"] == "tail"
    assert tnr["tail_fraction"] == PRESENCE_TAIL_FRACTION and tnr["covers"] == ["median", "p90"]
    assert args.term_tails == {"tnr": PRESENCE_TAIL_FRACTION}
    assert "tails: weighted own term(s) tailed for a gated p90 (tnr<-tnr tail 0.1)" in (
        capsys.readouterr().err
    )
    # Without a p90 bound the weighted term keeps the run's reduction.
    plain = _prefit_world(tmp_path, monkeypatch, {"tnr": {"median": 1.0}})
    autofit.apply_presence(plain, [plain])
    assert plain.term_tails == {} and plain.presence["weighted"]["tnr"]["covers"] == ["median"]


def test_a_pitched_crest_has_no_own_term(tmp_path, monkeypatch):
    """Peak over held RMS is not the gate's crest reading, so weighing it is not owning it."""
    args = _prefit_world(tmp_path, monkeypatch, {"crest": {"median": 1.0, "p90": 2.0}})
    assert autofit.cli_weights(args)["crest"] > 0.0
    plan = autofit.apply_presence(args, [args])
    assert plan["terms"] == plan["weighted"] == {} and plan["unscorable"] == ["crest"]


def test_recorded_tails_are_used_as_given_and_a_mismatch_is_refused(tmp_path, monkeypatch):
    args = _prefit_world(tmp_path, monkeypatch, {"tnr": {"p90": 2.0}})
    args.term_tails_given = "tnr=0.05"
    autofit.apply_presence(args, [args])
    assert args.term_tails == {"tnr": 0.05}
    wrong = _prefit_world(tmp_path, monkeypatch, {"tnr": {"p90": 2.0}})
    wrong.term_tails_given = "body=0.1"
    with pytest.raises(ValueError, match="--term-tails names"):
        autofit.apply_presence(wrong, [wrong])


def _presence_member(args, terms, stage="fit"):
    loss = autofit.LossWeights(autofit.cli_weights(args))
    loss.calibrate(terms)
    return argparse.Namespace(
        args=args, baseline_terms=terms, normalize=True, stage=stage, loss=loss
    )


def test_the_start_point_caps_a_presence_term_before_the_search(tmp_path, monkeypatch, capsys):
    from loss_weights import PRESENCE_TERM_SHARE, PRESENCE_WEIGHT, TERM_UNITS

    args = _prefit_world(tmp_path, monkeypatch, _presence_bounds())
    autofit.apply_presence(args, [args])
    # One unit of harm, and a body forty units out: at 0.25 it would carry most of the loss.
    terms = {"harm": TERM_UNITS["harm"], "body": 40.0 * TERM_UNITS["body"]}
    member = _presence_member(args, terms)
    autofit.fix_presence(args, [member])
    body = args.presence["terms"]["body"]
    assert body["cap"] == "term" and body["capped_from"] == PRESENCE_WEIGHT
    assert 0.0 < body["weight"] < PRESENCE_WEIGHT and args.w_body == body["weight"]
    contrib = member.loss.term_contributions(terms)
    assert contrib["body"] <= PRESENCE_TERM_SHARE * sum(contrib.values()) + 1e-12
    # Calibrated under the capped weights, so the start still scores exactly 1.0.
    assert member.loss.weights["body"] == body["weight"]
    assert member.loss.combine(terms) == pytest.approx(1.0)
    assert "presence: capped at the start point: body 0.25->" in capsys.readouterr().err
    # Once: a later point never re-caps, so the objective is fixed through the search.
    autofit.fix_presence(args, [_presence_member(args, {"harm": 1.0, "body": 1.0})])
    assert args.presence["terms"]["body"]["weight"] == body["weight"]


def test_a_staged_start_caps_the_cli_weights_and_leaves_the_stage_loss(tmp_path, monkeypatch):
    from loss_weights import TERM_UNITS

    args = _prefit_world(tmp_path, monkeypatch, _presence_bounds())
    autofit.apply_presence(args, [args])
    terms = {"harm": TERM_UNITS["harm"], "body": 40.0 * TERM_UNITS["body"]}
    member = _presence_member(args, terms, stage="excitation")
    stage_loss = member.loss
    autofit.fix_presence(args, [member])
    assert member.loss is stage_loss and args.w_body < 0.25


def test_recorded_presence_weights_are_used_as_given_and_a_mismatch_is_refused(
    tmp_path, monkeypatch
):
    args = _prefit_world(tmp_path, monkeypatch, _presence_bounds())
    args.presence_weights = "body=0.0625"
    plan = autofit.apply_presence(args, [args])
    assert plan["capped"] and plan["pinned"] and args.w_body == 0.0625
    autofit.fix_presence(args, [_presence_member(args, {"harm": 1.0, "body": 99.0})])
    assert args.w_body == 0.0625
    wrong = _prefit_world(tmp_path, monkeypatch, _presence_bounds())
    wrong.presence_weights = "bright=0.1"
    with pytest.raises(ValueError, match="--presence-weights names"):
        autofit.apply_presence(wrong, [wrong])


def test_the_presence_plan_is_recorded_under_fit_profile(tmp_path, monkeypatch):
    args = _prefit_world(tmp_path, monkeypatch, _presence_bounds())
    autofit.apply_presence(args, [args])
    out = tmp_path / "fit.json"
    out.write_text(json.dumps({"fit_profile": {"weights": {"harm": 1.0}}}))
    autofit._fold_presence(str(out), args)
    profile = json.loads(out.read_text())["fit_profile"]
    assert profile["weights"] == {"harm": 1.0}
    assert profile["presence"]["body"]["dimensions"] == ["body"]
    assert profile["presence"]["body"]["covers"] == ["abs_median", "median", "p90"]
    assert profile["unscorable"] == ["decay"] and profile["presence_capped"] is False
    # vel_range's velspan is already weighted on this probe and bounds no p90: recorded, not tailed.
    assert profile["tails"]["velspan"]["aggregate"] == "mean"
    assert profile["term_tails"] == {"body": 0.1}


def test_refit_and_autofit_print_the_already_weighted_line_from_one_function():
    import refit

    got = refit.already_weighted_lines(
        "v",
        {
            "stretch": {"weight": 0.5, "red": True, "far": {"floors": 11.0, "guessed": True}},
        },
    )
    assert got == [
        "v: stretch already weighted (0.5): red",
        "v: stretch already weighted (0.5): far 11 guessed floors",
    ]


# --------------------------------------------------------------------------
# the held window, in the fit's own start-up lines


def test_a_capture_prints_its_held_window_against_the_models(tmp_path, capsys):
    root = _write_corpus(tmp_path / "c", notes=(60,), velocities=(56,), gate_ms=1500, seconds=3.6)
    args = autofit.argparse.Namespace(**vars(_fit_args()))
    args.corpus, args.corpus_timbre, args.pattern = str(root), "t", "corpus"
    autofit.resolve_probe(args)
    err = capsys.readouterr().err.splitlines()
    assert err.count("window: model 1.5 s / reference 1.5 s") == 1
    assert err.index("window: model 1.5 s / reference 1.5 s") == (
        next(i for i, ln in enumerate(err) if ln.startswith("corpus oracle:")) + 1
    )
