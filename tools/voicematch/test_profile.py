"""The parts of a reference profile that are not specific to one instrument.

A profile is measured once and compared against for months, so the failures
worth a test are the ones that produce a plausible number from a wrong premise:
a program nobody chose, a noise measurement whose window is narrower than an FFT
bin, a dynamic range read off a single velocity.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module  # noqa: E402
from capture import note_groups, note_map  # noqa: E402
from loss import _kit_terms, kit_report  # noqa: E402
from metrics import _spectrum  # noqa: E402

SR = 48000


CAPTURE_DIR = Path(__file__).resolve().parent / "capture"
REFERENCE_DIR = Path(__file__).resolve().parent / "reference"


def shipped_captures(root: Path | None = None) -> list[str]:
    """Every committed capture definition, by id.

    Globbed rather than listed. A hand-kept list of instrument names is the
    mirror table these tests exist to make unnecessary: the failure worth
    catching is a capture added without being added to the list, which a list
    cannot catch by construction. The `.local.json` overlays are the untracked
    identity half and are excluded by suffix.
    """
    here = root or CAPTURE_DIR
    return sorted(p.stem for p in here.glob("*.json") if not p.name.endswith(".local.json"))


def assert_names_no_product(name: str, *, capture_dir: Path, reference_dir: Path) -> None:
    """Neither half of one instrument's committed record identifies a product.

    Keys rather than the words: the prose in these files explains that the
    plugin triple and the presets live in the untracked overlay, and it should.
    What must not appear is a value - `profile.py measure` copies the capture
    block into the reference, so a field added on one side reaches the other.

    Taking its directories as arguments is what lets the test below point it at
    a file that does name a product, and so show that it fails.
    """
    docs = [(json.loads((capture_dir / f"{name}.json").read_text()), "capture")]
    reference = reference_dir / f"{name}.json"
    if reference.exists():
        docs.append((json.loads(reference.read_text()).get("capture", {}), "reference"))
    for doc, where in docs:
        assert "plugin" not in doc, f"{name} {where} names its plugin"
        for timbre in doc.get("timbres", []):
            assert "preset" not in timbre, f"{name} {where} names a preset"


def harmonic(f0: float, n_harm: int = 8, seconds: float = 1.0,
             noise: float = 0.0) -> np.ndarray:
    t = np.arange(int(SR * seconds)) / SR
    x = sum(np.sin(2 * np.pi * f0 * n * t) / n for n in range(1, n_harm + 1))
    if noise:
        x = x + noise * np.random.default_rng(0).standard_normal(t.shape)
    return np.asarray(x, dtype=np.float64)


# --------------------------------------------------------------------------
# tone against mechanism noise


def test_a_clean_harmonic_series_reads_far_above_a_noisy_one():
    clean = _spectrum(harmonic(220.0), SR)
    noisy = _spectrum(harmonic(220.0, noise=0.30), SR)
    assert (profile_module.tone_to_noise_db(*clean, 220.0)
            > profile_module.tone_to_noise_db(*noisy, 220.0) + 20.0)


def test_broadband_noise_alone_is_not_reported_as_tonal():
    t = np.arange(SR) / SR
    noise = np.random.default_rng(1).standard_normal(t.shape)
    assert profile_module.tone_to_noise_db(*_spectrum(noise, SR), 220.0) < 0.0


@pytest.mark.parametrize("f0", [44.0, 55.0, 82.4])
def test_a_bass_note_is_not_scored_as_noise_by_the_analysis_window(f0):
    """The partial window has to stay wider than the FFT bin.

    At 44 Hz a +/-2 % window asks for +/-0.9 Hz out of a 1.7 Hz grid, so a
    relative-only window would put a clean bass note's own fundamental outside
    the band counted as tonal and report the string as a noise burst.
    """
    assert profile_module.tone_to_noise_db(*_spectrum(harmonic(f0), SR), f0) > 20.0


def test_an_empty_spectrum_is_not_a_measurement():
    assert not np.isfinite(
        profile_module.tone_to_noise_db(np.zeros(0), np.zeros(0), 220.0)
    )


# --------------------------------------------------------------------------
# the velocity axis


def rows(peaks: dict[int, float], note: int = 60, timbre: str = "a") -> list[dict]:
    return [{"timbre": timbre, "note": note, "velocity": v, "peak_dbfs": p}
            for v, p in peaks.items()]


def test_the_range_spans_the_whole_captured_axis():
    got = profile_module.velocity_response(rows({24: -30.0, 88: -18.0, 120: -12.0}))
    assert got["60"]["range_db"] == pytest.approx(18.0)
    assert got["60"]["monotonic"]


def test_a_non_monotonic_instrument_is_reported_rather_than_smoothed():
    """On a plucked instrument this is a property, not a fault.

    A model that rises monotonically would otherwise pass on the range alone
    while being wrong about the one thing the axis was captured to settle.
    """
    got = profile_module.velocity_response(rows({24: -20.0, 88: -14.0, 120: -17.0}))
    assert got["60"]["range_db"] == pytest.approx(6.0)
    assert not got["60"]["monotonic"]


def test_one_velocity_is_no_range_at_all():
    assert profile_module.velocity_response(rows({88: -18.0})) == {}


def test_the_summary_carries_the_velocity_response_per_timbre():
    measured = rows({24: -30.0, 120: -12.0}, timbre="dry") + \
        rows({24: -20.0, 120: -18.0}, timbre="wet")
    summary = profile_module.summarize(measured)
    assert summary["dry"]["velocity_response"]["60"]["range_db"] == pytest.approx(18.0)
    assert summary["wet"]["velocity_response"]["60"]["range_db"] == pytest.approx(2.0)


# --------------------------------------------------------------------------
# which instrument the model answers with


def test_the_program_comes_from_the_profile_the_capture_recorded():
    """Not from whichever config the compare was handed.

    The two are only tied together where the capture was measured, and a
    hardcoded program is how a harpsichord reference ends up diffed against a
    grand piano without a word of complaint.
    """
    assert profile_module.profile_program({"capture": {"program": 6}}, {"program": 0}) == 6


def test_a_profile_measured_before_the_field_existed_falls_back_to_the_config():
    assert profile_module.profile_program({"capture": {}}, {"program": 6}) == 6
    assert profile_module.profile_program({}, {}) == 0


def test_the_command_line_overrides_what_the_profile_recorded():
    """One capture judging two programs is a real thing to want."""
    cfg = {"program": 7, "_program_override": True}
    assert profile_module.profile_program({"capture": {"program": 6}}, cfg) == 7


# --------------------------------------------------------------------------
# what a capture definition declares


def test_a_capture_that_names_no_phrase_set_still_resolves_one(tmp_path):
    """`load_config` declares every field it knows about, which turns a missing
    key into an empty one — and a `get(key, default)` downstream then never
    fires. The audition set is the field where that shows."""
    from capture import load_config

    cfg_path = tmp_path / "c.json"
    cfg_path.write_text(json.dumps({"id": "x", "label": "x", "plugin": "a:b:c",
                                    "timbres": [], "notes": [60], "velocities": [100]}))
    cfg = load_config(cfg_path)
    assert cfg["takes"] == ""
    assert (cfg.get("takes") or "piano") == "piano"


def test_a_capture_declares_the_program_the_model_answers_with(tmp_path):
    from capture import load_config

    cfg_path = tmp_path / "c.json"
    cfg_path.write_text(json.dumps({"id": "x", "label": "x", "plugin": "a:b:c",
                                    "program": 6, "timbres": [], "notes": [60],
                                    "velocities": [100]}))
    assert load_config(cfg_path)["program"] == 6


def test_the_identity_overlay_matches_timbres_by_id(tmp_path):
    """The tracked half holds the method, the untracked half holds the product.

    Matching is by timbre id, so a rename on one side and not the other yields a
    config that loads, reports no error, and captures every slot with an empty
    preset — which on a rack is slot 1, four times over.
    """
    from capture import load_config

    (tmp_path / "c.json").write_text(json.dumps({
        "id": "c", "label": "Concert grands, close", "notes": [60], "velocities": [100],
        "timbres": [{"id": "grand-227", "label": "227 cm concert grand", "channel": 1},
                    {"id": "grand-274", "label": "274 cm concert grand", "channel": 2}],
    }))
    (tmp_path / "c.local.json").write_text(json.dumps({
        "plugin": "aumu:xxxx:Vend",
        "timbres": [{"id": "grand-227", "preset": "A/Close"},
                    {"id": "grand-274", "preset": "B/Close"}],
    }))
    cfg = load_config(tmp_path / "c.json")
    assert cfg["plugin"] == "aumu:xxxx:Vend"
    assert [t["preset"] for t in cfg["timbres"]] == ["A/Close", "B/Close"]
    # The tracked side still owns everything it declared.
    assert [t["channel"] for t in cfg["timbres"]] == [1, 2]
    assert [t["label"] for t in cfg["timbres"]] == ["227 cm concert grand",
                                                    "274 cm concert grand"]


def test_a_capture_without_its_overlay_loads_and_carries_no_product(tmp_path):
    """A clone has no overlay, and that is the normal case rather than an error:
    everything downstream reads the committed reference profile."""
    from capture import load_config

    (tmp_path / "c.json").write_text(json.dumps({
        "id": "c", "label": "Concert grands, close", "notes": [60], "velocities": [100],
        "timbres": [{"id": "grand-227", "label": "227 cm concert grand", "channel": 1}],
    }))
    cfg = load_config(tmp_path / "c.json")
    assert "plugin" not in cfg
    assert cfg["timbres"][0]["id"] == "grand-227"


def test_the_shipped_captures_carry_no_plugin_identity():
    """The committed half must not say which commercial product was captured."""
    shipped = shipped_captures()
    assert shipped, "no capture definitions found to check"
    for name in shipped:
        assert_names_no_product(name, capture_dir=CAPTURE_DIR, reference_dir=REFERENCE_DIR)


def test_the_identity_guard_fails_on_a_capture_that_does_name_a_product(tmp_path):
    """Without this the guard above passes whether or not it can see anything.

    Both halves are checked, because the reference is written from the capture
    and a scrub that only cleaned the capture would leave the copy behind.
    """
    capture, reference = tmp_path / "capture", tmp_path / "reference"
    capture.mkdir()
    reference.mkdir()
    (capture / "x.json").write_text(json.dumps({
        "id": "x", "program": 0, "plugin": "aumu:xxxx:Yyyy", "timbres": [{"id": "t"}],
    }))
    assert shipped_captures(capture) == ["x"]
    with pytest.raises(AssertionError, match="names its plugin"):
        assert_names_no_product("x", capture_dir=capture, reference_dir=reference)

    (capture / "x.json").write_text(json.dumps({
        "id": "x", "program": 0, "timbres": [{"id": "t", "preset": "Grand/Close.fxp"}],
    }))
    with pytest.raises(AssertionError, match="names a preset"):
        assert_names_no_product("x", capture_dir=capture, reference_dir=reference)

    (capture / "x.json").write_text(json.dumps({"id": "x", "program": 0, "timbres": [{"id": "t"}]}))
    (reference / "x.json").write_text(json.dumps({"capture": {"plugin": "aumu:xxxx:Yyyy"}}))
    with pytest.raises(AssertionError, match="reference names its plugin"):
        assert_names_no_product("x", capture_dir=capture, reference_dir=reference)


def test_measure_records_the_method_and_not_the_captured_product(tmp_path):
    """The guard above checks the committed files; this checks what writes them.

    A corpus manifest is written from the *merged* configuration, so it holds
    the untracked overlay's half — the plugin triple, and the preset each slot
    was loaded from. Copying that block through is how a product name reaches a
    committed reference, and the file guard only sees it once someone has
    already measured and staged one.
    """
    tracked = {
        "id": "x",
        "label": "A method, stated without naming a product",
        "timbres": [{"id": "t", "label": "The registration, described"}],
    }
    manifest = {
        "plugin": "aumu:xxxx:Yyyy",
        "params": [], "sample_rate": 48000, "gate_ms": 1000, "tail": "2s",
        "preroll_ms": 100, "notes": [60], "velocities": [100],
        "timbres": [
            {"id": "t", "label": "Product Name 9 Concert", "preset": "Product/Close.vstpreset"},
            {"id": "model", "label": "libsonare, GM program 19"},
        ],
    }
    block = profile_module.committed_capture({"program": 19}, tracked, manifest)

    assert "plugin" not in block
    assert block["timbres"] == [{"id": "t", "label": "The registration, described"}]
    assert block["notes"] == [60] and block["gate_ms"] == 1000


def test_a_kit_is_recognised_from_the_channel_its_notes_are_played_on():
    """Which metric set a capture gets, decided where the distinction already lives.

    The failure this catches is silent and total: a kit whose channel went
    missing is measured with the pitched metric set, and every note of it comes
    back with a fundamental, a stretch and an inharmonicity, none of which a
    drum has. It reads as a successful measurement of the wrong instrument.
    """
    from capture import load_config

    here = Path(__file__).resolve().parent
    for name, percussion in (("drums", True), ("piano", False), ("harpsichord", False),
                             ("pipe_organ", False)):
        cfg = load_config(here / "capture" / f"{name}.json")
        assert profile_module.is_percussion(cfg) is percussion, name

    # And from the committed reference, which is what `compare` actually reads:
    # the channel has to survive into the profile or a later comparison decides
    # differently from the measurement it is comparing against.
    reference = REFERENCE_DIR / "drums.json"
    if reference.exists():
        assert profile_module.is_percussion(json.loads(reference.read_text())["capture"])


def test_a_capture_that_mixes_a_kit_with_a_melodic_slot_is_refused():
    """One profile cannot be measured both ways, and picking one would be wrong twice."""
    with pytest.raises(ValueError, match="mixes percussion and melodic"):
        profile_module.is_percussion(
            {"timbres": [{"id": "kit", "channel": 10}, {"id": "lead", "channel": 1}]}
        )


def test_the_band_comparisons_report_a_direction_and_a_magnitude_separately():
    """Tilt says which way a hit is wrong; shape says how much that failed to explain."""
    flat = [0.0] * len(profile_module.THIRD_OCTAVE_CENTERS)
    assert profile_module.band_tilt_db(flat) == pytest.approx(0.0)
    assert profile_module.band_shape_error_db(flat, flat) == pytest.approx(0.0)

    bright = [
        0.0 if c >= profile_module.TILT_HIGH_HZ else -12.0
        for c in profile_module.THIRD_OCTAVE_CENTERS
    ]
    assert profile_module.band_tilt_db(bright) == pytest.approx(12.0)
    # Same tilt, different spectrum: a resonance in the wrong band with a hole
    # beside it cancels out of the tilt and has to survive in the magnitude.
    lumpy = list(flat)
    lumpy[3], lumpy[4] = 9.0, -9.0
    assert profile_module.band_tilt_db(lumpy) == pytest.approx(0.0, abs=1e-9)
    assert profile_module.band_shape_error_db(lumpy, flat) > 2.0

    assert profile_module.band_tilt_db(None) is None
    assert profile_module.band_shape_error_db([], [0.0]) is None


def test_a_band_that_decayed_on_only_one_side_is_left_out_of_the_decay_average():
    """`analyze_hit` reports None for a band with no energy; that is not agreement."""
    assert profile_module.mean_band_decay_delta([1.0, None, 3.0],
                                                [0.0, 2.0, None]) == pytest.approx(1.0)
    assert profile_module.mean_band_decay_delta([None], [1.0]) is None


def test_the_model_grid_is_not_measured_into_the_reference_it_is_measured_against():
    """`render-grid` writes into the same corpus; a profile is the target half of it."""
    shipped = [t["id"] for t in json.loads(
        (REFERENCE_DIR / "drums.json").read_text())["capture"]["timbres"]]
    measured = {r["timbre"] for r in json.loads(
        (REFERENCE_DIR / "drums.json").read_text())["rows"]}
    assert measured <= set(shipped)
    assert "model" not in measured


def _decaying_burst(sr: int, seconds: float, tau_s: float, seed: int = 0) -> np.ndarray:
    burst = np.random.default_rng(seed).normal(0, 0.2, int(sr * seconds))
    return burst * np.exp(-np.arange(len(burst)) / (tau_s * sr))


def test_a_hit_the_host_sounded_late_measures_the_same_as_one_it_sounded_on_time():
    """The window follows the strike, because a hosted plugin's does not follow the note-on.

    Measured on a sampled kit: the same key at six velocities started anywhere
    from 0 to 750 ms after its note-on. Anchoring on the note-on charges that
    latency to the instrument — time to peak comes back as the delay itself, and
    the leading silence dilutes the RMS that crest and level are read against.
    """
    sr = SR
    burst = _decaying_burst(sr, 0.4, 0.05)
    preroll = np.zeros(int(0.1 * sr))
    on_time = np.concatenate([preroll, burst])
    late = np.concatenate([preroll, np.zeros(int(0.25 * sr)), burst])

    a = profile_module.measure_hit(on_time, sr, 38, 100, preroll_s=0.1, gate_s=0.05)
    b = profile_module.measure_hit(late, sr, 38, 100, preroll_s=0.1, gate_s=0.05)

    assert a["onset_ms"] == pytest.approx(0.0, abs=2.0)
    assert b["onset_ms"] == pytest.approx(250.0, abs=2.0)
    assert b["attack_ms"] == pytest.approx(a["attack_ms"], abs=1.0)
    assert b["crest_db"] == pytest.approx(a["crest_db"], abs=0.5)
    assert b["level_db"] == pytest.approx(a["level_db"], abs=0.5)
    assert b["decay_ms"] == pytest.approx(a["decay_ms"], abs=2.0)


def test_a_wash_does_not_move_its_attack_when_ripple_moves_its_loudest_frame():
    """Time to the peak is not a statistic on a cymbal; time to arrival is.

    A crash holds within a couple of dB of its maximum for hundreds of
    milliseconds, so which frame carries the maximum is decided by noise. Two
    renders of the same gesture, differing only in where that ripple puts the
    maximum, have to report the same attack.
    """
    sr = SR
    n = int(sr * 1.2)
    wash = np.random.default_rng(2).normal(0, 0.2, n)
    wash *= np.minimum(1.0, np.arange(n) / (0.008 * sr))          # 8 ms strike
    wash *= np.exp(-np.arange(n) / (2.0 * sr))                    # then a long plateau

    def bump_at(seconds: float) -> np.ndarray:
        lift = np.ones(n)
        i = int(seconds * sr)
        lift[i:i + int(0.02 * sr)] = 1.15
        return np.concatenate([np.zeros(int(0.1 * sr)), wash * lift])

    early = profile_module.measure_hit(bump_at(0.02), sr, 49, 100,
                                       preroll_s=0.1, gate_s=0.05)
    late = profile_module.measure_hit(bump_at(0.40), sr, 49, 100,
                                      preroll_s=0.1, gate_s=0.05)

    assert late["attack_ms"] == pytest.approx(early["attack_ms"], abs=5.0)
    assert early["attack_ms"] < 30.0


def test_a_momentary_dip_does_not_end_a_ring_that_is_still_going():
    """Decay is the last moment above the threshold, not the first moment under it.

    A 2 ms window on a noise wash crosses -20 dB and comes straight back. Read
    as a first crossing, a hi-hat that rings for half a second reports 14 ms.
    """
    sr = SR
    n = int(sr * 1.0)
    ring = np.random.default_rng(3).normal(0, 0.2, n)
    ring *= np.exp(-np.arange(n) / (0.15 * sr))
    ring[int(0.05 * sr):int(0.052 * sr)] *= 0.001                 # one dropout frame
    audio = np.concatenate([np.zeros(int(0.1 * sr)), ring])

    row = profile_module.measure_hit(audio, sr, 46, 100, preroll_s=0.1, gate_s=0.05)

    # exp(-t/0.15) is 20 dB down — a tenth of the amplitude — at 0.15 * ln(10).
    assert row["decay_ms"] == pytest.approx(345.0, abs=30.0)


def test_a_hit_that_swells_keeps_its_onset_rather_than_being_cut_to_its_peak():
    """A crash and a vibraslap peak well after the strike; that is the instrument."""
    sr = SR
    swell = np.random.default_rng(1).normal(0, 0.2, int(sr * 0.9))
    ramp = np.minimum(1.0, np.arange(len(swell)) / (0.3 * sr))
    swell *= ramp * np.exp(-np.arange(len(swell)) / (0.6 * sr))
    audio = np.concatenate([np.zeros(int(0.1 * sr)), swell])

    row = profile_module.measure_hit(audio, sr, 58, 100, preroll_s=0.1, gate_s=0.05)

    assert row["onset_ms"] == pytest.approx(0.0, abs=5.0)
    assert row["attack_ms"] > 100.0


def test_measure_hit_reports_a_strike_and_not_a_fundamental():
    """The pitched columns are absent rather than present and meaningless."""
    sr = SR
    noise = np.random.default_rng(0).normal(0, 0.2, int(sr * 0.4))
    noise *= np.exp(-np.arange(len(noise)) / (0.05 * sr))
    audio = np.concatenate([np.zeros(int(0.1 * sr)), noise])

    row = profile_module.measure_hit(audio, sr, 38, 100, preroll_s=0.1, gate_s=0.05)

    assert row["peak_dbfs"] is not None and row["peak_dbfs"] < 0.0
    assert row["bands_db"] and max(row["bands_db"]) == pytest.approx(0.0)
    for pitched in ("f0_hz", "cents_vs_et", "inharmonicity_b", "partials_db"):
        assert pitched not in row


def test_the_two_captures_with_references_name_their_program_and_phrase_set():
    """These two fields are what stop an instrument being measured as another.

    Named rather than globbed, because the regression is a specific pairing:
    the harpsichord was measured against program 0 for as long as the program
    was a literal in `profile.py`.
    """
    from capture import load_config

    here = Path(__file__).resolve().parent
    for name, program, takes in (("piano", 0, "piano"), ("harpsichord", 6, "harpsichord")):
        cfg = load_config(here / "capture" / f"{name}.json")
        assert cfg["program"] == program
        assert cfg["takes"] == takes


def test_every_shipped_capture_names_the_program_it_answers_with():
    """Read from the file, not from `load_config`, which defaults it to 0.

    A capture that leaves the program out is not measured against nothing, it
    is measured against the piano — which is a plausible profile of the wrong
    instrument rather than a failure anyone would notice.
    """
    here = Path(__file__).resolve().parent / "capture"
    for name in shipped_captures():
        raw = json.loads((here / f"{name}.json").read_text())
        assert isinstance(raw.get("program"), int), f"{name} does not name its GM program"


def test_a_capture_naming_a_phrase_set_names_one_that_exists():
    """A typo here is otherwise found by rendering the whole audition first."""
    from make_audition import TAKE_SETS

    here = Path(__file__).resolve().parent / "capture"
    for name in shipped_captures():
        takes = json.loads((here / f"{name}.json").read_text()).get("takes")
        if takes:
            assert takes in TAKE_SETS, f"{name} names phrase set {takes!r}, which does not exist"


def test_an_audition_of_a_capture_with_no_phrase_set_is_refused(capsys):
    """Not rendered on the piano's phrases, which would look like it worked.

    The message is asserted, not just the exit code: `main` has other ways to
    return 2, and a test that accepts any of them would keep passing after the
    fallback came back.
    """
    import make_audition

    # The organ is the shipped capture with no phrase set. The kit used to be,
    # and now has one: a fit that moves one drum note at a time has no
    # measurement of what happens BETWEEN hits — the hi-hats' mute group, a
    # sixteenth pattern landing on its own ring — so the kit needed a listening
    # path more than most.
    assert "pipe_organ" in shipped_captures()
    argv = ["make_audition.py", "--config", str(Path(make_audition.__file__).resolve().parent
                                                / "capture" / "pipe_organ.json")]
    old = sys.argv
    sys.argv = argv
    try:
        assert make_audition.main() == 2
    finally:
        sys.argv = old
    assert "no phrase set" in capsys.readouterr().err


# --------------------------------------------------------------------------
# which dimensions an instrument is judged on


def test_no_declared_dimensions_means_every_measured_one():
    summary = {"decay": {}, "damper": {}}
    assert profile_module.select_dimensions(summary, []) == summary


def test_a_declared_list_narrows_the_summary():
    summary = {"decay": {}, "damper": {}, "tnr": {}}
    assert set(profile_module.select_dimensions(summary, ["decay", "tnr"])) == {"decay", "tnr"}


def test_a_declared_dimension_that_was_not_measured_is_named(capsys):
    """Dropping it silently reads afterwards as a dimension that came out fine."""
    profile_module.select_dimensions({"decay": {}}, ["decay", "damper"])
    assert "damper" in capsys.readouterr().err


def test_every_gate_dimension_has_a_floor_under_its_bound(tmp_path):
    """A bound at zero fails on measurement noise, and then it gets switched off."""
    summary = {k: {"median": 0.0, "abs_median": 0.0, "n": 4}
               for k in profile_module.DELTA_LABELS}
    gate = tmp_path / "gate.json"
    profile_module.write_gate_file(summary, gate, "ref", 1.25)
    bounds = json.loads(gate.read_text())["bounds"]
    assert set(bounds) == set(profile_module.DELTA_LABELS)
    assert all(b["median"] > 0.0 and b["abs_median"] > 0.0 for b in bounds.values())


# --------------------------------------------------------------------------- #
# The captured layout, and the model's
# --------------------------------------------------------------------------- #
def test_a_capture_may_state_which_model_note_answers_each_of_its_own():
    """A drum note names an instrument, and a sampled kit need not use GM's order.

    The kit measured for `reference/drums.json` does not: its toms ascend
    45, 47, 48, 50, 41, 43. Without a map, a note-for-note comparison scores the
    low floor tom against the high one and reports a tuning error that is a
    mapping.
    """
    assert note_map({}) == {}
    assert note_map({"note_map": {"41": 45, "43": 47}}) == {41: 45, 43: 47}
    # Keys arrive from JSON as strings; both ends come back as ints so a caller
    # can look up a MIDI note without knowing where the config came from.
    mapped = note_map({"note_map": {"41": 45}})
    assert all(isinstance(k, int) and isinstance(v, int) for k, v in mapped.items())


def test_every_shipped_capture_s_note_map_names_notes_it_actually_captured():
    """A map entry for a note outside the grid is a typo that silently does nothing."""
    for name in shipped_captures():
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        mapping = note_map(cfg)
        if not mapping:
            continue
        captured = set(cfg.get("notes") or [])
        assert captured, f"{name} maps notes but lists none"
        unknown = sorted(set(mapping) - captured)
        assert not unknown, f"{name} maps notes it never captured: {unknown}"


# --------------------------------------------------------------------------- #
# The families inside a capture
# --------------------------------------------------------------------------- #
def test_every_shipped_tail_override_names_notes_the_capture_actually_holds():
    """A longer tail for a note the grid never records renders nothing at all.

    It reads as a decision — the belltree was named for ten seconds and the
    standard kit stops at 81 — and nothing in a capture run reports it, since
    the table is consulted per note of the grid and a note outside it is never
    looked up.
    """
    for name in shipped_captures():
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        table = cfg.get("tail_by_note") or {}
        if not table:
            continue
        captured = set(cfg.get("notes") or [])
        named: set[int] = set()
        for key in table:
            for part in str(key).split(","):
                part = part.strip()
                if "-" in part:
                    lo, hi = part.split("-", 1)
                    named |= set(range(int(lo), int(hi) + 1))
                elif part:
                    named.add(int(part))
        unknown = sorted(named - captured)
        assert not unknown, f"{name} gives a tail to notes it never captures: {unknown}"


def test_every_shipped_family_names_notes_the_capture_actually_holds():
    """A family naming a note outside the grid loses that member in silence."""
    for name in shipped_captures():
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        groups = note_groups(cfg)
        if not groups:
            continue
        captured = set(cfg.get("notes") or [])
        for family, notes in groups.items():
            assert len(notes) >= 2, f"{name}/{family} is not a family"
            unknown = sorted(set(notes) - captured)
            assert not unknown, f"{name}/{family} names uncaptured notes: {unknown}"


def test_a_reference_scores_no_kit_relation_against_itself():
    """The identity value, before any sweep of the term means anything.

    A relation term compares two contrast vectors, and a contrast is a
    subtraction against a median — arithmetic with several ways to come out
    non-zero on identical input. Exactly zero over a non-zero count is the only
    reading that says the term is measuring a difference rather than a method.
    """
    for name in shipped_captures():
        reference = REFERENCE_DIR / f"{name}.json"
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        groups = note_groups(cfg)
        if not groups or not reference.exists():
            continue
        rows = json.loads(reference.read_text())["rows"]
        value, count = _kit_terms(rows, rows, groups)
        assert count > 0, f"{name} declares families and none could be measured"
        assert value == 0.0


def test_every_shipped_family_holds_at_least_one_relation_in_its_own_rows():
    """A family whose members the reference cannot tell apart is not a family.

    Which relations a family has is measured rather than declared, so a group
    that survived nothing is one whose members are interchangeable in this
    capture — the whistle pair, whose lengths are the capture's gate — and
    declaring it puts a name in the file that scores nothing.
    """
    for name in shipped_captures():
        reference = REFERENCE_DIR / f"{name}.json"
        cfg = json.loads((CAPTURE_DIR / f"{name}.json").read_text())
        groups = note_groups(cfg)
        if not groups or not reference.exists():
            continue
        rows = json.loads(reference.read_text())["rows"]
        held = {row["family"] for row in kit_report(rows, rows, groups)}
        assert set(groups) == held, f"{name}: {sorted(set(groups) - held)} hold nothing"


# --------------------------------------------------------------------------- #
# The dimension that sees gain
# --------------------------------------------------------------------------- #
def test_the_compare_table_has_a_dimension_that_moves_when_a_gain_does():
    """Every other column is normalised, and so is blind to output level.

    Rewriting eighteen of the kit's output levels moved not one of the others by
    a digit. `vel_range` is a span and cancels an offset by construction, so it
    is not this either.
    """
    assert "level" in profile_module.DELTA_LABELS
    normalised = {"band_tilt", "band_shape", "band_decay", "attack", "crest",
                  "centroid_pct", "vel_range"}
    assert "level" not in normalised


# --------------------------------------------------------------------------
# the model is rendered over the window its reference was captured in


def test_the_model_grid_is_rendered_over_each_note_s_own_tail(tmp_path, monkeypatch):
    """A kit records eight seconds for a ride and two for a kick; so must the model.

    Both sides of every band and decay column are measured over a window, and
    the columns only mean something when it is the same window. Rendering the
    model at one flat length against a grid captured at several compares two
    seconds of model against two seconds of reference on the short notes and
    against eight on the long ones, where the six seconds the model never
    rendered read as a wash that died.
    """
    asked: dict[int, float] = {}

    def fake_render(smf, seconds, sr):
        asked[fake_render.note] = seconds
        return np.zeros((int(seconds * sr), 1), dtype=np.float32)

    monkeypatch.setattr(profile_module, "render_model", fake_render)
    monkeypatch.setattr(profile_module, "write_wav", lambda *a, **k: None)

    real_smf = profile_module.write_smf

    def spy_smf(notes, **kw):
        fake_render.note = notes[0].note
        return real_smf(notes, **kw)

    monkeypatch.setattr(profile_module, "write_smf", spy_smf)

    cfg = {"id": "kit", "notes": [35, 51, 81], "velocities": [100],
           "sample_rate": 48000, "gate_ms": 50, "preroll_ms": 100,
           "tail": "2s", "tail_by_note": {"49-59": "8s", "80-81": "6s"},
           "channel": 10, "timbres": [{"id": "t", "channel": 10}]}
    profile_module.render_grid(cfg, tmp_path, timbre="model", program=0)

    # preroll 0.1 + gate 0.05 + the note's own tail.
    assert asked[35] == pytest.approx(2.15)
    assert asked[51] == pytest.approx(8.15)
    assert asked[81] == pytest.approx(6.15)

    manifest = json.loads((tmp_path / "manifest.json").read_text())
    recorded = {r["note"]: r["seconds"] for r in manifest["renders"]}
    # And the manifest records it, which is what `load_corpus` reads the
    # analysis window back out of.
    assert recorded == {35: pytest.approx(2.15), 51: pytest.approx(8.15),
                        81: pytest.approx(6.15)}
