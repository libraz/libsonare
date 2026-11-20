"""Tests for newly exposed snake_case Mixer methods in the Python binding."""

from __future__ import annotations

import json
import math
import re
from pathlib import Path

import pytest

from libsonare import ErrorCode, SonareError, SonareValueError
from libsonare._runtime import _curve_value, _pan_law_value

from ._helpers import LIB_AVAILABLE, raises_coded

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not found")

_PAN_LAW_CORPUS = json.loads(
    (Path(__file__).resolve().parents[3] / "tests/conformance/pan_law_names.json").read_text(
        encoding="utf-8"
    )
)
_AUTOMATION_CURVE_CORPUS = json.loads(
    (
        Path(__file__).resolve().parents[3] / "tests/conformance/automation_curve_names.json"
    ).read_text(encoding="utf-8")
)


def _first_preset_json() -> str:
    """Return the JSON scene of the first built-in mixing preset."""
    from libsonare._mixing import mixing_scene_preset, mixing_scene_preset_names

    names = mixing_scene_preset_names()
    assert names, "expected at least one built-in mixing preset"
    return json.dumps(mixing_scene_preset(names[0]))


def _serial_tail_scene() -> str:
    def delay(ms: int) -> dict[str, object]:
        return {
            "slot": "post",
            "processor": "effects.delay.stereo",
            "params": {"delayTimeLMs": ms, "delayTimeRMs": ms, "feedback": 0, "dryWet": 1},
        }

    return json.dumps(
        {
            "version": 2,
            "strips": [
                {
                    "id": "source",
                    "inserts": [delay(10)],
                    "sends": [
                        {
                            "id": "to-aux",
                            "destinationBusId": "aux",
                            "sendDb": 0,
                            "timing": "postFader",
                        }
                    ],
                }
            ],
            "buses": [
                {"id": "aux", "role": "aux", "inserts": [delay(20)]},
                {"id": "master", "role": "master", "inserts": [delay(30)]},
            ],
            "connections": [{"source": "aux", "destination": "master"}],
        }
    )


@pytest.fixture()
def mixer():
    """Build a Mixer from the first preset scene and close it afterwards."""
    from libsonare import Mixer

    mixer = Mixer.from_json(_first_preset_json(), sample_rate=48000, max_block_size=256)
    try:
        yield mixer
    finally:
        mixer.close()


def _process_one_block(mixer) -> None:
    """Feed one block of audio through every strip so meters/goniometer fill."""
    n = mixer.strip_count
    block = [[0.2 * math.sin(2 * math.pi * 440 * i / 48000) for i in range(256)] for _ in range(n)]
    mixer.process_stereo(block, block)


def test_preset_scene_builds_mixer(mixer) -> None:
    """A mixer built from the first preset exposes the expected strips."""
    assert mixer.strip_count == 2
    scene = json.loads(mixer.to_json())
    assert len(scene["strips"]) == 2


def test_strip_by_id_returns_index_and_raises_on_unknown(mixer) -> None:
    """strip_by_id resolves a known id to its index; an unknown id is a refused argument."""
    index = mixer.strip_by_id("vocal")
    assert isinstance(index, int)
    assert 0 <= index < mixer.strip_count
    with pytest.raises(
        SonareValueError, match="^Mixer.strip_by_id: id must be the id of a strip in this mixer$"
    ):
        mixer.strip_by_id("does-not-exist")
    # A strip reference taken by any other method is refused the same way.
    with pytest.raises(
        SonareValueError, match="^Mixer.set_soloed: strip must be the id of a strip in this mixer$"
    ):
        mixer.set_soloed("does-not-exist", True)
    with pytest.raises(
        SonareValueError, match=rf"strip must be an integer in \[0, {mixer.strip_count - 1}\]$"
    ):
        mixer.set_soloed(mixer.strip_count, True)


def test_add_strip_returns_the_new_strip_index(mixer) -> None:
    """add_strip answers the index strip_by_id would, as the JS addStrip does."""
    index = mixer.add_strip(id="py-indexed-strip")
    assert index == mixer.strip_by_id(id="py-indexed-strip")
    assert index == mixer.strip_count - 1


def test_strip_meter_returns_snapshots(mixer) -> None:
    """strip_meter returns a MixMeterSnapshot with numeric fields at either tap."""
    from libsonare.types import MixMeterSnapshot

    _process_one_block(mixer)

    snapshot = mixer.strip_meter("vocal")
    assert isinstance(snapshot, MixMeterSnapshot)
    for value in (
        snapshot.peak_db_l,
        snapshot.peak_db_r,
        snapshot.rms_db_l,
        snapshot.rms_db_r,
        snapshot.correlation,
        snapshot.momentary_lufs,
        snapshot.short_term_lufs,
        snapshot.integrated_lufs,
        snapshot.true_peak_dbtp_l,
        snapshot.max_true_peak_dbtp,
    ):
        assert isinstance(value, float)
        assert math.isfinite(value)
    assert isinstance(snapshot.likely_mono_compatible, bool)
    assert isinstance(snapshot.seq, int)

    # The tap is a token; an ordinal is refused.
    assert isinstance(mixer.strip_meter(0, "preFader"), MixMeterSnapshot)
    assert not hasattr(mixer, "meter_tap")
    with pytest.raises(TypeError):
        mixer.strip_meter(0, 0)  # type: ignore[arg-type]
    assert isinstance(mixer.strip_meter(0, "preFader"), MixMeterSnapshot)
    master = mixer.bus_meter("master")
    assert isinstance(master, MixMeterSnapshot)
    assert master.num_channels == 2
    assert math.isfinite(master.peak_db[0])


def test_strip_non_finite_discard_count_stays_zero_on_clean_audio(mixer) -> None:
    """A strip processing only ordinary-level audio reports a zero discard count.

    The preset scene's strips carry no insert extreme enough to overflow, so
    this exercises the accessor's wiring against a clean run; see
    ``test_strip_non_finite_discard_count_rises_after_an_insert_overflow``
    below for a case that actually drives the counter.
    """
    assert mixer.strip_non_finite_discard_count("vocal") == 0
    _process_one_block(mixer)
    assert mixer.strip_non_finite_discard_count("vocal") == 0
    assert mixer.strip_non_finite_discard_count(0) == 0


def test_bus_non_finite_discard_count_rejects_an_uncompiled_bus(mixer) -> None:
    """A declared-but-uncompiled bus raises rather than reading as a clean zero.

    A bus's DSP record only exists after the graph has been compiled, so
    reading the count before that would either crash or have to fabricate a
    zero that reads as "clean" for a bus that was never actually processed.
    """
    mixer.add_bus("py-scratch-bus", "aux")
    with raises_coded():
        mixer.bus_non_finite_discard_count("py-scratch-bus")

    mixer.compile()
    assert mixer.bus_non_finite_discard_count("py-scratch-bus") == 0


def test_bus_non_finite_discard_count_rejects_an_unknown_bus_id(mixer) -> None:
    with raises_coded():
        mixer.bus_non_finite_discard_count("no-such-bus")


def test_bus_non_finite_discard_count_stays_zero_on_clean_audio(mixer) -> None:
    """The preset scene's master bus is compiled by the first processed block."""
    _process_one_block(mixer)
    assert mixer.bus_non_finite_discard_count("master") == 0


def _scene_with_pre_eq(gain_db: float, *, on_bus: bool) -> str:
    """Build a one-strip scene with an eq.parametric pre-insert.

    Placed on the strip by default, or on the master bus when ``on_bus`` is
    set -- the strip still has to carry audio into it either way.
    """
    insert = {
        "slot": "pre",
        "processor": "eq.parametric",
        "params": {"band0.frequencyHz": 1000, "band0.gainDb": gain_db, "band0.q": 2},
    }
    bus = {"id": "master", "role": "master"}
    strip = {"id": "a"}
    if on_bus:
        bus["inserts"] = [insert]
    else:
        strip["inserts"] = [insert]
    return json.dumps(
        {
            "version": 2,
            "buses": [bus],
            "strips": [strip],
            "connections": [{"source": "a", "destination": "master"}],
        }
    )


def test_strip_non_finite_discard_count_rises_after_an_insert_overflow() -> None:
    """A pre-insert whose own coefficients overflow float32 moves the count.

    ``process_stereo`` only rejects a block that already carries a
    non-finite sample; a constant 1e38 fill is finite and under FLT_MAX
    (~3.4e38), so it passes. A +200 dB peaking band scales its own
    feedforward coefficients large enough that multiplying by that sample
    overflows to infinity inside the insert's own recursive state -- the
    corruption is produced by the chain, not carried in by the caller,
    which is exactly the failure mode this counter exists to catch.
    """
    from libsonare import Mixer

    mixer = Mixer.from_json(
        _scene_with_pre_eq(200.0, on_bus=False), sample_rate=48000, max_block_size=64
    )
    try:
        block = [[0.3 * math.sin(2 * math.pi * 440 * i / 48000) for i in range(64)]]
        # Control: an ordinary block leaves the count at zero, so the rise
        # below is attributable to the poison rather than to processing at all.
        mixer.process_stereo(block, block)
        assert mixer.strip_non_finite_discard_count("a") == 0

        poison = [[1.0e38] * 64]
        mixer.process_stereo(poison, poison)
        assert mixer.strip_non_finite_discard_count("a") > 0
    finally:
        mixer.close()


def test_bus_non_finite_discard_count_rises_after_an_insert_overflow() -> None:
    """Same overflow mechanism as the strip case, for a bus's own insert."""
    from libsonare import Mixer

    mixer = Mixer.from_json(
        _scene_with_pre_eq(200.0, on_bus=True), sample_rate=48000, max_block_size=64
    )
    try:
        block = [[0.3 * math.sin(2 * math.pi * 440 * i / 48000) for i in range(64)]]
        mixer.process_stereo(block, block)
        assert mixer.bus_non_finite_discard_count("master") == 0

        poison = [[1.0e38] * 64]
        mixer.process_stereo(poison, poison)
        assert mixer.bus_non_finite_discard_count("master") > 0
    finally:
        mixer.close()


def test_strip_meter_rejects_invalid_tap(mixer) -> None:
    """An unknown meter tap name raises ValueError."""
    with pytest.raises(ValueError):
        mixer.strip_meter("vocal", "sideways")


def test_read_goniometer_latest_returns_points(mixer) -> None:
    """read_goniometer_latest returns GoniometerPoint values with left/right floats."""
    from libsonare.types import GoniometerPoint

    # No audio processed yet -> empty, and a non-positive request is empty too.
    assert mixer.read_goniometer_latest("vocal", 0) == []
    assert mixer.read_goniometer_latest("vocal", 16) == []

    _process_one_block(mixer)
    points = mixer.read_goniometer_latest("vocal", 32)
    assert isinstance(points, list)
    assert len(points) > 0
    for point in points:
        assert isinstance(point, GoniometerPoint)
        assert isinstance(point.left, float)
        assert isinstance(point.right, float)


def test_read_goniometer_latest_bounds_the_buffer_independently_of_max_points(mixer) -> None:
    """max_points is a request, not an allocation size.

    A ctypes array sized straight from the caller's number turned a metering UI
    deriving the count from a window size into a multi-gigabyte allocation. The
    working buffer is bounded by the strip's goniometer ring instead. Matches
    the Node and WASM facades.
    """
    _process_one_block(mixer)

    huge = mixer.read_goniometer_latest("vocal", 2**40)
    assert isinstance(huge, list)
    assert 0 < len(huge) <= 4096

    # Not a finite non-negative integer -> a catchable error, never a silent
    # clamp to zero, matching what Node and WASM do with the same value.
    for invalid in (-1, -4096, 1.5):
        with pytest.raises(
            ValueError,
            match=r"^Mixer\.read_goniometer_latest: max_points must be an integer in \[0, ",
        ):
            mixer.read_goniometer_latest("vocal", invalid)
    with pytest.raises(TypeError, match="max_points"):
        mixer.read_goniometer_latest("vocal", True)

    # Zero stays a legal empty read.
    assert mixer.read_goniometer_latest("vocal", 0) == []


def test_set_pan_law_takes_exact_tokens(mixer) -> None:
    """set_pan_law takes the four exact tokens; an ordinal is a TypeError."""
    for token in ("const3Db", "const4.5Db", "const6Db", "linear0Db"):
        mixer.set_pan_law(0, token)
    with pytest.raises(SonareValueError, match="const3Db"):
        mixer.set_pan_law(0, "not-a-pan-law")
    for value in (2, True, 2.0):
        with pytest.raises(TypeError):
            mixer.set_pan_law(0, value)


def test_strip_pan_and_width_refusals_are_value_errors(mixer) -> None:
    """An out-of-domain pan or width raises SonareValueError on every strip path."""
    for call in (
        lambda: mixer.set_pan(0, 1.5),
        lambda: mixer.set_pan(0, -1.01),
        lambda: mixer.set_dual_pan(0, -1.5, 0.0),
        lambda: mixer.set_width(0, 3.0),
        lambda: mixer.set_width(0, -0.1),
        lambda: mixer.schedule_pan_automation(0, 0, 1.5),
        lambda: mixer.schedule_width_automation(0, 0, 2.5),
    ):
        with pytest.raises(SonareValueError):
            call()


def test_set_pan_law_matches_shared_name_corpus(mixer) -> None:
    """Each exact token maps to the shared ordinal; other spellings and numbers are refused."""
    for case in _PAN_LAW_CORPUS["accepted"]:
        mixer.set_pan_law(0, case["value"])
        assert _pan_law_value(case["value"], "pan_law") == case["ordinal"], case["value"]
    for value in _PAN_LAW_CORPUS["rejected"]:
        with pytest.raises(SonareValueError):
            mixer.set_pan_law(0, value)
    for value in _PAN_LAW_CORPUS["rejected_numeric"]:
        with pytest.raises(TypeError):
            mixer.set_pan_law(0, value)


def test_simple_strip_setters_reach_the_scene(mixer) -> None:
    """The simple per-strip setters are observable through the shipped readback.

    Channel delay is the multi-mic phase-alignment control: a silently applied
    0 instead of 8 samples has no symptom until someone compares a final mix.
    Both values here are distinguishable from the scene defaults (0 and 0), so
    a setter reduced to a no-op fails rather than passing on the default.
    """
    mixer.set_polarity_invert("vocal", True, False)
    mixer.set_channel_delay_samples("vocal", 8)
    mixer.set_vca_offset_db("vocal", -2.0)
    mixer.set_dual_pan("vocal", -0.3, 0.4)

    scene = json.loads(mixer.to_json())
    vocal = next(strip for strip in scene["strips"] if strip["id"] == "vocal")
    assert vocal["channelDelaySamples"] == 8
    assert vocal["vcaOffsetDb"] == pytest.approx(-2.0)
    assert vocal["polarityInvertLeft"] is True
    assert vocal["polarityInvertRight"] is False
    assert vocal["dualPanLeft"] == pytest.approx(-0.3)
    assert vocal["dualPanRight"] == pytest.approx(0.4)


def test_asymmetric_strip_controls_round_trip_in_scene_json(mixer) -> None:
    """Asymmetric polarity and dual-pan controls survive public scene serialization."""
    mixer.set_polarity_invert("vocal", True, False)
    mixer.set_dual_pan("vocal", 0.75, -0.25)

    scene = json.loads(mixer.to_json())
    vocal = next(strip for strip in scene["strips"] if strip["id"] == "vocal")
    assert vocal["polarityInvertLeft"] is True
    assert vocal["polarityInvertRight"] is False
    assert vocal["dualPanLeft"] == pytest.approx(0.75)
    assert vocal["dualPanRight"] == pytest.approx(-0.25)


_SETTLE_SCENE = json.dumps(
    {
        "version": 2,
        "buses": [{"id": "master", "role": "master", "inserts": []}],
        "connections": [{"source": "s", "destination": "master"}],
        "strips": [
            {
                "id": "s",
                "inputTrimDb": 0,
                "faderDb": -3,
                "pan": 0.3,
                "panMode": "balance",
                "panLaw": "const3Db",
                "width": 1.2,
                "channelDelaySamples": 0,
                "dualPanLeft": -1,
                "dualPanRight": 1,
                "inserts": [],
                "muted": False,
                "polarityInvertLeft": False,
                "polarityInvertRight": False,
                "sends": [],
                "soloSafe": False,
                "soloed": False,
                "vcaOffsetDb": 0,
            }
        ],
        "vcaGroups": [],
    }
)


def _render_settle_scene(*, settle: bool, length: int = 4096) -> tuple[list[float], list[float]]:
    """Render one block of steady asymmetric DC through the one-strip settle scene.

    The input is constant and the two sides differ, so mid and side are both
    steady and the converged output is a single well-defined number per side --
    which pan (0.3) and width (1.2) both move away from their smoother's
    starting point.
    """
    from libsonare import Mixer

    mixer = Mixer.from_json(_SETTLE_SCENE, sample_rate=48000, max_block_size=length)
    try:
        if settle:
            mixer.settle("s")
        result = mixer.process_stereo([[1.0] * length], [[0.5] * length])
        return list(result.left), list(result.right)
    finally:
        mixer.close()


def test_settle_opens_a_render_at_the_converged_gain() -> None:
    """settle removes the head ramp an offline render otherwise carries.

    Both directions are asserted, because either one alone is satisfied by a
    settle that does nothing. The unsettled render must show the glide -- if it
    did not, the scene would not be reaching the smoothers at all and the
    settled assertions would pass over a strip that never ramps. The converged
    value is taken from the unsettled render's own tail, so the settled head is
    compared against an independently produced number rather than itself.
    """
    ramp_l, ramp_r = _render_settle_scene(settle=False)
    settled_l, settled_r = _render_settle_scene(settle=True)
    converged_l, converged_r = ramp_l[-1], ramp_r[-1]

    # Without settle the block opens roughly 7 dB hot on the left and 3 dB on
    # the right (measured 2.257x / 1.443x), and the image swings with it.
    assert ramp_l[0] / converged_l > 2.0
    assert ramp_r[0] / converged_r > 1.4

    # With settle the first sample is already at that converged value, and the
    # block holds it -- no glide anywhere in it.
    assert settled_l[0] == pytest.approx(converged_l, rel=1e-3)
    assert settled_r[0] == pytest.approx(converged_r, rel=1e-3)
    assert max(abs(value - settled_l[0]) for value in settled_l) < 1e-6
    assert max(abs(value - settled_r[0]) for value in settled_r) < 1e-6


def test_settle_leaves_the_serialized_scene_unchanged() -> None:
    """settle moves smoothers to values the scene already records, so it clears nothing."""
    from libsonare import Mixer

    mixer = Mixer.from_json(_SETTLE_SCENE, sample_rate=48000, max_block_size=256)
    try:
        before = json.loads(mixer.to_json())
        mixer.settle("s")
        assert json.loads(mixer.to_json()) == before
    finally:
        mixer.close()


def test_set_surround_pan_reflected_in_scene(mixer) -> None:
    """set_surround_pan stores azimuth/divergence/lfe on the strip's scene object."""
    mixer.set_surround_pan("vocal", azimuth=-45.0, divergence=0.25, lfe=0.5)
    scene = json.loads(mixer.to_json())
    by_id = {strip["id"]: strip for strip in scene["strips"]}
    pan = by_id["vocal"]["surroundPan"]
    assert pan["azimuth"] == pytest.approx(-45.0)
    assert pan["divergence"] == pytest.approx(0.25)
    assert pan["lfe"] == pytest.approx(0.5)


@pytest.mark.parametrize(
    "field", [{"azimuth": 181.0}, {"azimuth": -181.0}, {"divergence": 1.5}, {"lfe": -0.1}]
)
def test_set_surround_pan_refuses_out_of_range_fields(mixer, field) -> None:
    before = mixer.to_json()
    with raises_coded():
        mixer.set_surround_pan("vocal", **field)
    assert mixer.to_json() == before


def test_soloed_and_solo_safe_reflected_in_scene(mixer) -> None:
    """set_soloed / set_solo_safe are reflected in the re-serialized scene."""
    scene_before = json.loads(mixer.to_json())
    assert all(not strip["soloed"] for strip in scene_before["strips"])

    mixer.set_soloed("vocal", True)
    mixer.set_solo_safe("vocal-verb-return", True)

    scene_after = json.loads(mixer.to_json())
    by_id = {strip["id"]: strip for strip in scene_after["strips"]}
    assert by_id["vocal"]["soloed"] is True
    assert by_id["vocal-verb-return"]["soloSafe"] is True


def test_sends_add_and_set_level(mixer) -> None:
    """add_send returns an int index that set_send_db then accepts."""
    index = mixer.add_send("vocal", "extra-send", "vocal-verb", send_db=-12.0, timing="postFader")
    assert isinstance(index, int)
    assert index >= 0
    mixer.set_send_db("vocal", index, -6.0)


def test_remove_send_drops_a_send_and_shifts_indices(mixer) -> None:
    """remove_send drops a send; later sends shift down and the index goes stale."""
    i0 = mixer.add_send("vocal", "rm-send-0", "vocal-verb", send_db=-12.0, timing="postFader")
    i1 = mixer.add_send("vocal", "rm-send-1", "vocal-verb", send_db=-9.0, timing="postFader")
    assert i1 == i0 + 1

    mixer.remove_send("vocal", i0)
    # The surviving send shifted down to i0; the old top index is now out of range.
    mixer.set_send_db("vocal", i0, -6.0)
    with raises_coded():
        mixer.set_send_db("vocal", i1, -6.0)


def test_add_send_takes_exact_send_timing_tokens(mixer) -> None:
    """add_send takes postFader / preFader; another spelling or a number is refused."""

    i0 = mixer.add_send("vocal", "send-post", "vocal-verb", timing="postFader")
    i1 = mixer.add_send("vocal", "send-pre", "vocal-verb", timing="preFader")
    assert all(isinstance(i, int) and i >= 0 for i in (i0, i1))
    for other in ("pre_fader", "pre-fader", "sideways"):
        with pytest.raises(SonareValueError, match="preFader"):
            mixer.add_send("vocal", "send-bad", "vocal-verb", timing=other)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        mixer.add_send("vocal", "send-int", "vocal-verb", timing=1)  # type: ignore[arg-type]


def test_strip_can_be_added_to_a_live_mixer(mixer) -> None:
    """add_strip grows the strip set after construction, as add_bus does for buses.

    The strip set used to be fixed at whatever the scene document declared: the
    only way to reach a new strip was to serialize, edit the JSON and build a
    DIFFERENT mixer, which drops the automation position and queued events
    compile() exists to preserve.
    """
    before = mixer.strip_count
    mixer.add_strip("py-live-strip")
    assert mixer.strip_count == before + 1
    mixer.compile()

    # The metering keywords are the scene's own "metering" object, so a strip
    # added here and one declared in the document describe the same thing.
    mixer.add_strip("py-quiet-strip", enabled=False)
    scene = json.loads(mixer.to_json())
    ids = [strip["id"] for strip in scene["strips"]]
    assert "py-live-strip" in ids
    quiet = next(strip for strip in scene["strips"] if strip["id"] == "py-quiet-strip")
    assert quiet["metering"]["enabled"] is False

    # The C entry point reports failure by returning NULL, not an error code, so
    # a facade that forgot to check it would report success on both of these.
    with raises_coded():
        mixer.add_strip("py-live-strip")
    with raises_coded():
        mixer.add_strip("py-oversampled", true_peak_oversample=99)
    # Absence is the default (4x); an explicit 0 is refused rather than read as it.
    with pytest.raises(SonareValueError, match="true_peak_oversample"):
        mixer.add_strip("py-zero-oversample", true_peak_oversample=0)


def test_bus_add_remove_and_count(mixer) -> None:
    """add_bus / bus_count / remove_bus manage the mixer bus topology."""
    before = mixer.bus_count
    assert isinstance(before, int)
    mixer.add_bus("py-aux-bus", "aux")
    assert mixer.bus_count == before + 1
    mixer.remove_bus("py-aux-bus")
    assert mixer.bus_count == before


def test_add_bus_takes_a_role_token(mixer) -> None:
    """The bus role is a closed token set; the id is passed as ``id``."""
    before = mixer.bus_count
    mixer.add_bus(id="py-submix", role="submix")
    assert mixer.bus_count == before + 1
    with pytest.raises(SonareValueError, match="role"):
        mixer.add_bus(id="py-bad-role", role="Aux")  # type: ignore[arg-type]
    with pytest.raises(TypeError, match="role"):
        mixer.add_bus(id="py-int-role", role=1)  # type: ignore[arg-type]
    assert mixer.bus_count == before + 1
    mixer.remove_bus(id="py-submix")
    assert mixer.bus_count == before


def _empty_mixer():
    from libsonare import Mixer

    return Mixer.from_json(
        '{"version":2,"strips":[],"buses":[],"connections":[]}',
        sample_rate=48000,
        max_block_size=256,
    )


def _tone_block() -> list[float]:
    return [0.5 * math.sin(2 * math.pi * 440 * i / 48000) for i in range(256)]


def _connections(mixer) -> list[tuple[str, str]]:
    scene = json.loads(mixer.to_json())
    return [(c["source"], c["destination"]) for c in scene["connections"]]


def test_set_output_bus_routes_a_strip_through_a_submix_to_the_master() -> None:
    """The two-call recipe: strip to submix, then submix to the master."""
    mixer = _empty_mixer()
    try:
        mixer.add_strip("a")
        mixer.add_bus("sub", "submix")
        mixer.set_output_bus("a", "sub")
        mixer.compile()
        tone = _tone_block()
        for _ in range(4):
            out = mixer.process_stereo([tone], [tone])
        # An explicit bus is not default-routed, so the master stays silent.
        assert max(abs(v) for v in out.left) == 0.0
        assert mixer.bus_meter("sub").peak_db[0] > -60.0

        mixer.set_output_bus("sub", "master")
        mixer.compile()
        for _ in range(4):
            out = mixer.process_stereo([tone], [tone])
        assert max(abs(v) for v in out.left) > 0.01
        assert sorted(_connections(mixer)) == [("a", "sub"), ("sub", "master")]
    finally:
        mixer.close()


def test_set_output_bus_refusals_name_the_argument_and_leave_the_scene_unchanged() -> None:
    from libsonare import ErrorCode

    mixer = _empty_mixer()
    try:
        mixer.add_strip("a")
        mixer.add_bus("sub", "submix")
        mixer.add_bus("sub2", "submix")
        mixer.set_output_bus("sub", "sub2")
        before = mixer.to_json()
        cases = [
            ("ghost", "sub", "source_id must be the id of a strip or a bus in this mixer"),
            ("a", "ghost", "bus_id must be the id of a bus in this mixer"),
            ("a", "a", "bus_id must be the id of a bus, not of a strip"),
            ("master", "sub", "source_id must not be the id of the master bus"),
            ("sub", "sub", "bus_id must not be the same id as source_id"),
            ("sub2", "sub", "bus_id must not name a bus whose routing leads back to source_id"),
        ]
        for source, destination, text in cases:
            with pytest.raises(SonareValueError, match=re.escape(text) + "$") as info:
                mixer.set_output_bus(source, destination)
            assert info.value.code == int(ErrorCode.INVALID_PARAMETER)
            assert mixer.to_json() == before
        # No refused call compiled the graph: a bus never compiled still reports
        # an invalid parameter rather than a discard count.
        with pytest.raises(SonareValueError) as info:
            mixer.bus_non_finite_discard_count("sub")
        assert info.value.code == int(ErrorCode.INVALID_PARAMETER)
    finally:
        mixer.close()


def test_vca_group_add_remove_and_count(mixer) -> None:
    """add_vca_group / vca_group_count / remove_vca_group manage VCA groups."""
    before = mixer.vca_group_count
    assert isinstance(before, int)
    mixer.add_vca_group(id="py-vca", gain_db=-3.0, members=["vocal"])
    assert mixer.vca_group_count == before + 1
    mixer.set_vca_group_gain_db(id="py-vca", gain_db=-7.0)
    mixer.set_vca_group_members("py-vca", ["guest"])
    scene = json.loads(mixer.to_json())
    group = next(group for group in scene["vcaGroups"] if group["id"] == "py-vca")
    assert group["gainDb"] == -7.0
    assert group["members"] == ["guest"]
    mixer.remove_vca_group("py-vca")
    assert mixer.vca_group_count == before


def test_get_strip_count_matches_strip_count(mixer) -> None:
    """strip_count (now backed by sonare_mixer_get_strip_count) is consistent."""
    assert mixer.strip_count == 2


def test_automation_schedulers_take_curve_tokens(mixer) -> None:
    """Automation schedulers take the AutomationCurve tokens."""
    mixer.schedule_fader_automation("vocal", 0, -6.0)
    mixer.schedule_pan_automation("vocal", 128, 0.5, "linear")
    mixer.schedule_width_automation("vocal", 128, 1.2, "exponential")
    mixer.schedule_fader_automation("vocal", 256, -12.0, "hold")
    mixer.schedule_pan_automation("vocal", 384, -0.25, "sCurve")

    send_index = mixer.add_send(
        "vocal", "auto-send", "vocal-verb", send_db=-10.0, timing="preFader"
    )
    mixer.schedule_send_automation("vocal", send_index, 0, -3.0, "exponential")


def test_automation_curves_match_shared_name_corpus(mixer) -> None:
    """Each curve token maps to the shared ordinal; other spellings and numbers are refused."""
    for case in _AUTOMATION_CURVE_CORPUS["accepted"]:
        mixer.schedule_fader_automation("vocal", 0, -6.0, case["value"])
        assert _curve_value(case["value"], "curve") == case["ordinal"], case["value"]
    for value in _AUTOMATION_CURVE_CORPUS["rejected"]:
        with pytest.raises(SonareValueError):
            mixer.schedule_fader_automation("vocal", 0, -6.0, value)
    for value in _AUTOMATION_CURVE_CORPUS["rejected_numeric"]:
        with pytest.raises(TypeError):
            mixer.schedule_fader_automation("vocal", 0, -6.0, value)


def test_mixer_is_a_context_manager() -> None:
    """Leaving a with-block closes the native handle, on a normal exit and on a raise."""
    from libsonare import Mixer

    with Mixer.from_json(_first_preset_json(), sample_rate=48000, max_block_size=256) as mixer:
        assert mixer.strip_count > 0
    with raises_coded(ErrorCode.INVALID_STATE):
        _ = mixer.strip_count

    with (
        pytest.raises(KeyError),
        Mixer.from_json(_first_preset_json(), sample_rate=48000, max_block_size=256) as raised,
    ):
        raise KeyError("inside")
    with raises_coded(ErrorCode.INVALID_STATE):
        _ = raised.strip_count


def test_methods_after_close_raise(mixer) -> None:
    """Calling a method after close raises RuntimeError (guarded contract)."""
    mixer.close()
    with raises_coded(ErrorCode.INVALID_STATE):
        _ = mixer.strip_count
    with raises_coded(ErrorCode.INVALID_STATE):
        mixer.set_soloed("vocal", True)
    with raises_coded(ErrorCode.INVALID_STATE):
        mixer.settle("vocal")
    with raises_coded(ErrorCode.INVALID_STATE):
        mixer.strip_non_finite_discard_count("vocal")
    with raises_coded(ErrorCode.INVALID_STATE):
        mixer.bus_non_finite_discard_count("master")


def test_tail_samples_and_drain_tail_stereo(mixer) -> None:
    tail = mixer.tail_samples
    assert isinstance(tail, int)
    assert tail >= 0
    latency = mixer.latency_samples
    assert isinstance(latency, int)
    assert latency >= 0
    result = mixer.drain_tail_stereo(128)
    assert len(result.left) == 128
    assert len(result.right) == 128
    assert result.sample_rate == 48000


def test_drain_tail_stereo_enforces_block_count_range(mixer) -> None:
    # The accepted range is an integer in [1, block_size], shared with the Node
    # and WASM facades: 0 and out-of-range counts are rejected, and a fractional
    # float is refused rather than truncated toward zero.
    with pytest.raises(ValueError):
        mixer.drain_tail_stereo(0)
    with pytest.raises(ValueError):
        mixer.drain_tail_stereo(2.5)
    with pytest.raises(ValueError):
        mixer.drain_tail_stereo(1_000_000)


def test_tail_samples_reports_longest_serial_send_route() -> None:
    from libsonare import Mixer

    mixer = Mixer.from_json(_serial_tail_scene(), sample_rate=48000, max_block_size=128)
    try:
        assert mixer.tail_samples > 1440
    finally:
        mixer.close()


def _scene_with_insert_params(processor: str, params: dict[str, float]) -> str:
    """Build a one-strip scene whose post-fader insert carries `params`."""
    return json.dumps(
        {
            "version": 2,
            "buses": [{"id": "master", "role": "master"}],
            "strips": [
                {
                    "id": "vocal",
                    "inserts": [
                        {
                            "slot": "post",
                            "processor": processor,
                            "params": params,
                        }
                    ],
                }
            ],
            "connections": [{"source": "vocal", "destination": "master"}],
        }
    )


def test_mastering_insert_param_names_enumerates_keys() -> None:
    """insert_param_names lists the keys a processor reads (empty for unknown)."""
    from libsonare import mastering_insert_param_names

    comp = mastering_insert_param_names("dynamics.compressor")
    assert "thresholdDb" in comp
    assert "ratio" in comp
    # Band-indexed processors expose their band{i}.<field> keys.
    assert "band0.frequencyHz" in mastering_insert_param_names("eq.parametric")
    assert mastering_insert_param_names("not.a.real.processor") == []


def test_scene_warnings_surface_ignored_insert_params() -> None:
    """A scene insert with unread params loads but reports the ignored keys."""
    from libsonare import Mixer

    # eq.parametric reads only band{i}.* fields, so flat keys take no effect.
    scene = _scene_with_insert_params("eq.parametric", {"highPassHz": 80, "presenceDb": 4})
    mixer = Mixer.from_json(scene, sample_rate=48000, max_block_size=256)
    try:
        warnings = mixer.scene_warnings()
        assert len(warnings) == 1
        assert "eq.parametric" in warnings[0]
        assert "highPassHz" in warnings[0]
        assert "presenceDb" in warnings[0]
    finally:
        mixer.close()

    # A scene whose params are all consumed reports no warnings.
    clean = _scene_with_insert_params(
        "eq.parametric", {"band0.frequencyHz": 1000, "band0.gainDb": 3}
    )
    mixer = Mixer.from_json(clean, sample_rate=48000, max_block_size=256)
    try:
        assert mixer.scene_warnings() == []
    finally:
        mixer.close()


def test_scene_warnings_surface_a_send_to_an_undeclared_bus() -> None:
    """A send to a bus the scene does not declare loads and is reported, not refused."""
    import json

    from libsonare import Mixer

    def scene(destination: str) -> str:
        return json.dumps(
            {
                "version": 2,
                "buses": [{"id": "master", "role": "master"}, {"id": "aux1", "role": "aux"}],
                "strips": [
                    {
                        "id": "a",
                        "sends": [{"id": "s", "destinationBusId": destination, "sendDb": 0}],
                    }
                ],
                "connections": [{"source": "a", "destination": "master"}],
            }
        )

    mixer = Mixer.from_json(scene("aux1typo"), sample_rate=48000, max_block_size=256)
    try:
        assert mixer.scene_warnings() == [
            "send 'a' targets undeclared bus 'aux1typo'; created as implicit aux"
        ]
    finally:
        mixer.close()

    mixer = Mixer.from_json(scene("aux1"), sample_rate=48000, max_block_size=256)
    try:
        assert mixer.scene_warnings() == []
    finally:
        mixer.close()


def test_a_mixer_is_built_from_a_scene_mapping_or_empty_by_keyword() -> None:
    """The constructor takes the scene document mapping; JSON text goes through from_json."""
    from libsonare import Mixer, mixing_scene_preset, mixing_scene_preset_names

    scene = mixing_scene_preset(mixing_scene_preset_names()[0])
    with Mixer(sample_rate=48000, scene=scene, max_block_size=256) as from_mapping:
        text = from_mapping.to_json()
        assert from_mapping.strip_count == len(scene["strips"])
    with Mixer.from_json(text, sample_rate=48000, max_block_size=256) as from_text:
        assert json.loads(from_text.to_json()) == json.loads(text)
    with Mixer(sample_rate=48000) as empty:
        assert empty.strip_count == 0
    with pytest.raises(TypeError, match="json.loads"):
        Mixer(sample_rate=48000, scene=text)  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        Mixer(48000)  # type: ignore[misc]
    with pytest.raises(TypeError):
        Mixer.from_json(text, 48000)  # type: ignore[misc]
    with raises_coded(ErrorCode.INVALID_FORMAT, match=r"strips\[0\]\.faderDB"):
        Mixer(sample_rate=48000, scene={"strips": [{"id": "a", "faderDB": -3}]})


def test_from_json_refuses_an_unknown_scene_key_by_path() -> None:
    """A misspelled scene key is refused by path; ``$`` and ``x-`` keys are exempt."""
    import json

    from libsonare import Mixer

    def build(top: dict, strip: dict) -> str:
        return json.dumps(
            {
                "version": 2,
                **top,
                "buses": [{"id": "master", "role": "master"}],
                "strips": [{"id": "vocal", **strip}],
                "connections": [{"source": "vocal", "destination": "master"}],
            }
        )

    with pytest.raises(SonareError) as refused:
        Mixer.from_json(build({}, {"faderDB": -3}), sample_rate=48000, max_block_size=256)
    assert not isinstance(refused.value, SonareValueError)
    assert refused.value.code == int(ErrorCode.INVALID_FORMAT)
    assert "unknown key strips[0].faderDB" in str(refused.value)

    annotated = build({"$schema": "x", "x-note": 1}, {"x-color": "red"})
    mixer = Mixer.from_json(annotated, sample_rate=48000, max_block_size=256)
    try:
        assert mixer.scene_warnings() == []
    finally:
        mixer.close()


def test_mix_stereo_rejects_a_scene_with_no_audio() -> None:
    """An all-empty scene is a fault, not a mix of silence.

    A zero-frame block is a legitimate no-op for the C mixer, so without a
    facade guard `mix_stereo` answers an empty result plus meters reading the
    -120 dB floor — indistinguishable from a real mix. The positive control in
    the same test keeps this from passing because the call failed for some
    unrelated reason.
    """
    from libsonare import SonareValueError, mix_stereo

    with pytest.raises(SonareValueError, match="^mix_stereo: strips must not be empty$"):
        mix_stereo([], sample_rate=48000)

    with pytest.raises(SonareValueError, match="no audio to mix"):
        mix_stereo([([], [])], sample_rate=48000)
    with pytest.raises(SonareValueError, match="no audio to mix"):
        mix_stereo([([], []), ([], [])], sample_rate=48000)

    # Positive control: the ordinary two-strip mix still renders, and renders
    # something audible rather than the silence an empty result would show.
    left = [0.5, -0.5, 0.5, -0.5]
    right = [0.25, -0.25, 0.25, -0.25]
    result = mix_stereo([(left, right), (left, right)], sample_rate=48000)
    assert len(result.left) == len(left)
    assert len(result.right) == len(right)
    assert len(result.meters) == 2
    assert max(abs(v) for v in result.left) > 0.5
    assert max(abs(v) for v in result.right) > 0.25


def test_mix_stereo_applies_pan_and_pan_mode_independently() -> None:
    """A mode-only call reaches the strip instead of being dropped.

    `pan_mode` used to be read only when `pan` was supplied too, so a caller
    that changed nothing but the mode was mixed in the default one with nothing
    to show for it. The two are independent options and either alone applies.
    """
    from libsonare import mix_stereo

    # A left-only source tells the modes apart: Balance leaves it on the left,
    # stereoPan collapses the pair to a centred mono sum that reaches the right.
    frames = 512
    left = [1.0] * frames
    right = [0.0] * frames

    mode_only = mix_stereo([(left, right)], sample_rate=48000, pan_mode="stereoPan")
    assert mode_only.right[-1] > 0.1

    balance = mix_stereo([(left, right)], sample_rate=48000)
    assert balance.right[-1] == 0.0

    # The per-strip sequence form applies without a pan either.
    per_strip = mix_stereo(
        [(left, right), (left, right)],
        sample_rate=48000,
        pan_mode=["stereoPan", "stereoPan"],
    )
    assert per_strip.right[-1] > 0.1

    # A position-only call is unchanged by the mode now being read outside it.
    pan_only = mix_stereo([(left, right)], sample_rate=48000, pan=[0.3])
    pan_with_default_mode = mix_stereo(
        [(left, right)], sample_rate=48000, pan=[0.3], pan_mode="balance"
    )
    assert list(pan_only.left) == list(pan_with_default_mode.left)
    assert list(pan_only.right) == list(pan_with_default_mode.right)


def test_mix_stereo_rejects_non_finite_strip_samples() -> None:
    """NaN reads as silence on the peak meter, so it must never reach the mix."""
    from libsonare import SonareValueError, mix_stereo

    tone = [0.5, -0.5, 0.5, -0.5]

    with pytest.raises(SonareValueError, match=r"strips\[0\]\[0\] contains a non-finite sample"):
        mix_stereo([([math.nan] * 4, tone)], sample_rate=48000)
    with pytest.raises(SonareValueError, match=r"strips\[0\]\[1\] contains a non-finite sample"):
        mix_stereo([(tone, [math.inf] * 4)], sample_rate=48000)
    # The offending strip is named by index, not just the first one scanned.
    with pytest.raises(SonareValueError, match=r"strips\[1\]\[1\] contains a non-finite sample"):
        mix_stereo([(tone, tone), (tone, [0.5, math.nan, 0.5, -0.5])], sample_rate=48000)


def test_mix_stereo_true_peak_is_valid_on_a_one_shot_mix() -> None:
    """True peak is a max-hold, not an integrator, so it is valid immediately.

    Pins the `mix_stereo` docstring's meter caveat, which previously claimed
    true peak needed sustained streaming and read the floor on a one-shot mix.
    The loudness half of the caveat is real and is asserted alongside it.
    """
    from libsonare import metering_true_peak_dbtp, mix_stereo

    sr = 48000
    for frequency, n in ((1000, 64), (1000, 4096), (11000, 1024)):
        tone = [0.5 * math.sin(2 * math.pi * frequency * i / sr) for i in range(n)]
        meter = mix_stereo([(tone, tone)], sample_rate=sr).meters[0]

        # Not the floor sentinel, and above the -6.02 dBFS sample peak by the
        # genuine inter-sample overshoot rather than by chance.
        assert meter.true_peak_dbtp_l > -20.0
        assert meter.true_peak_dbtp_l > meter.peak_db_l
        # One block over the whole signal leaves no internal block edge, so the
        # strip meter agrees with the offline whole-signal measurement.
        assert meter.true_peak_dbtp_l == pytest.approx(
            metering_true_peak_dbtp(tone, sample_rate=sr), abs=1e-4
        )
        # The integrating loudness fields genuinely have not filled their
        # windows at these lengths.
        assert meter.momentary_lufs == pytest.approx(-120.0)

    # True peak floors only on digital silence.
    silence = [0.0] * 1024
    assert mix_stereo([(silence, silence)], sample_rate=sr).meters[0].true_peak_dbtp_l == (
        pytest.approx(-120.0)
    )


def test_one_shot_true_peak_covers_a_peak_in_the_last_samples() -> None:
    import libsonare

    left = [0.0] * 4092 + [0.9, -0.9, 0.9, -0.9]
    result = libsonare.mix_stereo([(left, list(left))], sample_rate=48000)
    expected = libsonare.metering_true_peak_dbtp(left, 48000)
    assert result.meters[0].true_peak_dbtp_l == pytest.approx(expected, abs=1e-4)
    assert result.meters[0].true_peak_dbtp_l > -1.0


def test_mixer_flush_meters_is_callable(mixer) -> None:
    mixer.flush_meters()


def test_process_stereo_input_count_rule(mixer) -> None:
    """Fewer inputs than strips leave the rest silent; more are refused naming both counts."""
    strips = mixer.strip_count
    block = [0.25] * 128
    mixer.process_stereo([], [])
    mixer.process_stereo([block], [block])
    mixer.process_stereo([block] * strips, [block] * strips)
    with pytest.raises(
        SonareValueError, match=f"left_inputs must hold at most {strips} inputs, one per strip$"
    ):
        mixer.process_stereo(
            left_inputs=[block] * (strips + 1), right_inputs=[block] * (strips + 1)
        )
    with pytest.raises(SonareValueError, match=r"left_inputs\[0\]"):
        mixer.process_stereo(left_inputs=[[[0.0]]], right_inputs=[[0.0]])


def test_mixing_scene_preset_returns_a_scene_document_mapping() -> None:
    from libsonare import Mixer, mixing_scene_preset, mixing_scene_preset_names

    for name in mixing_scene_preset_names():
        scene = mixing_scene_preset(name)
        assert isinstance(scene, dict)
        assert isinstance(scene["strips"], list)
        mixer = Mixer.from_json(json.dumps(scene), sample_rate=48000)
        try:
            assert mixer.strip_count == len(scene["strips"])
        finally:
            mixer.close()
    with pytest.raises(SonareValueError, match="must be one of"):
        mixing_scene_preset("noSuchScene")
    with pytest.raises(TypeError, match="^mixing_scene_preset: name must be one of "):
        mixing_scene_preset(0)  # type: ignore[arg-type]
