"""What a fit actually ran against: that a probe reached the code, the tree
state it compiled, and the measurements it kept across runs.

    rye run --pyproject bindings/python/pyproject.toml \\
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
    return Knob(label=label, lo=lo, hi=hi, log=False, start_value=start,
                tunable=f"file.{label}")


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
    overfit = {"axis": "velocities", "held_out": "48,88,112",
               "start": 1.0, "best": 1.1924}
    assert winner_or_defaults(knobs, [0.9], won, overfit) == [0.25]
    generalises = {**overfit, "best": 0.4803}
    assert winner_or_defaults(knobs, [0.9], won, generalises) == [0.9]
    wash = {**overfit, "best": 1.0043}
    assert winner_or_defaults(knobs, [0.9], won, wash) == [0.9]
    # No hold-out asked for, no verdict to act on.
    assert winner_or_defaults(knobs, [0.9], won, None) == [0.9]


def test_a_winner_that_stopped_being_scored_on_noise_writes_nothing():
    """`tnr` charges only where the model is noisier, so leaving is free.

    The shamisen's buzz is its sawari and it is the only between-partial energy
    the model has. A fit took it from 0.5 to 0.146 and the gate then read the
    voice 29.97 dB cleaner than its reference against a 1.81 bound, while the
    term reported 0.00 at every candidate — not a match, but a comparison that
    had stopped happening. The count of notes it still charged for is what
    separates those, and it falls as the model walks past the reference.
    """
    knobs = [_reach_knob("a", 0.0, 1.0, 0.25)]
    quit_early = argparse.Namespace(normalize=True, best_loss=0.5071,
                                    start_tnr_notes=7.0, best_tnr_notes=0.0)
    assert winner_or_defaults(knobs, [0.9], quit_early) == [0.25]
    # Still scored on every note it started with: nothing went quiet.
    held = argparse.Namespace(normalize=True, best_loss=0.5071,
                              start_tnr_notes=7.0, best_tnr_notes=7.0)
    assert winner_or_defaults(knobs, [0.9], held) == [0.9]
    # A model cleaner than a sampled reference from the start is ordinary, and
    # the term never spoke about any candidate — there is no delta to read.
    silent = argparse.Namespace(normalize=True, best_loss=0.5071,
                                start_tnr_notes=0.0, best_tnr_notes=0.0)
    assert winner_or_defaults(knobs, [0.9], silent) == [0.9]
    # A --raw-loss run leaves both anchors unset; the guard has nothing to read.
    unanchored = argparse.Namespace(normalize=True, best_loss=0.5071,
                                    start_tnr_notes=None, best_tnr_notes=None)
    assert winner_or_defaults(knobs, [0.9], unanchored) == [0.9]


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
    monkeypatch.setattr(ev, "_render_terms",
                        lambda values: {t: 1.0 for t in LOSS_TERMS}, raising=False)
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
    ev.check_overrides_reach([_reach_knob("a", 0.0, 1.0, 0.25),
                              _reach_knob("silencer", 0.0, 1.0, 0.25)])
    # Start, the unscorable all-at-once render, then the first knob on its own.
    assert seen == [[0.25, 0.25], [1.0, 1.0], [1.0, 0.25]]


def test_a_probe_that_measures_nothing_as_shipped_says_so_rather_than_blaming_overrides(
        monkeypatch):
    """No baseline means no fit, and the reason is the probe rather than the
    environment — a message about BUILD_TUNING would send the reader nowhere."""
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    monkeypatch.setattr(ev, "_render_terms", lambda values: None, raising=False)
    with pytest.raises(RuntimeError, match="own start values"):
        ev.check_overrides_reach([_reach_knob("a", 0.0, 1.0, 0.25)])


def test_the_one_at_a_time_fallback_still_catches_a_library_that_ignores_overrides(
        monkeypatch):
    """The fallback must not turn the plumbing failure into a pass: if no single
    knob moves anything either, the run is still searching nothing."""
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")

    def render(values):
        return None if values == [1.0, 1.0] else {t: 1.0 for t in LOSS_TERMS}

    monkeypatch.setattr(ev, "_render_terms", render, raising=False)
    with pytest.raises(RuntimeError, match="one range at a time"):
        ev.check_overrides_reach([_reach_knob("a", 0.0, 1.0, 0.25),
                                  _reach_knob("b", 0.0, 1.0, 0.25)])


def test_a_source_only_spec_has_no_override_plumbing_to_check(monkeypatch):
    """A rebuilding knob does not travel through the environment at all."""
    ev = Evaluator.__new__(Evaluator)
    ev.build_dir = Path("build")
    monkeypatch.setattr(ev, "_render_terms",
                        lambda values: pytest.fail("a source knob needs no reach check"),
                        raising=False)
    ev.check_overrides_reach([Knob(label="a", lo=0.0, hi=1.0, log=False,
                                   start_value=0.5, file=Path("x.cpp"), pattern="p")])


# --------------------------------------------------------------------------- #
# Measurements kept across runs
# --------------------------------------------------------------------------- #
def _refuse_to_render(values):
    raise AssertionError(f"rendered {values}, which the store already held")


def _stub_build(tmp_path: Path, contents: bytes = b"lib") -> Path:
    build = tmp_path / "build"
    (build / "lib").mkdir(parents=True, exist_ok=True)
    (build / "lib" / "libsonare.dylib").write_bytes(contents)
    return build


def _cached_evaluator(tmp_path, monkeypatch, *, corpus=None, **kwargs):
    """An `Evaluator` whose store is under `tmp_path` and whose build is a stub."""
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    knob = Knob(label="x.k", lo=0.0, hi=1.0, log=False, start_value=0.5, tunable="x.k")
    args = _fit_args(no_cache=False, **kwargs)
    return Evaluator([knob], {}, [{"note": 60}], None, args, _stub_build(tmp_path),
                     corpus=corpus)


def test_the_store_hands_back_what_it_was_given(tmp_path):
    store = open_cache(tmp_path, "sig")
    store.put(("1.0",), {"harm": 2.0})
    store.put(("2.0",), None)   # a render that produced nothing scorable
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
    "change", ["library", "harness", "oracle", "probe", "knobs", "corpus"],
)
def test_the_signature_moves_with_everything_a_stored_value_depends_on(
    tmp_path, monkeypatch, change,
):
    """A stored term is a number some particular code produced from some
    particular inputs. Anything the key leaves out is something the store will
    happily answer with after it has changed."""
    ev = _cached_evaluator(
        tmp_path, monkeypatch,
        corpus=load_corpus(_write_corpus(tmp_path / "corpus", notes=(60, 72))),
    )
    before = ev.cache_signature()
    if change == "library":
        (ev.build_dir / "lib" / "libsonare.dylib").write_bytes(b"rebuilt")
    elif change == "harness":
        monkeypatch.setattr(autofit, "source_digest", lambda d: "edited")
    elif change == "oracle":
        ev.oracle = [{"note": 61}]
    elif change == "probe":
        ev.args.notes = "48,60"
    elif change == "knobs":
        ev.knobs = [replace(ev.knobs[0], label="y.k", tunable="y.k")]
    elif change == "corpus":
        # By what it lays out, not by where it lives: a capture manifest is an
        # editable file and its path does not move when its gate does.
        ev.corpus = replace(ev.corpus, gate_s=ev.corpus.gate_s + 1.0)
    assert ev.cache_signature() != before


def test_a_second_run_reads_what_the_first_one_measured(tmp_path, monkeypatch):
    first = _cached_evaluator(tmp_path, monkeypatch)
    first._render_terms = lambda values: _terms(harm=float(values[0]))
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
    tmp_path, monkeypatch,
):
    """Every loss is a ratio of what the START point scored, and that division
    used to live on the rendering path alone. A run whose first candidate came
    back from the store therefore left the scales unset and minimised the raw
    sum instead — the same search, over a different quantity, silently.
    """
    first = _cached_evaluator(tmp_path, monkeypatch)
    first._render_terms = lambda values: _terms(harm=float(values[0]))
    start, other = first([0.25]), first([0.75])

    second = _cached_evaluator(tmp_path, monkeypatch)
    second._render_terms = _refuse_to_render
    assert (second([0.25]), second([0.75])) == (start, other)
    assert start == 1.0, "the start point is what every other loss is relative to"


def test_a_run_whose_library_was_rebuilt_measures_again(tmp_path, monkeypatch):
    """The C++ moved, so every stored number describes a voice that is gone."""
    first = _cached_evaluator(tmp_path, monkeypatch)
    first._render_terms = lambda values: _terms(harm=float(values[0]))
    first([0.25])

    second = _cached_evaluator(tmp_path, monkeypatch)
    (second.build_dir / "lib" / "libsonare.dylib").write_bytes(b"rebuilt")
    second._render_terms = lambda values: _terms(harm=float(values[0]))
    second([0.25])
    assert len(second.trajectory) == 1


def test_a_rebuilding_fit_keeps_no_store(tmp_path, monkeypatch):
    """Its library is different for every candidate, so no key could repeat."""
    monkeypatch.setattr(autofit, "CORPUS_ROOT", tmp_path / "scratch")
    monkeypatch.setattr(autofit, "build_shared", lambda *a, **k: None)
    path, knob = _source_knob(tmp_path, "a.cpp", "1.0")
    ev = Evaluator([knob], {path: path.read_text()}, [], None,
                   _fit_args(no_cache=False), _stub_build(tmp_path))
    ev._render_terms = lambda values: _terms(harm=1.0)
    ev([2.0])
    assert ev.disk.path is None
    assert not (tmp_path / "scratch").exists()


def test_the_notes_of_a_render_are_measured_at_once_only_when_nothing_else_is(
    tmp_path, monkeypatch,
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
    tmp_path, monkeypatch,
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

    monkeypatch.setattr(autofit.subprocess, "run", fake_run)
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


# --------------------------------------------------------------------------- #
# The tree-state precondition: whatever is in src/ at fit startup is what
# gets compiled and fit against, silently, and this tree routinely has several
# sessions editing src/ at once.
# --------------------------------------------------------------------------- #
def _init_git_repo(root: Path) -> None:
    """A scratch git repo with a tracked src/ file, for repo_tree_state's own
    git plumbing — a real `git status`/`rev-parse`, not a stand-in for one."""
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    (root / "src").mkdir()
    (root / "src" / "a.cpp").write_text("int a = 1;\n")
    subprocess.run(["git", "add", "."], cwd=root, check=True)
    subprocess.run(
        ["git", "-c", "user.email=t@t", "-c", "user.name=t",
         "commit", "-q", "-m", "init"],
        cwd=root, check=True,
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
    base = {"build_dir": "build-precondition-test"}
    base.update(kwargs)
    return argparse.Namespace(**base)


class _PastGuard(Exception):
    """Raised by a stand-in for the first call after the guard, so a test can
    tell "the guard let this through" apart from "the guard never ran"."""


def test_a_clean_tree_proceeds(monkeypatch):
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", []))
    monkeypatch.setattr(autofit, "resolve_probe",
                        lambda args: (_ for _ in ()).throw(_PastGuard()))
    with pytest.raises(_PastGuard):
        autofit.run(_precondition_args())


def test_a_dirty_tree_refuses_and_names_the_paths(monkeypatch):
    """The message carries the actual paths, not a count — the user needs to
    tell "my own edit" from "someone else's" apart, which a count cannot say."""
    dirty = ["M src/midi/synth/bowed_string_voice.cpp", "?? src/midi/synth/new.cpp"]
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", dirty))
    monkeypatch.setattr(autofit, "resolve_probe",
                        lambda args: pytest.fail("the guard let a dirty tree through"))
    with pytest.raises(RuntimeError) as exc:
        autofit.run(_precondition_args())
    assert "bowed_string_voice.cpp" in str(exc.value)
    assert "new.cpp" in str(exc.value)


def test_allow_dirty_src_proceeds_past_the_guard(monkeypatch):
    dirty = ["M src/midi/synth/bowed_string_voice.cpp"]
    monkeypatch.setattr(autofit, "repo_tree_state", lambda: ("deadbeef", dirty))
    monkeypatch.setattr(autofit, "resolve_probe",
                        lambda args: (_ for _ in ()).throw(_PastGuard()))
    with pytest.raises(_PastGuard):
        autofit.run(_precondition_args(allow_dirty_src=True))


def test_fold_tree_provenance_adds_to_an_existing_out_artifact(tmp_path):
    out = tmp_path / "result.json"
    out.write_text(json.dumps({"loss": {"best": 0.5}}))
    autofit._fold_tree_provenance(str(out), "deadbeef",
                                  ["M src/midi/synth/bowed_string_voice.cpp"])
    record = json.loads(out.read_text())
    assert record["loss"] == {"best": 0.5}  # what was already there survives
    assert record["tree"] == {"head": "deadbeef",
                              "dirty_src": ["M src/midi/synth/bowed_string_voice.cpp"]}


def test_fold_tree_provenance_is_a_noop_without_an_out_path(tmp_path):
    autofit._fold_tree_provenance("", "deadbeef", [])  # must not raise


def test_a_refusal_names_itself_in_the_out_artifact(tmp_path):
    """Unchanged values have two readings and only the record can tell them apart.

    A search that found nothing worth writing and an objective that went blind
    leave the same tree. The second is a missing measurement axis rather than a
    fit result, and it is the one worth counting later.
    """
    knobs = [_reach_knob("a", 0.0, 1.0, 0.25)]
    blind = argparse.Namespace(normalize=True, best_loss=0.5071,
                               start_tnr_notes=7.0, best_tnr_notes=0.0)
    assert winner_or_defaults(knobs, [0.9], blind) == [0.25]
    assert blind.write_back_refusal == "objective_went_blind"

    out = tmp_path / "result.json"
    out.write_text(json.dumps({"loss": {"best": 0.5071}}))
    autofit._fold_write_back_verdict(str(out), blind)
    record = json.loads(out.read_text())
    assert record["loss"] == {"best": 0.5071}  # what was already there survives
    assert record["write_back"] == {"refused": "objective_went_blind",
                                    "tnr_notes": {"start": 7.0, "best": 0.0}}


def test_each_refusal_is_distinguishable_and_a_write_names_none():
    knobs = [_reach_knob("a", 0.0, 1.0, 0.25)]
    lost = argparse.Namespace(normalize=True, best_loss=1.1536)
    winner_or_defaults(knobs, [0.9], lost)
    assert lost.write_back_refusal == "lost_to_start"

    overfit = argparse.Namespace(normalize=True, best_loss=0.8692)
    winner_or_defaults(knobs, [0.9], overfit,
                       {"axis": "velocities", "held_out": "48,88,112",
                        "start": 1.0, "best": 1.1924})
    assert overfit.write_back_refusal == "fitted_to_the_probe"

    # A run that wrote its winner must not leave a stale refusal behind it.
    won = argparse.Namespace(normalize=True, best_loss=0.5071,
                             write_back_refusal="lost_to_start")
    assert winner_or_defaults(knobs, [0.9], won) == [0.9]
    assert won.write_back_refusal is None


def test_fold_write_back_verdict_is_a_noop_without_an_out_path():
    autofit._fold_write_back_verdict("", argparse.Namespace())  # must not raise


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
        autofit, "oracle_reference",
        lambda args: ([], np.zeros(4, dtype=np.float32), None, None),
    )

    def fake_diagnosis(evaluator, knobs, args, catalogue, out_path=""):
        if out_path:
            Path(out_path).write_text(json.dumps({"terms": []}))

    monkeypatch.setattr(autofit, "run_diagnosis", fake_diagnosis)

    out = tmp_path / "diag.json"
    args = _fit_args(
        spec="ignored", program=0, drum_note=None, dump_knobs=False,
        program_only=False, bank=0, build_dir="build-precondition-test",
        screen=False, stages=False, diagnose=True, grid=0, optimizer="coord",
        metric_threads=0, out=str(out), dry_run=True, allow_dirty_src=True,
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
