"""Tests for newly exposed snake_case Mixer methods in the Python binding."""

from __future__ import annotations

import json
import math
import os
import sys
from pathlib import Path

import pytest


def _lib_available() -> bool:
    """Check if libsonare shared library is available."""
    env_path = os.environ.get("SONARE_LIB_PATH")
    if env_path and Path(env_path).exists():
        return True

    project_root = Path(__file__).parent.parent.parent.parent
    lib_name = "libsonare.dylib" if sys.platform == "darwin" else "libsonare.so"
    return any(
        (project_root / build_dir / "lib" / lib_name).exists()
        for build_dir in ("build-mastering-api", "build", "build-mastering")
    )


pytestmark = pytest.mark.skipif(not _lib_available(), reason="libsonare shared library not found")


def _first_preset_json() -> str:
    """Return the JSON scene of the first built-in mixing preset."""
    from libsonare._mixing import mixing_scene_preset_json, mixing_scene_preset_names

    names = mixing_scene_preset_names()
    assert names, "expected at least one built-in mixing preset"
    return mixing_scene_preset_json(names[0])


@pytest.fixture()
def mixer():
    """Build a Mixer from the first preset scene and close it afterwards."""
    from libsonare import Mixer

    mixer = Mixer.from_scene_json(_first_preset_json(), sample_rate=48000, block_size=256)
    try:
        yield mixer
    finally:
        mixer.close()


def _process_one_block(mixer) -> None:
    """Feed one block of audio through every strip so meters/goniometer fill."""
    n = mixer.strip_count()
    block = [[0.2 * math.sin(2 * math.pi * 440 * i / 48000) for i in range(256)] for _ in range(n)]
    mixer.process_stereo(block, block)


def test_preset_scene_builds_mixer(mixer) -> None:
    """A mixer built from the first preset exposes the expected strips."""
    assert mixer.strip_count() == 2
    scene = json.loads(mixer.to_scene_json())
    assert len(scene["strips"]) == 2


def test_strip_by_id_returns_index_and_raises_on_unknown(mixer) -> None:
    """strip_by_id resolves known ids to an int index and raises KeyError otherwise."""
    index = mixer.strip_by_id("vocal")
    assert isinstance(index, int)
    assert 0 <= index < mixer.strip_count()
    with pytest.raises(KeyError):
        mixer.strip_by_id("does-not-exist")


def test_strip_meter_and_meter_tap_return_snapshots(mixer) -> None:
    """strip_meter / meter_tap return a MixMeterSnapshot with numeric fields."""
    from libsonare import MeterTap
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
        snapshot.true_peak_db_l,
        snapshot.max_true_peak_db,
    ):
        assert isinstance(value, float)
        assert math.isfinite(value)
    assert isinstance(snapshot.likely_mono_compatible, bool)
    assert isinstance(snapshot.seq, int)

    # meter_tap is an alias and accepts the enum, an int, and a name.
    assert isinstance(mixer.meter_tap(0, MeterTap.PRE_FADER), MixMeterSnapshot)
    assert isinstance(mixer.strip_meter(0, 0), MixMeterSnapshot)
    assert isinstance(mixer.strip_meter(0, "pre"), MixMeterSnapshot)


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


def test_set_pan_law_accepts_enum_int_and_name(mixer) -> None:
    """set_pan_law accepts a PanLaw enum, a raw int, and a name; invalid raises."""
    from libsonare import PanLaw

    mixer.set_pan_law(0, PanLaw.CONST_6DB)
    mixer.set_pan_law(0, 2)
    mixer.set_pan_law(0, "linear")
    with pytest.raises(ValueError):
        mixer.set_pan_law(0, "not-a-pan-law")


def test_simple_strip_setters_do_not_raise(mixer) -> None:
    """The simple per-strip setters accept valid values without raising."""
    mixer.set_polarity_invert("vocal", True, False)
    mixer.set_channel_delay_samples("vocal", 8)
    mixer.set_vca_offset_db("vocal", -2.0)
    mixer.set_dual_pan("vocal", -0.3, 0.4)


def test_soloed_and_solo_safe_reflected_in_scene(mixer) -> None:
    """set_soloed / set_solo_safe are reflected in the re-serialized scene."""
    scene_before = json.loads(mixer.to_scene_json())
    assert all(not strip["soloed"] for strip in scene_before["strips"])

    mixer.set_soloed("vocal", True)
    mixer.set_solo_safe("vocal-verb-return", True)

    scene_after = json.loads(mixer.to_scene_json())
    by_id = {strip["id"]: strip for strip in scene_after["strips"]}
    assert by_id["vocal"]["soloed"] is True
    assert by_id["vocal-verb-return"]["soloSafe"] is True


def test_sends_add_and_set_level(mixer) -> None:
    """add_send returns an int index that set_send_db then accepts."""
    index = mixer.add_send("vocal", "extra-send", "vocal-verb", send_db=-12.0, timing=0)
    assert isinstance(index, int)
    assert index >= 0
    mixer.set_send_db("vocal", index, -6.0)


def test_add_send_accepts_send_timing_enum_name_and_int(mixer) -> None:
    """add_send accepts a SendTiming enum, a name, and a raw int; invalid raises."""
    from libsonare import SendTiming

    i0 = mixer.add_send("vocal", "send-enum", "vocal-verb", timing=SendTiming.POST_FADER)
    i1 = mixer.add_send("vocal", "send-name", "vocal-verb", timing="pre_fader")
    i2 = mixer.add_send("vocal", "send-int", "vocal-verb", timing=1)
    assert all(isinstance(i, int) and i >= 0 for i in (i0, i1, i2))
    with pytest.raises(ValueError):
        mixer.add_send("vocal", "send-bad", "vocal-verb", timing="sideways")


def test_bus_add_remove_and_count(mixer) -> None:
    """add_bus / bus_count / remove_bus manage the mixer bus topology."""
    before = mixer.bus_count()
    assert isinstance(before, int)
    mixer.add_bus("py-aux-bus", "aux")
    assert mixer.bus_count() == before + 1
    mixer.remove_bus("py-aux-bus")
    assert mixer.bus_count() == before


def test_vca_group_add_remove_and_count(mixer) -> None:
    """add_vca_group / vca_group_count / remove_vca_group manage VCA groups."""
    before = mixer.vca_group_count()
    assert isinstance(before, int)
    mixer.add_vca_group("py-vca", gain_db=-3.0, members=["vocal"])
    assert mixer.vca_group_count() == before + 1
    mixer.remove_vca_group("py-vca")
    assert mixer.vca_group_count() == before


def test_get_strip_count_matches_strip_count(mixer) -> None:
    """strip_count (now backed by sonare_mixer_get_strip_count) is consistent."""
    assert mixer.strip_count() == 2


def test_automation_schedulers_accept_curve_enum(mixer) -> None:
    """Automation schedulers accept the AutomationCurve enum, names, and ints."""
    from libsonare import AutomationCurve

    mixer.schedule_fader_automation("vocal", 0, -6.0, AutomationCurve.LINEAR)
    mixer.schedule_pan_automation("vocal", 128, 0.5, "linear")
    mixer.schedule_width_automation("vocal", 128, 1.2, AutomationCurve.EXPONENTIAL)

    send_index = mixer.add_send("vocal", "auto-send", "vocal-verb", send_db=-10.0, timing=1)
    mixer.schedule_send_automation("vocal", send_index, 0, -3.0, 1)


def test_automation_rejects_invalid_curve(mixer) -> None:
    """An unknown automation curve name raises ValueError."""
    with pytest.raises(ValueError):
        mixer.schedule_fader_automation("vocal", 0, -6.0, "wobble")


def test_methods_after_close_raise(mixer) -> None:
    """Calling a method after close raises RuntimeError (guarded contract)."""
    mixer.close()
    with pytest.raises(RuntimeError):
        mixer.strip_count()
    with pytest.raises(RuntimeError):
        mixer.set_soloed("vocal", True)
