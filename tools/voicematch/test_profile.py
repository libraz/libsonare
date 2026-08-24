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

    assert "drums" in shipped_captures()
    argv = ["make_audition.py", "--config", str(Path(make_audition.__file__).resolve().parent
                                                / "capture" / "drums.json")]
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
