"""Which reference a voice can actually be compared against, and the
tolerances an agreement is read against.

    rye run --pyproject bindings/python/pyproject.toml \\
        python -m pytest tools/voicematch/test_profile_reference.py -q
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import profile as profile_module


def _audition_capture(cap_id: str, *, plugin: str = "") -> object:
    from bank import Capture

    raw = {"id": cap_id, "plugin": plugin} if plugin else {"id": cap_id}
    return Capture(
        path=Path(f"{cap_id}.json"),
        id=cap_id,
        label=cap_id,
        program=0,
        bank=0,
        take_set="drums",
        timbres=(),
        dry=True,
        title=cap_id,
        source_class=None,
        raw=raw,
    )


def test_a_capture_that_can_render_no_reference_does_not_represent_the_page(tmp_path):
    """The page's representative is whichever capture can actually play one.

    The kit is the live case: the policy aims a kit at the machine, so the
    module captures lead, and a capture imported from a file names no plugin.
    Left alone the most-calibrated voice in the bank renders model-only while
    its library reference sits in the archive.
    """
    import make_audition
    from bank import Voice

    module, library = _audition_capture("m"), _audition_capture("l", plugin="a:b:c")
    voice = Voice(program=0, kit=True, captures=(module, library))
    assert voice.capture is module
    assert make_audition.playable_first(voice, tmp_path).capture is library


def test_the_archive_is_the_other_route_to_a_reference(tmp_path):
    """A capture with no plugin still represents the page when takes are held.

    Otherwise this reorders away from the layer the policy asked for on the
    strength of a plugin nobody needed.
    """
    import make_audition
    from bank import Voice

    module, library = _audition_capture("m"), _audition_capture("l", plugin="a:b:c")
    (tmp_path / "m").mkdir()
    voice = Voice(program=0, kit=True, captures=(module, library))
    assert make_audition.playable_first(voice, tmp_path).capture is module
    # And with no archive at all the plugin is the only route left.
    assert make_audition.playable_first(voice, None).capture is library


def test_a_voice_whose_captures_can_all_supply_one_is_left_in_policy_order(tmp_path):
    """The reorder must not become a second opinion about which layer wins."""
    import make_audition
    from bank import Voice

    first = _audition_capture("first", plugin="a:b:c")
    second = _audition_capture("second", plugin="d:e:f")
    voice = Voice(program=0, captures=(first, second))
    assert make_audition.playable_first(voice, tmp_path).captures == (first, second)


def _hit(**kw) -> dict:
    """A measured percussion cell, with every field `agreement_row` reads."""
    base = {
        "peak_dbfs": -10.0,
        "bands_db": [-20.0] * 8,
        "centroid_hz": 400.0,
        "attack_ms": 5.0,
        "crest_db": 12.0,
        "band_decay_db_s": -40.0,
        "decay_ms": 200.0,
        "decay_capped": False,
    }
    return {**base, **kw}


def test_every_agreement_tolerance_has_a_delta_behind_it():
    """A tolerance with no row entry reads as a dimension nobody disagreed on.

    `agree` indexes the row with `row[key]`, so the two key sets drifting apart
    is a KeyError rather than a silent pass — but only if they are compared
    somewhere, which is here.
    """
    row = profile_module.agreement_row(_hit(), _hit())
    assert set(row) == set(profile_module.AGREEMENT_TOLERANCE)


def test_the_two_percussion_dimensions_are_measured_and_bounded():
    """Neither was reachable before, so a ring regression could fail nothing.

    The widths are the two references' own disagreement over the kit grid, as
    recorded in `capture/drums.json`.
    """
    assert profile_module.AGREEMENT_TOLERANCE["ring"] == 0.69
    assert profile_module.AGREEMENT_TOLERANCE["band_decay"] == 41.7
    row = profile_module.agreement_row(_hit(decay_ms=400.0, band_decay_db_s=-10.0), _hit())
    assert row["ring"] == pytest.approx(1.0)
    assert row["band_decay"] == pytest.approx(30.0)


def test_a_capped_decay_yields_no_ring_on_either_side():
    """A capped decay is the analysis window, not the hit.

    Measured live: one drum note's model never fell 20 dB inside the only window
    covering it, so its length is a bound of the window. Compared as a ratio it
    would read as a confident agreement with whatever the other side happened to
    be.
    """
    assert profile_module.agreement_row(_hit(decay_capped=True), _hit())["ring"] is None
    assert profile_module.agreement_row(_hit(), _hit(decay_capped=True))["ring"] is None


def test_a_melodic_cell_reports_the_percussion_dimensions_as_unmeasured():
    """They must come back None rather than raising, and count nothing."""
    melodic = {
        "peak_dbfs": -10.0,
        "bands_db": [-20.0] * 8,
        "centroid_hz": 400.0,
        "attack_ms": 5.0,
        "crest_db": 12.0,
    }
    row = profile_module.agreement_row(melodic, melodic)
    assert row["ring"] is None and row["band_decay"] is None


def test_an_audition_of_a_capture_with_no_phrase_set_is_refused(capsys, tmp_path):
    """Not rendered on the piano's phrases, which would look like it worked.

    The message is asserted, not just the exit code: `main` has other ways to
    return 2, and a test that accepts any of them would keep passing after the
    fallback came back.
    """
    import make_audition

    # The config is built here rather than borrowed from the shipped set, and
    # what that costs is worth paying: every shipped capture now names a phrase
    # set, so a test anchored on whichever one did not would stop testing the
    # refusal the day that capture gained its phrases — and, because nothing
    # short of the refusal stops `main`, would render the whole audition for
    # real, through the plugin, into the shared scratch directory.
    source = Path(make_audition.__file__).resolve().parent / "capture" / "pipe_organ.json"
    cfg = json.loads(source.read_text())
    cfg.pop("takes", None)
    cfg.pop("_takes", None)
    config = tmp_path / "no_phrase_set.json"
    config.write_text(json.dumps(cfg))
    argv = ["make_audition.py", "--config", str(config), "--out", str(tmp_path / "out")]
    old = sys.argv
    sys.argv = argv
    try:
        assert make_audition.main() == 2
    finally:
        sys.argv = old
    assert "no phrase set" in capsys.readouterr().err


def test_a_capture_run_writes_under_its_voice_and_not_at_the_root(tmp_path):
    """A single-voice page must not land on the path every other one uses.

    `--config` once wrote flat, straight into `--out`, so the second instrument
    auditioned took the first one's manifest and left that page's takes on disk
    with nothing to name or group them. Asserted on the layout rather than on
    the flag, because what matters is that two voices can coexist under one
    root; `main` is not run here, since a real run renders through the library.
    """
    import make_audition

    source = Path(make_audition.__file__).resolve().parent / "capture" / "pipe_organ.json"
    cfg = json.loads(source.read_text())
    root = tmp_path / "out"
    written: list[Path] = []
    argv = ["make_audition.py", "--config", str(source), "--out", str(root)]

    def fake_render_set(voice, out, args, table, extra):
        written.append(Path(out))
        return 1

    old_argv, old_render = sys.argv, make_audition.render_set
    sys.argv = argv
    make_audition.render_set = fake_render_set
    try:
        assert make_audition.main() == 0
    finally:
        sys.argv, make_audition.render_set = old_argv, old_render

    assert written, "the run selected no voice"
    for out in written:
        assert out.parent == root.resolve(), f"{out} is not a voice directory under {root}"
        assert out != root.resolve(), "the page was written flat at the root"
    assert (root / "bank.json").exists(), "the merged index was skipped"
    assert cfg["program"] == 19
