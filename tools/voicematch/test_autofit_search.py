"""The search space: which interval a knob is searched over, which stage it
belongs to, the weights a spec carries, and when a run stops.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_autofit_search.py -q
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import json

import autofit
import voicematch
from autofit_test_fixtures import (
    _bounded_knob,
    _knob,
    _probe_args,
)
from catalogue import Catalogue
from knobs import (
    _auto_range,
    format_value,
    load_spec,
    load_spec_weights,
    tunable_overrides,
)
from optimizers import cma_es, optimize
from staging import stage_of, staged_indices


# --------------------------------------------------------------------------- #
# Search ranges from the library's own clamp bounds
# --------------------------------------------------------------------------- #
def test_unit_bound_is_searched_end_to_end():
    """A normalized field's whole interval is the range, zero and one included."""
    assert _auto_range("violin.bowed_string.bow_force", 0.55, (0.0, 1.0)) == (0.0, 1.0, False)


def test_small_physical_bound_is_searched_end_to_end():
    """A bow position at 0.02..0.5 is small enough to search whole."""
    assert _auto_range("violin.bowed_string.bow_position", 0.12, (0.02, 0.5)) == (0.02, 0.5, False)


def test_wide_bound_becomes_a_log_window_around_the_default():
    """An envelope time's clamp spans four decades; the default anchors the window."""
    lo, hi, log = _auto_range("violin.amp_env.release_ms", 110.0, (1.0, 20000.0))
    assert log is True
    assert lo == pytest.approx(110.0 / 8.0)
    assert hi == pytest.approx(110.0 * 8.0)


def test_wide_window_is_capped_by_the_bound():
    """The window never leaves the interval the engine accepts."""
    lo, hi, _ = _auto_range("x.attack_ms", 4.0, (1.0, 20.0))
    assert lo == pytest.approx(1.0)
    assert hi == pytest.approx(20.0)


def test_wide_bound_with_a_zero_default_is_declined():
    """No magnitude to anchor on, and guessing one is what bounds replace."""
    assert _auto_range("x.percussion.base_freq_hz", 0.0, (0.0, 20000.0)) is None


def test_unbounded_knob_falls_back_to_the_name_heuristic():
    """An engine calibration constant has no clamp anywhere; the old rules apply."""
    lo, hi, log = _auto_range("bowed_string_voice.kSomeTime_ms", 12.0, None)
    assert (lo, hi, log) == (6.0, 24.0, True)


def test_structural_fields_are_never_auto_fitted():
    assert _auto_range("violin.glide_ms", 0.0, (0.0, 5000.0)) is None
    assert _auto_range("violin.amp_env.hold_ms", 0.0, (0.0, 5000.0)) is None


def test_catalogue_looks_a_bound_up_by_field_not_by_patch():
    """One bound serves every patch carrying the field, so the prefix is dropped."""
    cat = Catalogue({}, {}, {"bowed_string.bow_force": (0.0, 1.0)})
    assert cat.bound_for("violin.bowed_string.bow_force") == (0.0, 1.0)
    assert cat.bound_for("cello.bowed_string.bow_force") == (0.0, 1.0)
    assert cat.bound_for("cello.bowed_string.stribeck") is None


# --------------------------------------------------------------------------- #
# Stage classification
# --------------------------------------------------------------------------- #
def test_stages_split_excitation_from_decay():
    knobs = [
        _knob("violin.bowed_string.bow_force"),
        _knob("violin.bowed_string.attack_ms"),
        _knob("violin.bowed_string.damping"),
        _knob("violin.amp_env.release_ms"),
        _knob("violin.cutoff_hz"),
    ]
    assert staged_indices(knobs, "excitation") == [0, 1]
    assert staged_indices(knobs, "decay") == [2, 3]


def test_a_section_name_does_not_classify_its_fields():
    """`bowed_string` contains "ring"; the field is what decides, not the section."""
    assert stage_of("violin.bowed_string.bow_force") == "excitation"
    assert stage_of("guitar.plucked_string.brightness") is None


def test_an_excitation_transient_is_not_a_loop_decay():
    """`click_decay_ms` is how fast the key click dies, not how fast the tone does."""
    assert stage_of("organ.additive.click_decay_ms") == "excitation"
    assert stage_of("organ.pipe_organ.tone_decay_s") == "decay"


def test_an_unclassified_knob_is_still_fitted_in_the_final_stage():
    """Only the final stage's membership matters for reach, and it takes everything."""
    knobs = [_knob("violin.cutoff_hz")]
    assert staged_indices(knobs, "excitation") == []
    assert staged_indices(knobs, "decay") == []


# --------------------------------------------------------------------------- #
# Reporting
# --------------------------------------------------------------------------- #
def test_only_the_knobs_that_moved_reach_the_override_string():
    knobs = [_knob("a.b"), _knob("c.d")]
    assert tunable_overrides(knobs, [0.5, 0.9], changed_only=True) == "c.d=0.9"
    assert tunable_overrides(knobs, [0.5, 0.9]) == "a.b=0.5,c.d=0.9"


def test_a_written_literal_always_carries_a_decimal_point():
    assert format_value(22.0) == "22.0"
    assert format_value(0.5) == "0.5"


# --------------------------------------------------------------------------- #
# Sweeping a knob the library reads
# --------------------------------------------------------------------------- #
def test_room_match_refuses_a_library_that_ignores_the_override(monkeypatch):
    """Without BUILD_TUNING the decay axis is inert and every render is identical."""
    monkeypatch.setattr(
        voicematch, "dump_catalogue", lambda *a, **k: Catalogue({"gs_effects.kOther": 1.0}, {}, {})
    )

    def refuse(*args, **kwargs):
        raise AssertionError("run_room_match rendered before checking the override table")

    monkeypatch.setattr(voicematch, "obtain_oracle", refuse)
    with pytest.raises(SystemExit, match="kReverbDecayScale"):
        voicematch.run_room_match(
            argparse.Namespace(programs="19", pattern="sustain", verbose=False)
        )


def test_room_match_reports_a_library_built_without_the_table(monkeypatch):
    def no_dump(*args, **kwargs):
        raise RuntimeError("the render produced no knob dump")

    monkeypatch.setattr(voicematch, "dump_catalogue", no_dump)
    with pytest.raises(SystemExit, match="BUILD_TUNING=ON"):
        voicematch.require_live_tunable(19, "sustain", voicematch.DECAY_SCALE_KEY)


def test_room_match_proceeds_when_the_library_reports_the_key(monkeypatch):
    monkeypatch.setattr(
        voicematch,
        "dump_catalogue",
        lambda *a, **k: Catalogue({voicematch.DECAY_SCALE_KEY: 1.0}, {}, {}),
    )
    voicematch.require_live_tunable(19, "sustain", voicematch.DECAY_SCALE_KEY)


# --------------------------------------------------------------------------- #
# Termination
# --------------------------------------------------------------------------- #
class _CacheOnlyEvaluator:
    """Every point it is asked about has already been rendered.

    Which is two different states, and the search has to end in both: a
    converged run whose samples have collapsed onto points it already scored,
    and a run started against a store an earlier one filled, where nothing is
    rendered at all and yet every candidate is new to this search.

    `trajectory` gets one entry per distinct candidate the way `Evaluator` does
    — keyed through `format_value`, so two samples that round together are one
    point — while nothing here renders.
    """

    def __init__(self, start: list[float], limit: int = 400):
        self.trajectory: list[tuple[float, float, str]] = []
        self.best_loss = 1.0
        self.best_values = list(start)
        self.workers = 1
        self.quiet = False
        self.calls = 0
        self.limit = limit
        self.seen: set[tuple[str, ...]] = set()

    def __call__(self, values) -> float:
        self.calls += 1
        if self.calls > self.limit:
            raise AssertionError(
                f"the search did not terminate after {self.calls} evaluations, "
                f"none of which rendered anything"
            )
        key = tuple(format_value(v) for v in values)
        if key not in self.seen:
            self.seen.add(key)
            self.trajectory.append((1.0, 1.0, "fit"))
        return 1.0

    def evaluate_batch(self, batch) -> list[float]:
        return [self(values) for values in batch]


def _optimizer_args(**kwargs) -> argparse.Namespace:
    base = {
        "max_evals": 30,
        "per_knob_evals": 6,
        "population": 6,
        "sigma0": 0.25,
        "seed": 0,
        "restarts": 0,
    }
    base.update(kwargs)
    return argparse.Namespace(**base)


def test_cma_es_terminates_when_every_candidate_is_a_cache_hit():
    knobs = [_knob("a.b"), _knob("c.d")]
    evaluator = _CacheOnlyEvaluator([k.start_value for k in knobs])
    assert cma_es(evaluator, knobs, _optimizer_args()) == [0.5, 0.5]


def test_coordinate_descent_terminates_when_every_candidate_is_a_cache_hit():
    knobs = [_knob("a.b"), _knob("c.d")]
    evaluator = _CacheOnlyEvaluator([k.start_value for k in knobs])
    assert optimize(evaluator, knobs, _optimizer_args()) == [0.5, 0.5]


class _CountingEvaluator:
    """Scores a fixed objective, counting what it rendered apart from what it saw.

    `warm` names the candidates an earlier run already measured, which this one
    therefore answers without rendering — the shape a store on disk gives the
    real `Evaluator`. Everything else about the search is identical, which is the
    point: the two runs below differ in nothing but how much they had to render.
    """

    def __init__(self, objective, warm: set[tuple[str, ...]] | None = None):
        self.objective = objective
        self.warm = set(warm or ())
        self.trajectory: list[tuple[float, float, str]] = []
        self.seen: set[tuple[str, ...]] = set()
        self.order: list[tuple[str, ...]] = []
        self.renders: list[tuple[str, ...]] = []
        self.best_loss = float("inf")
        self.best_values: list[float] | None = None
        self.workers = 1
        self.quiet = False

    def __call__(self, values) -> float:
        key = tuple(format_value(v) for v in values)
        if key not in self.seen:
            self.seen.add(key)
            self.order.append(key)
            self.trajectory.append((self.best_loss, 0.0, "fit"))
            if key not in self.warm:
                self.renders.append(key)
        loss = self.objective(values)
        if loss < self.best_loss:
            self.best_loss, self.best_values = loss, list(values)
        return loss

    def evaluate_batch(self, batch) -> list[float]:
        return [self(values) for values in batch]


def _bowl(values) -> float:
    return sum((v - 0.31) ** 2 for v in values)


@pytest.mark.parametrize("optimizer", [optimize, cma_es])
def test_a_run_that_renders_nothing_searches_exactly_as_far_as_one_that_renders(
    optimizer,
):
    """The store may make a search cheap. It may not make it shorter.

    Both loops used to stop when a round rendered nothing, and to spend
    `--max-evals` on renders. Against a store an earlier run filled that reads as
    "converged" on the first pass, so the second run of the same fit would hand
    back whichever point pass one liked and never look at the rest — a worse
    answer, arrived at faster, with nothing in the output saying so.
    """
    knobs = [_knob("a.b"), _knob("c.d")]
    cold = _CountingEvaluator(_bowl)
    optimizer(cold, knobs, _optimizer_args())
    assert cold.renders, "the cold run rendered nothing, so it proves nothing"

    warm = _CountingEvaluator(_bowl, warm=set(cold.order))
    optimizer(warm, knobs, _optimizer_args())
    assert warm.renders == []
    assert warm.order == cold.order
    assert warm.best_values == cold.best_values


# --------------------------------------------------------------------------- #
# The range a search was allowed to visit
# --------------------------------------------------------------------------- #
def test_a_result_on_a_range_bound_is_named_rather_than_reported_as_an_optimum():
    """The most expensive failure this tool has, because nothing else looks wrong."""
    knobs = [
        _bounded_knob("kTrebleDecayOct", 0.5, 3.0, 1.9),
        _bounded_knob("kOther", 0.0, 1.0, 0.5),
    ]
    pinned = autofit.report_pinned(knobs, [3.0, 0.5])
    assert len(pinned) == 1
    assert "kTrebleDecayOct" in pinned[0] and "maximum" in pinned[0]
    assert autofit.report_pinned(knobs, [1.9, 0.0])[0].endswith("at its minimum (0)")
    assert autofit.report_pinned(knobs, [1.9, 0.5]) == []


# --------------------------------------------------------------------------- #
# A spec that carries the weights its knobs answer to
# --------------------------------------------------------------------------- #
def _spec_file(tmp_path: Path, body) -> Path:
    path = tmp_path / "spec.json"
    path.write_text(json.dumps(body))
    return path


def test_a_bare_array_spec_still_loads(tmp_path):
    entry = {"tunable": "kX", "min": 0.0, "max": 1.0}
    path = _spec_file(tmp_path, [entry])
    assert load_spec(path) == [entry]
    assert load_spec_weights(path) == {}


def test_a_spec_can_carry_the_weights_its_knobs_answer_to(tmp_path):
    entry = {"tunable": "kX", "min": 0.0, "max": 1.0}
    path = _spec_file(tmp_path, {"weights": {"tail": 2.0, "crest": 2.0}, "knobs": [entry]})
    assert load_spec(path) == [entry]
    assert load_spec_weights(path) == {"tail": 2.0, "crest": 2.0}


def test_spec_weights_apply_but_never_over_an_explicit_flag(tmp_path):
    path = _spec_file(
        tmp_path,
        {
            "weights": {"tail": 2.0, "crest": 3.0},
            "knobs": [{"tunable": "kX", "min": 0.0, "max": 1.0}],
        },
    )
    # argparse has already put the flag's value on the namespace by this point;
    # what apply_spec_weights decides is whether the spec is allowed to replace it.
    args = _probe_args(spec=str(path), w_crest=0.5)
    autofit.apply_spec_weights(args, ["--w-crest", "0.5"])
    assert args.w_tail == 2.0
    assert args.w_crest == 0.5


def test_a_spec_naming_a_term_that_does_not_exist_is_refused(tmp_path):
    path = _spec_file(
        tmp_path,
        {"weights": {"loudness": 1.0}, "knobs": [{"tunable": "kX", "min": 0.0, "max": 1.0}]},
    )
    with pytest.raises(ValueError, match="not a loss term"):
        autofit.apply_spec_weights(_probe_args(spec=str(path)), [])
