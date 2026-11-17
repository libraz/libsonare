"""Every gated dimension's own term reads its notes through the gate's own reader."""

from __future__ import annotations

import metrics
import metrics_hit
import numpy as np
import profile_measure
import pytest
from loss import (
    OWN_TERM_READERS,
    PERCUSSION_OWN_TERM_READERS,
    loss_terms,
    oracle_tnr_masks,
    percussion_terms,
    probe_rows,
)
from loss_weights import GATE_OWN_TERMS, PERCUSSION_GATE_OWN_TERMS
from patterns import analysis_window_end, build_pattern
from profile_measure import attack_hf_share_of, measure_hit, measure_note
from profile_percussion import percussion_row_deltas

SR = 48000
#: Modules a gate reader may come from: the gate's own, and the metrics it reads a note with.
GATE_MODULES = (profile_measure, metrics, metrics_hit)
#: Where an own term's reading must not be defined.
LOSS_MODULES = ("loss", "loss_dimensions", "loss_kit", "loss_weights", "loss_aggregate")


def _struck(
    n: int, f0: float, *, bright: float = 3.0, seed: int = 0, vibrato: float = 0.0
) -> np.ndarray:
    """A slightly noisy harmonic note of `n` samples that overshoots its onset and then holds.

    `vibrato` is a 5.5 Hz pitch motion's depth, as a fraction of f0.
    """
    rng = np.random.default_rng(seed)
    t = np.arange(n) / SR
    phase = 2 * np.pi * np.cumsum(f0 * (1.0 + vibrato * np.sin(2 * np.pi * 5.5 * t))) / SR
    y = sum(
        10 ** (-bright * (k - 1) / 20.0) * np.sin(k * phase)
        for k in range(1, 16)
        if f0 * k < 0.45 * SR
    )
    click = 0.3 * rng.standard_normal(n) * np.exp(-t * 400.0)
    held = 1.0 + 0.6 * np.exp(-t * 40.0)
    return 0.2 * (y * held + click + 2e-3 * rng.standard_normal(n))


def _hit(n: int, *, bright: float, seed: int) -> np.ndarray:
    """A drum-like hit: a pitched mode under a decaying noise burst."""
    rng = np.random.default_rng(seed)
    t = np.arange(n) / SR
    noise = rng.standard_normal(n) * np.exp(-t * 30.0)
    tone = np.sin(2 * np.pi * 180.0 * t) * np.exp(-t * 8.0)
    return 0.3 * (tone + bright * np.diff(noise, prepend=0.0) + 0.2 * noise)


def _pitched(bright: float = 3.0, seed: int = 0, vibrato: float = 0.004, oracle=None):
    """One sustained note in its probe, measured the loss's way and the gate's way.

    With `oracle`, a (row, gate row) pair, the note is the model's: the loss reads it through
    the oracle row's masks, the gate through the oracle gate row's `tnr_mask`.
    """
    pattern = build_pattern("sustain", 0, notes=[57])
    (note,) = pattern.analysis_notes
    y = np.zeros(int(analysis_window_end(pattern, note) * SR))
    at = int(note.start * SR)
    y[at:] = _struck(len(y) - at, 220.0, bright=bright, seed=seed, vibrato=vibrato)
    masks = None if oracle is None else oracle_tnr_masks([oracle[0]])
    (row,) = probe_rows(y, pattern, SR, raw=y, tnr_masks=masks)
    gate = measure_note(
        y,
        SR,
        note.note,
        preroll_s=note.start,
        gate_s=note.dur,
        tnr_mask=None if oracle is None else oracle[1]["tnr_mask"],
    )
    return row, gate


def _percussive(bright: float, seed: int):
    pattern = build_pattern("drum", 0, notes=[38], velocities=(100,))
    (note,) = pattern.analysis_notes
    y = np.zeros(int(analysis_window_end(pattern, note) * SR))
    at = int(note.start * SR)
    y[at:] = _hit(len(y) - at, bright=bright, seed=seed)
    (row,) = probe_rows(y, pattern, SR, raw=y)
    gate = measure_hit(y, SR, note.note, note.velocity, preroll_s=note.start, gate_s=note.dur)
    return row, gate


def _share(row: dict) -> float | None:
    return attack_hf_share_of(row.get("attack_hf_db"))


#: Per pitched own term: its per-note reading off a probe row, and the gate row's field(s).
PITCHED_READINGS = {
    "cents": (lambda r: r["cents_vs_et"], ("cents_vs_et",)),
    "tnr": (lambda r: r["fit_tnr_db"], ("tnr_db",)),
    "bright": (lambda r: r["fit_centroid_hz"], ("centroid_hz",)),
    "velspan": (lambda r: r["mono_peak_dbfs"], ("peak_dbfs",)),
    "level": (lambda r: r["held_peak_dbfs"], ("held_peak_dbfs",)),
    "onset": (
        lambda r: (r["onset_overshoot_db"], r["onset_settle_ms"]),
        ("onset_overshoot_db", "onset_settle_ms"),
    ),
    "hfshare": (_share, ("attack_hf_db",)),
    "hfdyn": (_share, ("attack_hf_db",)),
    "body": (lambda r: r["body_below_f0_db"], ("body_below_f0_db",)),
}
#: The same for the percussion set's own terms read off one hit.
PERCUSSION_READINGS = {
    "velspan": ("mono_peak_dbfs", "peak_dbfs"),
    "crest": ("crest_db", "crest_db"),
    "bright": ("centroid_hz", "centroid_hz"),
    "tonal": ("flatness_db", "flatness_db"),
    "rise": ("tilt_rise_db", "tilt_rise_db"),
}


def _own(table: dict[str, tuple[str, ...]]) -> list[str]:
    return sorted({t for terms in table.values() for t in terms})


def test_every_own_term_has_an_equality_case():
    assert sorted(PITCHED_READINGS) == _own(GATE_OWN_TERMS)
    assert sorted({*PERCUSSION_READINGS, "bandrms"}) == _own(PERCUSSION_GATE_OWN_TERMS)


@pytest.mark.parametrize(
    "readers,own",
    [(OWN_TERM_READERS, GATE_OWN_TERMS), (PERCUSSION_OWN_TERM_READERS, PERCUSSION_GATE_OWN_TERMS)],
    ids=["pitched", "percussion"],
)
def test_no_own_term_has_a_private_reader(readers, own):
    """Each own term's reader is a gate module's function, not one the loss defines."""
    assert sorted(readers) == _own(own)
    for term, reader in readers.items():
        assert reader.__module__ not in LOSS_MODULES, term
        assert any(getattr(m, reader.__name__, None) is reader for m in GATE_MODULES), term


@pytest.fixture(scope="module")
def pitched():
    return _pitched()


@pytest.mark.parametrize("term", sorted(PITCHED_READINGS))
def test_a_pitched_own_term_reads_what_the_gate_reads(pitched, term):
    row, gate = pitched
    read, fields = PITCHED_READINGS[term]
    want = tuple(gate[f] for f in fields)
    got = read(row)
    assert (got if isinstance(got, tuple) else (got,)) == want


@pytest.fixture(scope="module")
def hits():
    return _percussive(1.0, 1), _percussive(0.3, 2)


@pytest.mark.parametrize("term", sorted(PERCUSSION_READINGS))
def test_a_percussion_own_term_reads_what_the_gate_reads(hits, term):
    (row, gate), _ = hits
    loss_field, gate_field = PERCUSSION_READINGS[term]
    assert row[loss_field] == gate[gate_field]


def test_the_band_shape_own_term_is_the_gates_band_shape(hits):
    (model, model_gate), (ref, ref_gate) = hits
    terms = percussion_terms([model], [ref])
    want = percussion_row_deltas(model_gate, ref_gate)["band_shape"]
    assert terms["bandrms"] == pytest.approx(want, abs=1e-12) and terms["bandrms_hits"] == 1.0


def test_the_percussion_crest_term_is_the_gates_crest(hits):
    (model, model_gate), (ref, ref_gate) = hits
    want = abs(percussion_row_deltas(model_gate, ref_gate)["crest"])
    assert percussion_terms([model], [ref])["crest"] == pytest.approx(want, abs=1e-12)


def test_the_cents_term_takes_the_gates_stretch_with_the_reference_a4_removed(tmp_path, pitched):
    import json
    from types import SimpleNamespace

    from autofit_resolve import reference_a4_offset_c
    from loss import loss_terms
    from profile_summary import stretch_ruler

    rows = [{"timbre": "t", "note": n, "velocity": 80, "cents_vs_et": -3.0} for n in (64, 69, 74)]
    profile = {"capture": {"program": 0}, "rows": rows}
    (tmp_path / "cap.json").write_text(json.dumps(profile))
    offset = reference_a4_offset_c(SimpleNamespace(capture_id="cap", timbre="t"), tmp_path)
    assert offset == stretch_ruler(profile, "t", 0)["offset_c"] == -3.0

    row, _ = pitched
    oracle = {**row, "a4_offset_c": offset}
    model = {**row, "cents_vs_et": row["cents_vs_et"] + 1.0}
    gate = (model["cents_vs_et"] - oracle["cents_vs_et"]) + offset
    assert loss_terms([model], [oracle])["cents"] == pytest.approx(abs(gate))
    # A model tuned to 440 against a reference whose A4 sits 3 c flat is in tune.
    flat = {**row, "cents_vs_et": row["cents_vs_et"] - 3.0, "a4_offset_c": offset}
    assert loss_terms([{**row}], [flat])["cents"] == pytest.approx(0.0, abs=1e-9)


def test_the_tnr_term_reads_both_sides_through_the_references_fit_window_mask(pitched):
    """The reference's own fit-window mask, and the model through it, as the gate reads them."""
    row, gate = pitched
    assert row["fit_tnr_mask"] == gate["tnr_mask"] and "fm" in gate["tnr_mask"]
    model, model_gate = _pitched(bright=5.0, seed=4, vibrato=0.0, oracle=pitched)
    assert model["fit_tnr_mask"] == model_gate["tnr_mask"] == gate["tnr_mask"]
    assert model["fit_tnr_db"] == model_gate["tnr_db"]
    want = abs(model_gate["tnr_db"] - gate["tnr_db"])
    assert want > 0.0
    assert loss_terms([model], [row])["tnr"] == pytest.approx(want, abs=1e-9)
    # The sustain-window record stays the model's `analyze_note` reading, without the fit mask.
    assert model["tnr_mask"] == {k: v for k, v in oracle_tnr_masks([row])[0].items() if k != "fit"}


# --------------------------------------------------------------------------- #
# Where the term's input could still differ from the gate's, one case each
# --------------------------------------------------------------------------- #
def _probe_and_gate(
    capture: np.ndarray,
    *,
    note: int = 57,
    mono_mode: str = "mean",
    preroll_s: float | None = None,
    until: float | None = None,
):
    """One note's probe row, read as a fit reads an oracle, and the gate's row of the same capture.

    `capture` is laid out on the probe's timeline (note-on at the pattern's start); `until` is
    the seconds a looped reference is read to, given to the probe as `read_ends` and to the gate
    as the reference row's loop record.
    """
    from metrics import normalize_rms

    pattern = build_pattern("sustain", 0, notes=[note])
    (n,) = pattern.analysis_notes
    mono = normalize_rms(metrics.to_mono(capture, mono_mode))
    ends = None if until is None else {(n.note, n.velocity): until}
    (row,) = probe_rows(mono, pattern, SR, raw=capture, read_ends=ends)
    loop = None if until is None else {"verdict": "looped", "read_to_join_s": until}
    gate = measure_note(
        capture,
        SR,
        n.note,
        preroll_s=n.start if preroll_s is None else preroll_s,
        gate_s=n.dur,
        loop=loop,
    )
    return row, gate


def _timeline(note: int, f0: float, **kw) -> np.ndarray:
    pattern = build_pattern("sustain", 0, notes=[note])
    (n,) = pattern.analysis_notes
    y = np.zeros(int(analysis_window_end(pattern, n) * SR))
    at = int(n.start * SR)
    y[at:] = _struck(len(y) - at, f0, **kw)
    return y


def _assert_every_own_reading_equal(row: dict, gate: dict) -> None:
    for term, (read, fields) in PITCHED_READINGS.items():
        got = read(row)
        assert (got if isinstance(got, tuple) else (got,)) == tuple(gate.get(f) for f in fields), (
            term
        )


def test_a_looped_reference_is_read_to_its_join_as_the_gate_reads_it():
    """Past the join the note changes pitch; the gate's fit window and body stop at it, and so does the term."""
    until = 0.7
    y = _timeline(57, 220.0, vibrato=0.0)
    at = int(0.1 * SR)
    join = at + int(until * SR)
    y[join:] = _timeline(57, 233.0, vibrato=0.0, seed=3)[join:]
    row, gate = _probe_and_gate(y, until=until)
    _, whole_gate = _probe_and_gate(y)
    assert gate["cents_vs_et"] != whole_gate["cents_vs_et"]
    _assert_every_own_reading_equal(row, gate)
    assert row["read_to_join_s"] == until


def test_a_model_row_reads_to_the_join_its_references_row_carries():
    y = _timeline(57, 220.0, vibrato=0.0)
    y[int(0.8 * SR) :] = _timeline(57, 233.0, vibrato=0.0, seed=3)[int(0.8 * SR) :]
    ref, _ = _probe_and_gate(y, until=0.7)
    pattern = build_pattern("sustain", 0, notes=[57])
    (model,) = probe_rows(y, pattern, SR, raw=y, tnr_masks=oracle_tnr_masks([ref]))
    assert model["cents_vs_et"] == ref["cents_vs_et"]
    assert model["body_below_f0_db"] == ref["body_below_f0_db"]


def test_a_stereo_capture_is_read_off_the_mean_downmix_whatever_the_mono_mode():
    left = _timeline(57, 220.0, bright=3.0, seed=1)
    right = _timeline(57, 220.0, bright=9.0, seed=2, vibrato=0.01)
    capture = np.stack([left, right], axis=1)
    row, gate = _probe_and_gate(capture, mono_mode="left")
    _assert_every_own_reading_equal(row, gate)
    mean_row, _ = _probe_and_gate(capture)
    assert row["fit_centroid_hz"] == mean_row["fit_centroid_hz"]


def test_a_sound_that_starts_after_its_preroll_is_read_as_the_gate_reads_it():
    """A capture with a lead-in; the oracle drops it (`corpus_oracle`) and the term reads the rest."""
    from metrics_signal import sound_onset_s

    lead = int(0.13 * SR)
    note = _struck(int(3.0 * SR), 220.0)
    capture = np.concatenate([1e-5 * np.ones(lead), note])
    gate = measure_note(capture, SR, 57, preroll_s=0.1, gate_s=2.0)
    start = round(sound_onset_s(capture, SR, 0.1, len(capture) / SR) * SR)
    pattern = build_pattern("sustain", 0, notes=[57])
    (n,) = pattern.analysis_notes
    timeline = np.zeros(int(analysis_window_end(pattern, n) * SR))
    seg = capture[start:]
    at = int(n.start * SR)
    room = min(len(seg), len(timeline) - at)
    timeline[at : at + room] = seg[:room]
    (row,) = probe_rows(timeline, pattern, SR, raw=timeline)
    _assert_every_own_reading_equal(row, gate)


@pytest.mark.parametrize("note,f0", [(33, 55.0), (45, 110.0)])
def test_a_low_note_is_read_at_the_fit_windows_fundamental(note, f0):
    """The onset's envelope window is a period of the f0 the gate's partial fit found."""
    row, gate = _probe_and_gate(_timeline(note, f0, vibrato=0.0), note=note)
    _assert_every_own_reading_equal(row, gate)


def test_the_onset_is_read_at_the_fit_windows_f0_not_the_held_pitchs(monkeypatch):
    """`analyze_note` reads its onset at its own held pitch; the term takes the gate's f0 instead."""
    import loss
    from metrics import onset_shape

    y = _timeline(33, 55.0, vibrato=0.0)
    _, gate = _probe_and_gate(y, note=33)
    real = loss.analyze_note

    def off_pitch(mono, sr, note, end, **kw):
        out = real(mono, sr, note, end, **kw)
        out.f0_hz = 55.0 / 8.0
        shape = onset_shape(mono, sr, kw["onset"], note.dur, out.f0_hz)
        out.onset_overshoot_db, out.onset_settle_ms = (
            shape["onset_overshoot_db"],
            shape["onset_settle_ms"],
        )
        return out

    monkeypatch.setattr(loss, "analyze_note", off_pitch)
    row, _ = _probe_and_gate(y, note=33)
    assert (row["onset_overshoot_db"], row["onset_settle_ms"]) == (
        gate["onset_overshoot_db"],
        gate["onset_settle_ms"],
    )
    held = off_pitch(
        y, SR, build_pattern("sustain", 0, notes=[33]).analysis_notes[0], 3.6, onset=0.1
    )
    assert (held.onset_overshoot_db, held.onset_settle_ms) != (
        gate["onset_overshoot_db"],
        gate["onset_settle_ms"],
    )


def test_the_velspan_term_reads_the_velocities_the_two_sides_share():
    from loss_dimensions import LEVEL_DELTA_CAP_DB, VELSPAN_FIELD, _velspan_terms
    from profile_gate import vel_range_deltas

    def rows(by_velocity: dict[int, float], velocities=(40, 80, 120)) -> list[dict]:
        """One row per probe velocity, the reading absent where the side holds none."""
        return [{"note": 60, "velocity": v, VELSPAN_FIELD: by_velocity.get(v)} for v in velocities]

    reference = {40: -30.0, 80: -18.0, 120: -6.0}
    # Where the model holds every velocity the reference does, the reading is the gate's.
    for model, ref in (
        ({40: -31.0, 80: -25.0, 120: -3.0}, reference),
        ({40: -30.0, 80: -17.0, 120: -2.0}, {80: -18.0, 120: -6.0}),
    ):
        (gate,) = vel_range_deltas({60: model}, {60: ref})
        term, groups = _velspan_terms(rows(model), rows(ref))
        assert groups == 1 and term == pytest.approx(abs(gate))
    # A velocity the reference holds and the model does not is the model gone blind: the cap.
    for model in ({80: -17.0, 120: -2.0}, {40: -30.0, 80: -25.0}, {40: -31.0, 120: -3.0}):
        term, groups = _velspan_terms(rows(model), rows(reference))
        assert groups == 1 and term == pytest.approx(LEVEL_DELTA_CAP_DB)
    # A velocity only the model holds is not the reference's to charge.
    four = (40, 80, 100, 120)
    soft, hard = {40: -30.0, 80: -18.0}, {40: -30.0, 80: -18.0, 100: -9.0, 120: -6.0}
    assert _velspan_terms(rows(hard, four), rows(soft, four)) == (0.0, 1)


# --------------------------------------------------------------------------- #
# The recording's floor, and the loop, take a reading off the gate; the term follows
# --------------------------------------------------------------------------- #
def _floor_row(**censors) -> dict:
    return {
        "note": 57,
        "velocity": 100,
        "timbre": "t",
        "tnr_db": 20.0,
        "body_below_f0_db": -30.0,
        "floor_state": {"censors": {"body": None, "tnr": None, **censors}},
    }


def test_the_floor_takes_the_terms_the_gate_takes_off_a_reference_row():
    from loop_record import floor_censored_terms
    from metrics_floor import body_floor_state, tnr_floor_state

    for censors in ({}, {"tnr": "floor"}, {"body": "hum"}, {"tnr": "floor", "body": "floor"}):
        row = _floor_row(**censors)
        gate_drops = {
            t
            for t, state in (("tnr", tnr_floor_state), ("body", body_floor_state))
            if state(row) is not None
        }
        assert set(floor_censored_terms(row)) == gate_drops == set(censors)


def test_a_floor_censored_row_costs_the_term_nothing_and_is_counted(pitched, monkeypatch):
    import loop_record

    row, _ = pitched
    far = {**row, "fit_tnr_db": row["fit_tnr_db"] + 9.0, "body_below_f0_db": 5.0}
    ref = {**row, "note": 57, "velocity": 100}
    monkeypatch.setattr(
        loop_record,
        "_committed_rows",
        lambda *_: [_floor_row(tnr="floor", body="floor")],
    )
    assert loss_terms([far], [ref])["tnr"] > 0.0
    assert loop_record.mark_floor_rows([ref], "cap", "t") == 1
    terms = loss_terms([far], [ref])
    assert terms["tnr"] == 0.0 and terms["body"] == 0.0
    assert terms["tnr_notes"] == 0.0 and terms["body_hits"] == 0.0
    assert terms["tnr_floor_dropped"] == terms["body_floor_dropped"] == 1.0
    # Every reading dropped is no comparison, not a perfect one.
    assert loop_record.committed_term_cells("cap", "t", "tnr") == set()
    assert loop_record.committed_term_cells("cap", "t", "body") == set()


def test_a_probe_every_cell_of_which_is_at_the_floor_does_not_weigh_the_terms(monkeypatch):
    from types import SimpleNamespace

    import loop_record
    from autofit_resolve import has_reference_cells
    from loss_weights import DROP_REASONS, cli_weights

    corpus = SimpleNamespace(capture_id="cap", timbre="t")
    pattern = build_pattern("sustain", 0, notes=[57])
    rows = [_floor_row(tnr="floor", body="floor")]
    monkeypatch.setattr(loop_record, "_committed_rows", lambda *_: rows)
    assert not has_reference_cells(corpus, pattern, "tnr")
    assert not has_reference_cells(corpus, pattern, "body")
    rows[:] = [_floor_row()]
    assert has_reference_cells(corpus, pattern, "tnr") and has_reference_cells(
        corpus, pattern, "body"
    )
    assert {"tnr", "body"} <= set(DROP_REASONS)
    args = SimpleNamespace(
        program=0,
        percussive=False,
        drum_note=None,
        has_tnr_reference=False,
        has_body_reference=False,
        **{f"w_{t}": None for t in ("tnr", "body")},
    )
    assert "tnr" not in cli_weights(args) and "body" not in cli_weights(args)


def test_a_loop_takes_body_off_the_row_as_the_gate_excludes_it():
    from loop_record import LOOP_TERM_DIMENSION, unweighted_terms

    assert unweighted_terms({"excluded": {"body": "x"}}) == ["body"]
    assert LOOP_TERM_DIMENSION["body"] == "body"


# --------------------------------------------------------------------------- #
# An absent reading never costs less than a present one
# --------------------------------------------------------------------------- #
@pytest.mark.parametrize(
    "term,field,ref",
    [("tnr", "fit_tnr_db", 20.0), ("cents", "cents_vs_et", 0.0)],
)
def test_an_absent_model_reading_costs_no_less_than_any_present_one(pitched, term, field, ref):
    row, _ = pitched
    oracle = {**row, field: ref}
    absent = loss_terms([{**row, field: None}], [oracle])[term]
    for delta in np.linspace(0.0, 400.0, 81):
        present = loss_terms([{**row, field: ref + float(delta)}], [oracle])[term]
        assert absent >= present - 1e-12, (term, delta)


def test_an_absent_attack_share_costs_no_less_than_any_present_one(pitched):
    from loss_dimensions import HF_DELTA_CAP_DB

    row, _ = pitched
    oracle = row
    absent = loss_terms([{**row, "attack_hf_db": [None] * len(row["attack_hf_db"])}], [oracle])
    base = attack_hf_share_of(oracle["attack_hf_db"])
    assert base is not None
    for delta in np.linspace(0.0, 4.0 * HF_DELTA_CAP_DB, 81):
        shifted = [None if v is None else v + float(delta) for v in oracle["attack_hf_db"]]
        present = loss_terms([{**row, "attack_hf_db": shifted}], [oracle])
        assert absent["hfshare"] >= present["hfshare"] - 1e-12, delta


# --------------------------------------------------------------------------- #
# Kit counterparts, and the window a slot is read over
# --------------------------------------------------------------------------- #
KIT_FIELDS = ("crest_db", "centroid_hz", "flatness_db", "tilt_rise_db", "bands_db")


def _stereo_hit(n: int, seed: int) -> np.ndarray:
    """Two channels that differ in brightness, so no single channel reads as their mean."""
    return np.stack([_hit(n, bright=0.2, seed=seed), _hit(n, bright=3.0, seed=seed + 1)], axis=1)


def test_a_kit_hit_is_read_off_the_mean_downmix_whatever_the_mono_mode():
    from metrics import normalize_rms

    pattern = build_pattern("drum", 0, notes=[38], velocities=(100,))
    (note,) = pattern.analysis_notes
    capture = np.zeros((int(analysis_window_end(pattern, note) * SR), 2))
    at = int(note.start * SR)
    capture[at:] = _stereo_hit(len(capture) - at, 5)
    mono = normalize_rms(metrics.to_mono(capture, "left"))
    (row,) = probe_rows(mono, pattern, SR, raw=capture)
    gate = measure_hit(capture, SR, note.note, note.velocity, preroll_s=note.start, gate_s=note.dur)
    for field in (*KIT_FIELDS, "mono_peak_dbfs"):
        want = gate["peak_dbfs"] if field == "mono_peak_dbfs" else gate[field]
        assert row[field] == want, field


def test_a_corpus_slot_is_read_whole_as_the_gate_reads_its_recording():
    """A hit that starts 0.8 s into a 2 s slot lies past its own gate plus the last slot's tail."""
    from patterns import Note, Pattern

    notes = [Note(38, 100, 0.0, 0.3), Note(38, 100, 2.0, 0.3)]
    pattern = Pattern("corpus", notes, analysis_notes=list(notes), tail=0.3, percussive=True)
    assert analysis_window_end(pattern, notes[0]) == 2.0
    slot = np.zeros(int(2.0 * SR))
    late = int(0.8 * SR)
    slot[late:] = _hit(len(slot) - late, bright=1.0, seed=7)
    timeline = np.concatenate([slot, np.zeros(int(0.6 * SR))])
    (row, _) = probe_rows(timeline, pattern, SR, raw=timeline)
    gate = measure_hit(slot, SR, 38, 100, preroll_s=0.0, gate_s=0.3)
    for field in KIT_FIELDS:
        assert row[field] == gate[field], field


def test_an_absent_centroid_costs_no_less_than_any_present_one(pitched, hits):
    (kit, _), _ = hits
    row, _ = pitched
    for rows, field, terms in (
        ((row, row), "fit_centroid_hz", loss_terms),
        ((kit, kit), "centroid_hz", percussion_terms),
    ):
        absent = terms([{**rows[0], field: None}], [rows[1]])["bright"]
        for factor in np.linspace(0.1, 20.0, 80):
            shifted = {**rows[0], field: rows[0][field] * float(factor)}
            assert absent >= terms([shifted], [rows[1]])["bright"] - 1e-12, (field, factor)


def test_an_absent_band_profile_costs_no_less_than_any_present_one(hits):
    (ref, _), _ = hits
    absent = percussion_terms([{**ref, "bands_db": []}], [ref])["bandrms"]
    assert absent > 0.0
    for delta in np.linspace(0.0, 300.0, 61):
        shifted = {
            **ref,
            "bands_db": [b + float(delta) * (-1) ** i for i, b in enumerate(ref["bands_db"])],
        }
        assert absent >= percussion_terms([shifted], [ref])["bandrms"] - 1e-12, delta


def _a4_world(tmp_path):
    """A profile with five off-grid rows reading +30 c beside two in-grid rows reading -3 c."""
    import json
    from types import SimpleNamespace

    rows = [
        {"timbre": "t", "note": n, "velocity": 80, "cents_vs_et": 30.0}
        for n in (67, 68, 69, 70, 71)
    ] + [{"timbre": "t", "note": n, "velocity": 80, "cents_vs_et": -3.0} for n in (66, 72)]
    (tmp_path / "cap.json").write_text(json.dumps({"capture": {"program": 0}, "rows": rows}))
    (tmp_path / "def").mkdir()
    (tmp_path / "def" / "cap.json").write_text(json.dumps({"id": "cap", "notes": [66, 72]}))
    return SimpleNamespace(capture_id="cap", timbre="t"), tmp_path / "def"


def test_the_a4_offset_is_read_over_the_capture_definitions_grid(tmp_path):
    from autofit_resolve import reference_a4_offset_c

    corpus, definitions = _a4_world(tmp_path)
    assert reference_a4_offset_c(corpus, tmp_path, capture_dir=definitions) == -3.0
    # Without a definition naming a grid every row stands.
    assert reference_a4_offset_c(corpus, tmp_path, capture_dir=tmp_path / "none") == 30.0


def test_the_a4_offset_follows_the_program_the_model_answers_with(tmp_path):
    from autofit_resolve import reference_a4_offset_c

    corpus, definitions = _a4_world(tmp_path)
    assert reference_a4_offset_c(corpus, tmp_path, program=0, capture_dir=definitions) == -3.0
    # A program whose class reads no stretch has no ruler, whatever program the profile recorded.
    assert reference_a4_offset_c(corpus, tmp_path, program=8, capture_dir=definitions) is None


def test_an_absent_flatness_or_tilt_rise_costs_no_less_than_any_present_one(hits):
    (kit, _), _ = hits
    for field, term in (("flatness_db", "tonal"), ("tilt_rise_db", "rise")):
        absent = percussion_terms([{**kit, field: None}], [kit])
        assert absent[term] > 0.0 and absent[f"{term}_hits"] == 1.0
        for delta in np.linspace(0.0, 200.0, 81):
            present = percussion_terms([{**kit, field: kit[field] + float(delta)}], [kit])
            assert absent[term] >= present[term] - 1e-12, (field, delta)


def test_a_kit_hit_is_searched_for_its_onset_where_the_gate_searched_with_its_lead():
    """The gate's one-second search began in the recording's lead-in, so it reached that much less of the sound."""
    from patterns import Note, Pattern

    lead, slot = 0.3, 1.5
    sound = np.zeros(int((slot - lead) * SR))
    quiet = int(0.2 * SR)
    sound[:quiet] = 0.0005 * np.sin(2 * np.pi * 220.0 * np.arange(quiet) / SR)
    loud = int(0.85 * SR)
    sound[loud:] = _hit(len(sound) - loud, bright=1.0, seed=3)
    capture = np.concatenate([np.zeros(int(lead * SR)), sound])
    notes = [Note(38, 100, 0.0, 0.3), Note(38, 100, slot, 0.3)]
    pattern = Pattern("corpus", notes, analysis_notes=list(notes), tail=slot - 0.3, percussive=True)
    timeline = np.concatenate([sound, np.zeros(len(capture) - len(sound)), np.zeros(len(capture))])
    gate = measure_hit(capture, SR, 38, 100, preroll_s=0.0, gate_s=0.3)
    leads = {(38, 100): lead}
    (row, _) = probe_rows(timeline, pattern, SR, raw=timeline, leads=leads)
    (blind, _) = probe_rows(timeline, pattern, SR, raw=timeline)
    # The timeline drops the lead and pads the slot's end, so the windows agree to a few percent.
    assert row["attack_ms"] == pytest.approx(gate["attack_ms"], abs=2.0)
    assert row["crest_db"] == pytest.approx(gate["crest_db"], abs=1.5)
    assert row["tilt_rise_db"] == pytest.approx(gate["tilt_rise_db"], abs=0.5)
    # Without the lead the search reaches the loud hit, and the note is read from there.
    assert abs(blind["attack_ms"] - gate["attack_ms"]) > 500.0


def test_an_absent_arrival_costs_no_less_than_any_present_one(pitched):
    row, _ = pitched
    absent = loss_terms([{**row, "arrival_ms": None}], [row])["arrival"]
    assert absent > 0.0
    for delta in np.linspace(0.0, 400.0, 81):
        present = loss_terms([{**row, "arrival_ms": row["arrival_ms"] + float(delta)}], [row])
        assert absent >= present["arrival"] - 1e-12, delta


def test_a_reference_with_no_fit_window_mask_leaves_the_models_cell_absent(pitched):
    """The gate has no cell where the reference read no fit window, so the model is not read through a mask of its own."""
    row, _ = pitched
    bare = {k: v for k, v in row.items() if k != "fit_tnr_mask"}
    model, _ = _pitched(
        bright=5.0, seed=4, vibrato=0.0, oracle=(bare, {"tnr_mask": row["tnr_mask"]})
    )
    assert model["fit_tnr_db"] is None and "fit_tnr_mask" not in model
    assert model["cents_vs_et"] is None and model["fit_centroid_hz"] is None
    assert model["body_below_f0_db"] is None
    # The reference's own cell is absent as well, so the term drops it rather than charging it.
    oracle = {**bare, "fit_tnr_db": None}
    terms = loss_terms([model], [oracle])
    assert terms["tnr"] == 0.0 and terms["tnr_notes"] == 0.0


def test_the_cells_charged_at_an_absent_cap_are_counted(pitched):
    row, _ = pitched
    assert loss_terms([row], [row])["absent_charged"] == 0.0
    blind = {**row, "arrival_ms": None, "fit_tnr_db": None, "fit_centroid_hz": None}
    assert loss_terms([blind], [row])["absent_charged"] == 3.0
    # A reference without the reading drops the cell and charges nothing.
    assert loss_terms([row], [{**row, "arrival_ms": None}])["absent_charged"] == 0.0
    # A relation the model went blind on counts once, for the pitch.
    from loss_dimensions import VELSPAN_FIELD

    rows = [{**row, "velocity": v, VELSPAN_FIELD: -30.0 + v / 8} for v in (40, 80, 120)]
    blinded = [rows[0], {**rows[1], VELSPAN_FIELD: None}, rows[2]]
    assert loss_terms(blinded, rows)["absent_charged"] == 1.0
    kit, _ = _percussive(1.0, 1)
    assert percussion_terms([{**kit, "flatness_db": None}], [kit])["absent_charged"] == 1.0
