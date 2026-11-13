"""The loop record: an exactly repeated cycle is told from a stable natural source."""

from __future__ import annotations

import numpy as np
import pytest
from loop_record import (
    READ_TO_JOIN_CAPTURES,
    SNAPSHOT_LABEL,
    VERDICT_LOOPED,
    VERDICT_NOT,
    VERDICT_NOT_TESTABLE,
    body_loop_lines,
    detect_loop,
    exclusions,
    loop_summary,
    read_to_join_s,
)
from loss_weights import LOOP_READ_TERMS, cli_weights, loop_unweighted_terms
from profile_measure import measure_note, reference_loop

SR = 24000
PERIOD = 288  # 12 ms
F0 = 220.0


def _cycle(seed: int = 3) -> np.ndarray:
    """One loop cycle: harmonics of a pitch that does not divide it, plus a fixed noise burst."""
    rng = np.random.default_rng(seed)
    t = np.arange(PERIOD) / SR
    cyc = sum(np.sin(2 * np.pi * F0 * k * t + k) / k for k in range(1, 8))
    return cyc + 0.05 * rng.standard_normal(PERIOD)


def _looped(seconds: float = 3.5, join_s: float = 0.3) -> np.ndarray:
    n = int(seconds * SR)
    out = np.tile(_cycle(), n // PERIOD + 1)[:n]
    rng = np.random.default_rng(9)
    head = int(join_s * SR)
    out[:head] = rng.standard_normal(head) * 0.3  # not yet repeating
    t = np.arange(n) / SR
    return out * (0.2 + 0.1 * np.exp(-t))


def _natural(seconds: float = 3.5) -> np.ndarray:
    """A steady tone with 0.2 % pitch jitter and a breath floor: stable, never sample-exact."""
    rng = np.random.default_rng(5)
    n = int(seconds * SR)
    jitter = np.cumsum(0.002 * rng.standard_normal(n)) / np.sqrt(n) * 40
    ph = (
        2
        * np.pi
        * np.cumsum(F0 * (1 + 0.002 * np.sin(np.arange(n) / SR * 5.3) + jitter * 0.01) / SR)
    )
    x = sum(np.sin(k * ph) / k for k in range(1, 8))
    return 0.2 * (x + 0.03 * rng.standard_normal(n))


def test_looped_note_reads_looped_with_its_period_and_join():
    rec = detect_loop(_looped(), SR)
    assert rec["verdict"] == VERDICT_LOOPED
    assert rec["period_ms"] == pytest.approx(12.0, abs=0.05)
    assert rec["residual_db"] < -36.0
    assert rec["join_s"] == pytest.approx(0.3, abs=0.1)


def test_stable_natural_source_is_not_a_loop():
    rec = detect_loop(_natural(), SR)
    assert rec["verdict"] == VERDICT_NOT
    assert "join_s" not in rec and "period_ms" not in rec


def test_short_note_is_not_testable():
    assert detect_loop(_looped(0.3), SR)["verdict"] == VERDICT_NOT_TESTABLE
    assert detect_loop(np.zeros(SR), SR)["verdict"] == VERDICT_NOT_TESTABLE


def test_detection_is_deterministic():
    assert detect_loop(_looped(), SR) == detect_loop(_looped(), SR)


LOOPED = {"verdict": VERDICT_LOOPED, "period_ms": 12.0, "residual_db": -90.0, "join_s": 0.05}


def test_looped_row_excludes_tnr_stretch_and_time_varying_and_keeps_a_labelled_snapshot():
    excluded, snapshot = exclusions(LOOPED)
    assert {"tnr", "stretch", "decay", "aftersound", "partial_tilt", "damper"} <= set(excluded)
    assert "residual -90.0 dB" in excluded["tnr"] and "0.05 s" in excluded["tnr"]
    assert set(snapshot) == {"balance", "centroid_pct", "register", "crest"}
    assert all(v == {"label": SNAPSHOT_LABEL} for v in snapshot.values())
    assert "body" not in excluded  # only where its sub-f0 power is the loop's lines


def test_body_is_excluded_only_where_it_sits_on_the_loop_lines():
    excluded, _ = exclusions(LOOPED, body_on_lines=True)
    assert "subharmonics at 1/period" in excluded["body"]


def test_a_dimension_read_before_the_join_is_not_excluded():
    late = {**LOOPED, "join_s": 1.4}
    excluded, snapshot = exclusions(late)
    assert "onset_settle" not in excluded and "decay" in excluded
    assert "tnr" in excluded  # its window runs to 1.62 s
    assert exclusions({**LOOPED, "join_s": 1.7})[0].get("tnr") is None


def test_unlooped_row_excludes_nothing():
    assert exclusions({"verdict": VERDICT_NOT, "residual_db": -20.0}) == ({}, {})


def test_read_to_join_applies_to_the_named_captures_past_the_minimum_join():
    rec = {**LOOPED, "join_s": 0.37}
    assert read_to_join_s(rec, "cello") == 0.37
    assert read_to_join_s(rec, "clarinet") is None
    assert read_to_join_s({**LOOPED, "join_s": 0.2}, "cello") is None
    assert read_to_join_s({"verdict": VERDICT_NOT}, "cello") is None
    assert "cello" in READ_TO_JOIN_CAPTURES


def test_read_to_join_keeps_spectral_dimensions_and_excludes_the_time_varying_ones():
    excluded, snapshot = exclusions({**LOOPED, "join_s": 0.37, "read_to_join_s": 0.37})
    assert "tnr" not in excluded and "stretch" not in excluded and not snapshot
    assert "decay" in excluded


def test_body_is_the_loops_line_structure_when_lines_fall_below_the_fundamental():
    long_loop = body_loop_lines(466.0, 200.0)  # lines every 5 Hz
    assert long_loop["on_lines"] and long_loop["lines_below_f0"] > 50
    short_loop = body_loop_lines(466.0, 2.0)  # one line at 500 Hz, above the band
    assert not short_loop["on_lines"]
    assert body_loop_lines(100.0, 200.0) is None  # f0 under the body floor


def _two_cycle_loop(seconds: float = 3.5, join_s: float = 0.1) -> np.ndarray:
    """f0 150 Hz; the loop is two periods, the second differing, so its one sub-f0 line is that."""
    rng = np.random.default_rng(11)
    period = SR // 150
    t = np.arange(period) / SR
    first = sum(np.sin(2 * np.pi * 150.0 * k * t) / k for k in range(1, 6))
    second = 0.7 * first + 0.15 * rng.standard_normal(period)
    n = int(seconds * SR)
    out = np.tile(np.concatenate([first, second]), n // (2 * period) + 1)[:n]
    head = int(join_s * SR)
    out[:head] = rng.standard_normal(head) * 0.3
    return 0.2 * out


def test_a_two_cycle_exact_loop_has_its_body_excluded():
    pre = int(0.1 * SR)
    audio = np.concatenate([np.zeros(pre), _two_cycle_loop()]).astype(np.float32)
    row = measure_note(audio, SR, 50, preroll_s=0.1, gate_s=3.0)
    rec = row["loop"]
    assert rec["verdict"] == VERDICT_LOOPED and rec["join_s"] <= 0.2
    assert rec["body_lines"]["lines_below_f0"] == 1
    assert "body" in rec["excluded"] and "subharmonics" in rec["excluded"]["body"]
    assert "body_below_f0_db" in row  # still measured; the record says why it is not read


def test_body_is_kept_when_the_loop_joins_after_its_window_starts():
    rec = {**LOOPED, "join_s": 0.5}
    assert "body" not in exclusions(rec, body_on_lines=True)[0]
    assert "body" in exclusions({**LOOPED, "join_s": 0.2}, body_on_lines=True)[0]


def test_join_inside_the_attack_excludes_the_onset_and_after_it_keeps_it():
    inside, _ = exclusions({**LOOPED, "join_s": 0.02}, attack_ms=40.0)
    assert {"attack", "arrival"} <= set(inside)
    assert (
        "onset is the envelope over the loop" in inside["attack"] and "0.02 s" in inside["attack"]
    )
    after, _ = exclusions({**LOOPED, "join_s": 0.1}, attack_ms=40.0)
    assert "attack" not in after and "arrival" not in after
    assert "attack" not in exclusions({**LOOPED, "join_s": 0.02})[0]  # no attack_ms, no claim
    assert "attack" not in exclusions({"verdict": VERDICT_NOT})[0]


def test_crest_is_labelled_a_snapshot_of_the_frozen_cycle():
    excluded, snapshot = exclusions(LOOPED)
    assert snapshot["crest"] == {"label": SNAPSHOT_LABEL} and "crest" not in excluded


def test_capture_summary_names_status_and_the_exclusions_to_write():
    row = {"note": 60, "velocity": 64, "loop": {**LOOPED, "excluded": {"tnr": "x"}, "snapshot": {}}}
    out = loop_summary([row] * 9 + [{"loop": {"verdict": VERDICT_NOT, "residual_db": -20.0}}])
    assert out["status"] == "looped" and out["looped_rows"] == 9 and out["tested"] == 10
    assert out["excluded"] == {"tnr": "x"} and out["join_s_median"] == 0.05
    assert (
        loop_summary([{"loop": {"verdict": VERDICT_NOT_TESTABLE}}])["status"]
        == VERDICT_NOT_TESTABLE
    )
    mixed = loop_summary([row, {"loop": {"verdict": VERDICT_NOT, "residual_db": -20.0}}])
    assert mixed["status"] == "mixed"


def _tone(seconds: float = 3.0) -> np.ndarray:
    n = int(seconds * SR)
    x = np.zeros(n + int(0.1 * SR))
    t = np.arange(n) / SR
    x[int(0.1 * SR) :] = 0.3 * sum(np.sin(2 * np.pi * 220.0 * k * t) / k for k in range(1, 6))
    return x.astype(np.float32)


def test_measure_note_records_the_loop_and_a_model_note_carries_the_one_it_is_given():
    ref = measure_note(_tone(), SR, 57, preroll_s=0.1, gate_s=2.5)
    assert ref["loop"]["verdict"] in (VERDICT_NOT, VERDICT_LOOPED)
    given = {"verdict": VERDICT_NOT, "residual_db": -20.0, "marker": 1}
    model = measure_note(_tone(), SR, 57, preroll_s=0.1, gate_s=2.5, loop=given)
    assert model["loop"] == given
    assert {k: v for k, v in model.items() if k != "loop"} == {
        k: v for k, v in ref.items() if k != "loop"
    }  # a row with no join is read whole


def test_a_reference_row_without_a_record_reads_as_unrecorded():
    assert reference_loop({"note": 60})["verdict"] == "unrecorded"
    assert reference_loop({"loop": {"verdict": VERDICT_NOT}})["verdict"] == VERDICT_NOT


def test_read_to_join_stops_the_spectrum_at_the_join_and_the_model_follows_it():
    audio = _tone()
    whole = measure_note(audio, SR, 57, preroll_s=0.1, gate_s=2.5)
    rec = {**LOOPED, "join_s": 0.5, "read_to_join_s": 0.5}
    joined = measure_note(audio, SR, 57, preroll_s=0.1, gate_s=2.5, loop=rec)
    assert joined["loop"] == rec
    assert joined["centroid_hz"] != whole["centroid_hz"] or joined["tnr_db"] != whole["tnr_db"]
    # the fields read outside the spectrum window are untouched
    for key in ("peak_dbfs", "rms_dbfs", "held_peak_dbfs", "attack_ms", "decay_db_s"):
        assert joined[key] == whole[key]


class _Args:
    program = 71
    percussive = False
    has_loop_free_reference = True


def test_looped_reference_takes_the_loop_read_terms_out_of_the_objective():
    args = _Args()
    free = cli_weights(args)
    assert {"cents", "tnr"} <= set(free)
    args.has_loop_free_reference = False
    weights = cli_weights(args)
    assert not set(LOOP_READ_TERMS) & set(weights)
    assert {t: w for t, w in free.items() if t not in LOOP_READ_TERMS} == weights
    assert {"cents", "tnr"} <= set(loop_unweighted_terms(args))


def test_a_named_weight_does_not_bring_a_loop_read_term_back():
    args = _Args()
    args.has_loop_free_reference = False
    args.w_tnr = 3.0
    assert "tnr" not in cli_weights(args)
    assert "tnr" in loop_unweighted_terms(args)
    args.has_loop_free_reference = True
    assert cli_weights(args)["tnr"] == 3.0 and loop_unweighted_terms(args) == []
