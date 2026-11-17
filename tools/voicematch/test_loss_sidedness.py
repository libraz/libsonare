"""Terms that charge one direction of a defect must charge the other, and an absent model reading.

Each case moves one side of a synthetic grid and asserts that the side which used to
read zero now reads non-zero while the side that was already charged is unchanged.
"""

from __future__ import annotations

import math

import pytest
from loss import loss_terms, percussion_terms
from loss_dimensions import BRIGHT_ABSENT_CAP
from metrics_bands import THIRD_OCTAVE_CENTERS

NOTES = (60, 72)
VELS = (32, 64, 96, 127)
BASE = {
    "tilt": -6.0, "bvel": 0.2, "cents": 0.0, "tnr": 30.0, "sus": -5.0, "rel": 300.0, "att": 20.0, "attf": 20.0,
    "init": 0.0, "early": -20.0, "late": -10.0, "tail": -8.0, "hf": -30.0, "lf": -20.0, "held": -20.0, "held_v": 0.0,
    "crest": 10.0, "peak_v": 0.2, "ov": 3.0, "settle": 40.0, "ovv": 1.0, "attv": -3.0,
    "cent": 1500.0, "arr": 10.0, "body": -40.0, "b": 1e-4, "nb": 8, "vib": 20.0, "trem": 3.0, "beat": 2.0, "width": 10.0,
    "vibr": 5.5, "tremr": 5.0, "beatr": 1.0, "fms": 0.0, "fmf": 0.0,
}  # fmt: skip


def _row(p: dict, note: int, vel: int) -> dict:
    ladder = [0.0] + [p["tilt"] * k for k in range(1, 12)]
    for k in range(3, 10):
        ladder[k] += p["bvel"] * (vel - 64)
    lv = math.log2(vel / 64)
    return {
        "note": note, "velocity": vel, "f0_hz": 440.0, "harmonics_db": ladder,
        "cents_vs_et": p["cents"], "fit_tnr_db": p["tnr"], "sustain_slope_db_s": p["sus"],
        "release_ms": p["rel"], "attack_ms": p["att"],
        "attack_fine_ms": p["attf"] + p["attv"] * lv,
        "skeleton": {
            "init_db": [p["init"] - 3.0 * k for k in range(12)],
            "early_db_s": [p["early"]] * 12, "late_db_s": [p["late"]] * 12,
            "tail_db_s": [p["tail"]] * 12,
        },
        "attack_hf_db": [p["hf"] + 0.2 * (vel - 64)] * 30, "attack_lf_db": [p["lf"]] * 4,
        "held_peak_dbfs": p["held"] + p["held_v"] * (vel - 64),
        "held_crest_db": p["crest"], "peak_dbfs": -30.0 + p["peak_v"] * (vel - 64),
        "mono_peak_dbfs": -30.0 + p["peak_v"] * (vel - 64),
        "onset_overshoot_db": p["ov"] + p["ovv"] * lv, "onset_settle_ms": p["settle"],
        "fit_centroid_hz": p["cent"], "arrival_ms": p["arr"], "body_below_f0_db": p["body"],
        "inharmonicity_b": p["b"], "inharmonicity_partials": p["nb"],
        "vib_cents": p["vib"], "trem_db": p["trem"], "beat_db": p["beat"],
        "f0_width_cents": p["width"], "vib_rate_hz": p["vibr"], "trem_rate_hz": p["tremr"],
        "beat_rate_hz": p["beatr"], "fm_slow_cents": p["fms"], "fm_fast_cents": p["fmf"],
        "modal_hz": [440.0 * m for m in (1, 2.76, 5.4)], "modal_db": [0.0, -6.0, -12.0],
    }  # fmt: skip


def _rows(p: dict) -> list[dict]:
    return [_row(p, n, v) for n in NOTES for v in VELS]


def _terms(model=None, oracle=None, mutate=None, **kw) -> dict:
    o = {**BASE, **(oracle or {})}
    m = {**o, **(model or {})}
    mr, orr = _rows(m), _rows(o)
    if mutate:
        mutate(mr, orr)
    out = loss_terms(mr, orr, **kw)
    assert out is not None
    return out


def _set(rows: list[dict], **fields) -> None:
    for r in rows:
        r.update(fields)


# --- mod: fm_slow / fm_fast ---


def test_fm_slow_deficit_is_charged_and_excess_is_unchanged():
    deficit = _terms(model={"fms": 0.0}, oracle={"fms": 10.0})["mod"]
    excess = _terms(model={"fms": 30.0}, oracle={"fms": 10.0})["mod"]
    assert deficit == pytest.approx(2.0)
    assert excess == pytest.approx(4.0)


def test_fm_fast_deficit_is_charged_and_excess_is_unchanged():
    deficit = _terms(model={"fmf": 0.0}, oracle={"fmf": 3.0})["mod"]
    excess = _terms(model={"fmf": 6.0}, oracle={"fmf": 3.0})["mod"]
    assert deficit == pytest.approx(6.0)
    assert excess == pytest.approx(6.0)
    assert _terms(model={"fmf": 3.0}, oracle={"fmf": 3.0})["mod"] == 0.0


def test_fm_cap_still_binds_both_ways():
    assert _terms(model={"fms": 500.0})["mod"] == pytest.approx(12.0)
    assert _terms(model={"fms": 0.0}, oracle={"fms": 500.0})["mod"] == pytest.approx(12.0)


# --- tnr: a cleaner model is charged as a noisier one ---


def test_tnr_charges_a_cleaner_model_as_a_noisier_one():
    assert _terms(model={"tnr": 24.0})["tnr"] == pytest.approx(6.0)
    assert _terms(model={"tnr": 36.0})["tnr"] == pytest.approx(6.0)
    assert _terms()["tnr"] == 0.0


# --- env release: a reference that never fell 40 dB has no release to compare ---


def test_a_capped_reference_release_is_skipped_and_an_uncapped_one_is_charged():
    def release(capped: bool) -> dict:
        def mutate(mr, orr):
            _set(mr, release_ms=400.0)
            _set(orr, release_ms=300.0, release_capped=capped)

        return _terms(mutate=mutate)

    free, capped = release(False), release(True)
    cells = len(NOTES) * len(VELS)
    assert capped["env_skipped"] - free["env_skipped"] == cells
    # |400 - 300| / 100 per uncapped cell is the whole difference between the runs.
    assert free["env"] - capped["env"] == pytest.approx(1.0)


# --- absent model reading is charged the cap, absent oracle reading is skipped ---


@pytest.mark.parametrize("term", ["bright", "arrival", "body"])
def test_absent_model_note_reading_is_charged_the_cap(term):
    field = {"bright": "fit_centroid_hz", "arrival": "arrival_ms", "body": "body_below_f0_db"}[term]
    t = _terms(mutate=lambda mr, orr: _set(mr, **{field: None}))
    assert t[term] == pytest.approx(24.0)
    assert t[f"{term}_hits"] == len(NOTES) * len(VELS)


@pytest.mark.parametrize("term", ["bright", "arrival", "body"])
def test_absent_oracle_note_reading_stays_skipped(term):
    field = {"bright": "fit_centroid_hz", "arrival": "arrival_ms", "body": "body_below_f0_db"}[term]
    t = _terms(mutate=lambda mr, orr: _set(orr, **{field: None}))
    assert t[term] == 0.0
    assert t[f"{term}_hits"] == 0.0


def test_measured_note_readings_are_unchanged():
    t = _terms(model={"cent": 3000.0, "arr": 20.0, "body": -50.0})
    assert t["bright"] == pytest.approx(
        BRIGHT_ABSENT_CAP
    )  # +100 %, held to an absent reading's cost
    assert t["arrival"] == pytest.approx(10.0)
    assert t["body"] == pytest.approx(10.0)


def test_dyn_family_charges_a_model_with_no_readable_slope():
    def silent_upper(mr, orr):
        for r in mr:
            r["harmonics_db"] = r["harmonics_db"][:3] + [-120.0] * 9

    t = _terms(mutate=silent_upper)
    assert t["dyn"] == pytest.approx(12.0)
    assert t["dyn_groups"] == len(NOTES)


def test_hfdyn_charges_a_model_with_no_attack_hf():
    t = _terms(mutate=lambda mr, orr: _set(mr, attack_hf_db=None))
    assert t["hfdyn"] == pytest.approx(24.0)


def test_modedyn_charges_a_pitch_whose_model_modes_are_gone():
    def one_pitch_silent(mr, orr):
        _set([r for r in mr if r["note"] == NOTES[1]], modal_hz=[], modal_db=[])

    t = _terms(mutate=one_pitch_silent)
    assert t["modedyn"] == pytest.approx(12.0 / 2)
    assert t["modedyn_groups"] == len(NOTES)


def test_dyn_with_unreadable_oracle_stays_skipped():
    def silent_upper(mr, orr):
        for r in orr:
            r["harmonics_db"] = r["harmonics_db"][:3] + [-120.0] * 9

    t = _terms(mutate=silent_upper)
    assert t["dyn"] == 0.0
    assert t["dyn_groups"] == 0.0


def test_dyn_slope_difference_is_unchanged():
    assert _terms(model={"bvel": 0.3})["dyn"] == pytest.approx(6.4)


def test_stiff_charges_a_model_with_too_few_partials():
    short = _terms(model={"b": 4e-4, "nb": 3})["stiff"]
    assert short == pytest.approx(50.0)
    assert _terms(model={"b": 4e-4})["stiff"] == pytest.approx(36.1005, abs=1e-3)


def test_stiff_with_a_thin_oracle_stays_skipped():
    assert _terms(model={"b": 4e-4}, oracle={"nb": 3})["stiff"] == 0.0


def test_onset_charges_a_model_with_no_onset_fields():
    t = _terms(mutate=lambda mr, orr: _set(mr, onset_overshoot_db=None))
    assert t["onset"] == pytest.approx(16.0)
    assert t["onset_compared"] == len(NOTES) * len(VELS)


def test_onset_with_an_ineligible_oracle_stays_skipped():
    t = _terms(model={"ov": 20.0}, mutate=lambda mr, orr: _set(orr, onset_overshoot_db=None))
    assert t["onset"] == 0.0


# --- harm ---


def test_silent_ladder_with_modes_present_is_charged():
    def silent(mr, orr):
        for r in mr:
            r["harmonics_db"] = [0.0] + [-120.0] * 11

    assert _terms(mutate=silent)["harm"] > 100.0


def test_a_sentinel_bin_on_either_side_alone_is_still_skipped():
    def one_bin(mr, orr):
        for r in mr:
            r["harmonics_db"][11] = -120.0

    assert _terms(mutate=one_bin)["harm"] == 0.0


def test_audibility_weight_comes_from_the_louder_side():
    dim = _terms(model={"tilt": -9.0})["harm"]
    bright = _terms(model={"tilt": -3.0})["harm"]
    assert dim == pytest.approx(46.5, abs=0.1)
    assert bright == pytest.approx(101.5, abs=0.1)


# --- ring ---

_NB = len(THIRD_OCTAVE_CENTERS)
_PERC = {
    "band_off": 0.0, "floor_fill": -60.0, "bdec": -50.0, "att": 5.0, "dec": 300.0, "crest": 12.0, "cent": 2000.0,
    "flat": -10.0, "rise": 4.0, "strike": 3.0, "dens": 10.0, "prompt": -20.0, "evo": 5.0, "wfl": -8.0, "capped": False,
    "tone": 100.0, "peak_v": 0.2, "lfboost": 0.0,
}  # fmt: skip


def _perc_row(p: dict, note: int, vel: int) -> dict:
    bands = [-3.0 * abs(i - 8) / 2 + p["band_off"] for i in range(_NB)]
    bands = [b if i < _NB - 4 else p["floor_fill"] for i, b in enumerate(bands)]
    return {
        "note": note, "velocity": vel, "bands_db": bands, "band_decay_db_s": [p["bdec"]] * 8,
        "attack_ms": p["att"], "decay_ms": p["dec"], "crest_db": p["crest"],
        "centroid_hz": p["cent"], "flatness_db": p["flat"], "tilt_rise_db": p["rise"],
        "tilt_strike_db": p["strike"], "decay_capped": p["capped"],
        "tone_f0_hz": p["tone"] * (1 + 0.5 * (note - 36) / 6),
        "modal_density": [p["dens"]] * 3, "modal_density_valid": [True] * 3,
        "prompt_late_db": [p["prompt"]] * 3, "prompt_late_valid": [True] * 3,
        "evolution_db": [[p["evo"]] * 3] * 2, "evolution_valid": [[True] * 3] * 2,
        "evolution_measured": [[True] * 3] * 2, "window_flatness_db": [p["wfl"]] * 3,
        "window_flatness_valid": [True] * 3, "window_flatness_measured": [True] * 3,
        "held_rms_dbfs": -20.0, "held_crest_db": p["crest"],
        "peak_dbfs": -30.0 + p["peak_v"] * (vel - 80), "modal_hz": [], "modal_db": [],
    }  # fmt: skip


def _ring(model: dict, oracle: dict | None = None) -> float:
    o = {**_PERC, **(oracle or {})}
    m = {**o, **model}
    rows = lambda p: [_perc_row(p, n, v) for n in (36, 38, 42) for v in (40, 80, 120)]
    out = percussion_terms(rows(m), rows(o))
    assert out is not None
    return out["ring"]


def test_capped_model_decay_is_a_lower_bound_not_a_skip():
    assert _ring({"dec": 1200.0, "capped": True}) == pytest.approx(2.0)
    assert _ring({"dec": 1200.0, "capped": False}) == pytest.approx(2.0)


def test_capped_oracle_decay_is_still_refused():
    assert _ring({"dec": 1200.0}, oracle={"capped": True}) == 0.0


def test_uncapped_ring_is_unchanged():
    assert _ring({"dec": 150.0}) == pytest.approx(1.0)
    assert _ring({"dec": 600.0}) == pytest.approx(1.0)
