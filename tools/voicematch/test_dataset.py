"""Tests for the (knob vector -> measurement) corpus generator.

Nothing here renders: the parts worth pinning are the ones that decide whether
a pair is labelled with the parameters that actually produced it, and whether a
corpus reports what it failed to measure. Both are silent when wrong.
"""

from __future__ import annotations

import gzip
import json
import math
import random
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import dataset  # noqa: E402
from knobs import Knob, format_value  # noqa: E402


def _knob(label: str, lo: float, hi: float, log: bool, start: float) -> Knob:
    return Knob(label=label, lo=lo, hi=hi, log=log, start_value=start, tunable=label)


def test_a_log_knob_is_sampled_evenly_across_its_decades():
    """A magnitude knob drawn linearly spends almost every sample in its top decade.

    Three decades sampled linearly puts 90 % of the points above a tenth of the
    range, so the corpus would describe one decade and guess at the other two.
    """
    knob = _knob("k", 0.001, 1.0, True, 0.03)
    rng = random.Random(0)
    draws = [dataset.sample_values([knob], rng)[0] for _ in range(3000)]
    per_decade = [sum(1 for d in draws if 10 ** -e > d >= 10 ** -(e + 1)) for e in range(3)]
    assert all(800 < n < 1200 for n in per_decade), per_decade


def test_a_linear_knob_stays_inside_its_range():
    knob = _knob("k", -2.0, 5.0, False, 0.0)
    rng = random.Random(1)
    draws = [dataset.sample_values([knob], rng)[0] for _ in range(500)]
    assert all(-2.0 <= d <= 5.0 for d in draws)
    assert min(draws) < -1.0 and max(draws) > 4.0


def test_a_log_knob_that_reaches_zero_falls_back_to_linear():
    """log(0) is not a number, and a knob whose range includes it is common."""
    knob = _knob("k", 0.0, 1.0, True, 0.5)
    rng = random.Random(2)
    draws = [dataset.sample_values([knob], rng)[0] for _ in range(200)]
    assert all(0.0 <= d <= 1.0 and math.isfinite(d) for d in draws)


def test_the_recorded_value_survives_the_override_channel_unchanged():
    """The pair's input must be the number the audio was made from.

    Overrides travel as text at six significant figures, so a drawn value and a
    rendered one are not the same number unless the drawn one is round-tripped
    first. Recording the raw draw mislabels every sample by a little.
    """
    drawn = 0.000965242193874
    recorded = float(format_value(drawn))
    assert recorded != drawn, "the channel really does lose digits here"
    assert float(format_value(recorded)) == recorded, "and the round trip is a fixed point"


def test_a_silent_render_is_labelled_rather_than_scored():
    floored = [{"harmonics_db": [0.0] + [-120.0] * 11}]
    assert dataset._silent(floored)
    sounding = [{"harmonics_db": [0.0, -18.0] + [-120.0] * 10}]
    assert not dataset._silent(sounding)


def test_a_row_with_no_partials_at_all_counts_as_silent():
    assert dataset._silent([{"harmonics_db": [0.0]}])
    assert dataset._silent([{}])


def _write(path: Path, rows: list[dict]) -> None:
    with dataset._open(path, "w") as fh:
        fh.write(json.dumps({"schema": dataset.SCHEMA, "knobs": []}) + "\n")
        for r in rows:
            fh.write(json.dumps(r) + "\n")


def test_a_corpus_reports_how_many_samples_it_holds(tmp_path):
    path = tmp_path / "c.jsonl"
    assert dataset.existing_rows(path) == 0
    _write(path, [{"i": 0, "ok": True}, {"i": 1, "ok": True}])
    assert dataset.existing_rows(path) == 2


def test_a_run_killed_mid_write_does_not_count_its_torn_line(tmp_path):
    """Otherwise a resume skips an index that was never actually measured."""
    path = tmp_path / "c.jsonl"
    _write(path, [{"i": 0, "ok": True}])
    with open(path, "a", encoding="utf-8") as fh:
        fh.write('{"i": 1, "ok": tr')
    assert dataset.existing_rows(path) == 1


def test_a_gzipped_corpus_reads_back_as_itself(tmp_path):
    path = tmp_path / "c.jsonl.gz"
    _write(path, [{"i": 0, "ok": True}, {"i": 1, "ok": False, "why": "silent"}])
    assert dataset.existing_rows(path) == 2
    with gzip.open(path, "rt", encoding="utf-8") as fh:
        assert json.loads(fh.readlines()[2])["why"] == "silent"


def test_the_manifest_carries_the_knob_order_the_rows_depend_on():
    """Rows store bare value lists, so the order is the only thing that decodes them."""
    knobs = [_knob("a", 0.0, 1.0, False, 0.5), _knob("b", 0.1, 10.0, True, 1.0)]
    man = dataset.manifest(0, "sustain", "48,60", "", knobs, seed=3)
    assert man["schema"] == dataset.SCHEMA
    assert [k["label"] for k in man["knobs"]] == ["a", "b"]
    assert man["knobs"][1] == {"label": "b", "tunable": "b", "lo": 0.1, "hi": 10.0,
                               "log": True, "default": 1.0}


@pytest.mark.parametrize("suffix", [".jsonl", ".jsonl.gz"])
def test_an_absent_corpus_is_not_an_error(tmp_path, suffix):
    assert dataset.existing_rows(tmp_path / f"missing{suffix}") == 0
