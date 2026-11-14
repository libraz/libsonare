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
    assign_groups,
    body_loop_lines,
    detect_loop,
    exclusions,
    independence_line,
    independent_count,
    ladder_db,
    ladder_distance,
    loop_summary,
    mark_loop_rows,
    note_groups,
    read_to_join_s,
    unweighted_terms,
)
from loss import loss_terms
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


def test_reasons_say_the_period_in_pitch_periods_and_what_periodising_did_to_the_control():
    rec = {**LOOPED, "period_pitch_periods": 31.4}
    excluded, _ = exclusions(rec, body_on_lines=True)
    assert "12.0 ms = 31.4 pitch periods" in excluded["tnr"]
    assert "tnr -11..-14 dB" in excluded["tnr"] and "body +2..+7 dB" in excluded["body"]
    assert "pitch periods" not in exclusions(LOOPED)[0]["tnr"]


def test_body_is_excluded_only_where_it_sits_on_the_loop_lines():
    excluded, _ = exclusions(LOOPED, body_on_lines=True)
    assert "own lines at 1/period" in excluded["body"]


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
    assert "body" in rec["excluded"] and "own lines" in rec["excluded"]["body"]
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
    assert out["not_testable_rows"] == 0 and out["unweighted_rows"] == 9
    assert out["excluded"] == {"tnr": "x"} and out["join_s_median"] == 0.05
    assert out["snapshot"]["register"]["independent_readings"] == 9  # no groups assigned yet
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


def _ladder(slope: float, ripple: float = 0.0) -> list:
    return [0.0] + [round(slope * h + (ripple if h % 2 else -ripple), 1) for h in range(2, 21)]


def _grouped_rows(table: dict[int, list]) -> list[dict]:
    rows = []
    for note, ladder in table.items():
        for vel in (40, 100):
            shifted = [
                None if v is None else round(v - (3.0 if vel == 40 and i else 0.0), 1)
                for i, v in enumerate(ladder)
            ]
            rows.append(
                {
                    "timbre": "t",
                    "note": note,
                    "velocity": vel,
                    "loop": {
                        **LOOPED,
                        "ladder_db": shifted,
                        "excluded": {},
                        "snapshot": {
                            d: {"label": SNAPSHOT_LABEL}
                            for d in ("balance", "centroid_pct", "register")
                        },
                    },
                    "held_peak_dbfs": -20.0,
                    "partials_db": [0.0],
                    "centroid_hz": 500.0,
                }
            )
    return rows


def test_notes_with_matching_ladders_share_a_group_and_the_rest_stand_alone():
    rows = _grouped_rows(
        {50: _ladder(-3.0), 54: _ladder(-3.1), 58: _ladder(-3.2), 62: _ladder(-6.0, 4.0)}
    )
    assign_groups(rows)
    assert note_groups(rows) == {50: 0, 54: 0, 58: 0, 62: 1}
    first = next(r for r in rows if r["note"] == 54)["loop"]
    assert first["group_notes"] == [50, 54, 58]
    assert first["snapshot"]["balance"]["group"] == 0
    assert "register" in first["flagged"] and "transposition" in first["flagged"]["register"]
    alone = next(r for r in rows if r["note"] == 62)["loop"]
    assert alone["group_notes"] == [62] and "flagged" not in alone


def test_unlooped_rows_are_grouped_by_their_ladders_too():
    rows = _grouped_rows({50: _ladder(-3.0), 54: _ladder(-3.0), 62: _ladder(-6.0, 4.0)})
    for r in rows:
        if r["note"] in (54, 62):
            r["loop"] = {
                "verdict": VERDICT_NOT,
                "residual_db": -20.0,
                "ladder_db": r["loop"]["ladder_db"],
            }
    assign_groups(rows)
    assert note_groups(rows) == {50: 0, 54: 0, 62: 1}
    rec = next(r for r in rows if r["note"] == 54)["loop"]
    assert rec["verdict"] == VERDICT_NOT and rec["group_notes"] == [50, 54]
    assert "register" in rec["flagged"]


def test_a_row_without_a_ladder_gets_no_group():
    rows = _grouped_rows({50: _ladder(-3.0)})
    rows.append({"timbre": "t", "note": 70, "velocity": 40, "loop": {"verdict": VERDICT_NOT}})
    assign_groups(rows)
    assert note_groups(rows) == {50: 0}


def test_a_note_with_a_neighbour_between_4_and_8_db_is_uncertain_with_both_readings():
    near = _ladder(-3.0 - 0.38)  # mean |dB| 4.2 over h2..h20, inside the cut
    rows = _grouped_rows({50: _ladder(-3.0), 54: near, 62: _ladder(-3.0 - 2.0)})
    assign_groups(rows)
    rec = next(r for r in rows if r["note"] == 54)["loop"]
    assert rec["group"] == 0 and 50 in rec["group_uncertain"]["ambiguous_with"]
    assert rec["group_uncertain"]["loosest_inside_db"] >= 4.0
    assert rec["group_uncertain"]["nearest_outside_db"] > 8.0
    assert rec["group_uncertain"]["band_db"] == [4.0, 8.0]


def test_ladder_distance_ignores_harmonics_missing_or_below_the_floor():
    a, b = (
        [0.0, -5.0, -10.0, None, -90.0, -20.0, -30.0],
        [0.0, -6.0, -12.0, -10.0, -95.0, -20.0, -31.0],
    )
    assert ladder_distance(a, b) == pytest.approx((1 + 2 + 0 + 1) / 4)


def test_ladder_reads_harmonic_bands_re_the_first():
    freqs = np.arange(0, 5000.0, 1.0)
    power = np.zeros_like(freqs)
    power[220] = 1.0
    power[440] = 0.1
    ladder = ladder_db(freqs, power, 220.0, 24000)
    assert ladder[0] == 0.0 and ladder[1] == pytest.approx(-10.0, abs=0.1) and ladder[2] is None


def test_independent_count_is_the_number_of_groups_not_rows():
    rows = _grouped_rows({50: _ladder(-3.0), 54: _ladder(-3.0), 62: _ladder(-6.0, 4.0)})
    assign_groups(rows)
    assert len(rows) == 6
    assert independent_count(rows, "register") == 2
    assert independence_line(rows, ("register", "balance")) == "register 2 of 6, balance 2 of 6"
    rows[0]["loop"]["excluded"] = {"register": "x"}
    assert independent_count(rows[:1], "register") == 0
    plain = [{"note": 60, "velocity": 64, "held_peak_dbfs": -20.0}] * 3
    assert (
        independent_count(plain, "register") == 3
        and independence_line(plain, ("register",)) is None
    )


def test_refit_holds_out_whole_groups_and_keeps_the_plain_alternation_without_them():
    from refit import _split

    notes = [50, 54, 58, 62, 66, 70, 74, 78, 82, 86]
    groups = {50: 0, 54: 0, 58: 0, 62: 1, 66: 1, 70: 2, 74: 2, 78: 3, 82: 3, 86: 3}
    train, held = _split(notes, groups)
    assert train == [50, 54, 58, 70, 74] and held == [62, 66, 78, 82, 86]
    assert not {groups[n] for n in train} & {groups[n] for n in held}
    assert _split(notes) == (notes[0::2], notes[1::2])
    assert _split([50, 60, 70], {50: 0}) == ([50, 70], [60])  # ungrouped notes stand alone


def test_a_held_out_note_sharing_a_group_with_a_fitted_note_is_dropped_with_its_reason(
    monkeypatch, capsys
):
    import autofit

    monkeypatch.setattr(autofit, "committed_note_groups", lambda *_: {50: 0, 54: 0, 62: 1})
    corpus = type("C", (), {"capture_id": "x", "timbre": "t", "groups": {}})()
    monkeypatch.setattr(autofit, "_score", lambda *a, **k: (None, None, None))
    args = type("A", (), {})()
    args.percussive, args.program, args.pattern = False, 71, "corpus"
    args.notes, args.validate_notes = "50", "54,62"
    args.velocities = ""
    pattern = type("P", (), {"notes": [], "analysis_notes": []})()
    monkeypatch.setattr(autofit, "_score", lambda *a, **k: (pattern, None, None))
    out = autofit.check_validation_partition(args, corpus)
    assert (
        out["status"] == "independent" and out["held_out"] == [62] and out["dropped"] == {54: [50]}
    )
    assert args.validate_notes == "62"
    assert "held-out note 54 dropped" in capsys.readouterr().err
    args.validate_notes = "54"
    gone = autofit.check_validation_partition(args, corpus)
    assert gone["status"] == "independent_missing" and "stored waveform" in gone["reason"]


def test_summary_counts_snapshot_readings_as_groups_not_rows():
    rows = _grouped_rows({50: _ladder(-3.0), 54: _ladder(-3.0), 62: _ladder(-6.0, 4.0)})
    assign_groups(rows)
    out = loop_summary(rows)
    assert out["groups"] == {"0": [50, 54], "1": [62]}
    assert out["snapshot"]["crest"]["independent_readings"] == 2


def _long_loop(period_s: float, join_s: float, seconds: float = 3.0) -> np.ndarray:
    rng = np.random.default_rng(21)
    n = int(seconds * SR)
    cycle = sum(
        np.sin(2 * np.pi * 220.0 * k * np.arange(int(period_s * SR)) / SR + k) / k
        for k in range(1, 6)
    )
    cycle = cycle + 0.05 * rng.standard_normal(len(cycle))
    out = np.tile(cycle, n // len(cycle) + 1)[:n]
    head = int(join_s * SR)
    out[:head] = rng.standard_normal(head) * 0.3
    return 0.2 * out


def test_a_463_ms_loop_in_a_three_second_note_is_looped_despite_frames_that_cannot_reach_it():
    """accordion n053: -64 dB at its best frame, but only three frames reach a 463 ms lag."""
    rec = detect_loop(_long_loop(0.463, 0.5), SR)
    assert rec["verdict"] == VERDICT_LOOPED and rec["reaching_frames"] == 3
    assert rec["period_ms"] == pytest.approx(463.0, abs=1.0)


def test_one_reaching_frame_that_fails_cannot_call_the_row_not_looped():
    rec = detect_loop(_long_loop(0.6, 0.5), SR)  # the frames that reach straddle the join
    assert rec["verdict"] == VERDICT_NOT_TESTABLE and "cannot test" in rec["reason"]


def test_one_reaching_frame_that_passes_is_enough_to_call_it_looped():
    rec = detect_loop(_long_loop(0.7, 0.2), SR)
    assert rec["verdict"] == VERDICT_LOOPED and rec["reaching_frames"] == 1


def test_a_stable_natural_source_stays_not_with_the_reaching_median():
    assert detect_loop(_natural(), SR)["verdict"] == VERDICT_NOT


def test_a_row_loses_the_terms_its_own_exclusions_name():
    assert unweighted_terms({"excluded": {"stretch": "x", "tnr": "x", "decay": "x"}}) == [
        "cents",
        "tnr",
        "slope",
        "mod",
    ]
    assert unweighted_terms({"excluded": {"damper": "x"}}) == []
    assert unweighted_terms(None) == []


def _loss_row(note: int, cents: float, tnr: float, **extra) -> dict:
    from test_loss_sidedness import BASE, _row

    row = _row(BASE, note, 64)
    row.update(f0_cents_err=cents, tnr_db=tnr, **extra)
    return row


def test_the_loss_skips_the_terms_on_a_looped_row_and_keeps_them_on_an_unlooped_one():
    model = [_loss_row(60, 0.0, 30.0), _loss_row(64, 0.0, 30.0)]
    oracle = [_loss_row(60, 7.0, 45.0), _loss_row(64, 7.0, 45.0)]
    both = loss_terms(model, oracle)
    oracle[0]["loop_unweighted"] = ["cents", "tnr"]
    one = loss_terms(model, oracle)
    assert both["cents"] == pytest.approx(7.0) and one["cents"] == pytest.approx(3.5)
    assert both["tnr"] == pytest.approx(15.0) and one["tnr"] == pytest.approx(7.5)
    for o in oracle:
        o["loop_unweighted"] = ["cents", "tnr"]
    none = loss_terms(model, oracle)
    assert none["cents"] == 0.0 and none["tnr"] == 0.0 and none["harm"] == both["harm"]


def test_mark_loop_rows_marks_only_the_rows_whose_reference_loops(monkeypatch):
    import loop_record

    monkeypatch.setattr(
        loop_record,
        "committed_loop_cells",
        lambda *_: {(60, 64): ["cents", "tnr"], (64, 64): []},
    )
    rows = [
        {"note": 60, "velocity": 64},
        {"note": 64, "velocity": 64},
        {"note": 70, "velocity": 64},
    ]
    assert mark_loop_rows(rows, "x", "t") == 1
    assert rows[0]["loop_unweighted"] == ["cents", "tnr"] and "loop_unweighted" not in rows[1]


def test_a_weak_loop_must_repeat_at_one_period_in_two_frames_and_a_strong_one_need_not():
    import loop_record as lr

    def frames(periods, r_long):
        return [
            {"L": p, "r": r_long, "r_long": r_long, "l_long": 3000.0, "lmax": 90000, "t": 9600}
            for p in periods
        ]

    sr = SR
    x = np.zeros(3 * sr) + 0.1
    for r, periods, verdict in (
        (0.99989, (5000.0, 5020.0, 7777.0), VERDICT_LOOPED),  # -36.5 dB, two agree within 1 %
        (0.99989, (5000.0, 6100.0, 7777.0), VERDICT_NOT),  # -36.5 dB, none agree
        (0.99999, (5000.0, 6100.0, 7777.0), VERDICT_LOOPED),  # -47 dB, unaffected
    ):
        seq = iter(frames(periods, r))
        orig = lr._frame
        lr._frame = lambda *a, seq=seq, **k: next(seq, None)
        try:
            rec = detect_loop(x + 0.01 * np.random.default_rng(1).standard_normal(x.size), sr)
        finally:
            lr._frame = orig
        assert rec["verdict"] == verdict, (r, periods, rec)
        if verdict == VERDICT_NOT:
            assert rec["borderline"] and rec["period_agreeing_frames"] == 1


def test_the_open_seam_question_is_in_every_tnr_and_body_reason():
    excluded, _ = exclusions({**LOOPED, "period_pitch_periods": 31.0}, body_on_lines=True)
    for dim in ("tnr", "body"):
        assert "open: a seam statistic did not separate" in excluded[dim]


def test_a_body_at_the_recording_floor_keeps_the_censors_reason_first():
    excluded, _ = exclusions(
        LOOPED, body_on_lines=True, body_floor="reference band at its recorded floor, flat"
    )
    assert excluded["body"].startswith(
        "reference band at its recorded floor, flat (body-band censor)"
    )
    assert "loop lines also recur" in excluded["body"]
