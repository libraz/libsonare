"""The mixer's master-output meter, read through ``configure_meter`` / ``meter_snapshot``.

The readings are held to ``tests/conformance/master_meter_reading.json``, the
record every surface reproduces on the same input.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

import numpy as np
import pytest

import libsonare
from libsonare import ErrorCode, SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

_CONFORMANCE = (
    Path(__file__).resolve().parents[3] / "tests" / "conformance" / "master_meter_reading.json"
)
_DOCUMENT = json.loads(_CONFORMANCE.read_text(encoding="utf-8"))

# JSON key -> MixMeterSnapshot field.
_FIELDS = {
    "peakDbL": ("peak_db", 0),
    "peakDbR": ("peak_db", 1),
    "rmsDbL": ("rms_db", 0),
    "rmsDbR": ("rms_db", 1),
    "correlation": "correlation",
    "truePeakDbL": ("true_peak_dbtp", 0),
    "truePeakDbR": ("true_peak_dbtp", 1),
}

_SCENE = json.dumps(
    {
        "version": 2,
        "strips": [{"id": "a"}],
        "buses": [{"id": "master", "role": "master"}],
        "connections": [],
    }
)


def _assert_reading(snapshot: libsonare.MixMeterSnapshot, expected: dict[str, float]) -> None:
    for key, field in _FIELDS.items():
        tolerance = _DOCUMENT["toleranceCorrelation" if key == "correlation" else "toleranceDb"]
        actual = (
            getattr(snapshot, field[0])[field[1]]
            if isinstance(field, tuple)
            else getattr(snapshot, field)
        )
        assert actual == pytest.approx(expected[key], abs=tolerance), key


@pytest.mark.parametrize("case", _DOCUMENT["cases"], ids=lambda case: case["name"])
def test_the_master_meter_reproduces_the_conformance_reading(case: dict) -> None:
    rate, block, blocks = case["sampleRate"], case["blockSize"], case["blocks"]
    n = np.arange(block * blocks, dtype=np.float64)
    phase = 2.0 * math.pi * case["frequencyHz"] * n / rate + case["phaseRad"]
    tone = case["amplitude"] * np.sin(phase)
    left = tone.astype(np.float32)
    right = (tone * case["rightGain"]).astype(np.float32)

    mixer = libsonare.Mixer.from_json(case["scene"], sample_rate=rate, max_block_size=block)
    try:
        mixer.configure_meter(true_peak_oversample=case["truePeakOversample"], measure_lufs=False)
        for start in range(0, block * blocks, block):
            mixer.process_stereo([left[start : start + block]], [right[start : start + block]])
        snapshot = mixer.meter_snapshot()
        assert snapshot.num_channels == 2
        assert len(snapshot.peak_db) == len(snapshot.rms_db) == len(snapshot.true_peak_dbtp) == 2
        _assert_reading(snapshot, case["expected"])
        mixer.flush_meters()
        _assert_reading(mixer.meter_snapshot(), case["expectedAfterFlush"])
    finally:
        mixer.close()


@pytest.fixture
def mixer():
    mixer = libsonare.Mixer.from_json(_SCENE, sample_rate=48000, max_block_size=256)
    try:
        yield mixer
    finally:
        mixer.close()


def test_reading_before_the_meter_is_enabled_is_an_invalid_state(mixer) -> None:
    with pytest.raises(SonareError) as caught:
        mixer.meter_snapshot()
    assert caught.value.code == int(ErrorCode.INVALID_STATE)


@pytest.mark.parametrize("factor", [0, 2, 16])
def test_an_oversample_factor_other_than_4_or_8_is_refused(mixer, factor: int) -> None:
    with pytest.raises(SonareValueError):
        mixer.configure_meter(true_peak_oversample=factor)


@pytest.mark.parametrize("name", ["enabled", "measure_lufs", "measure_true_peak"])
def test_a_flag_must_be_a_bool(mixer, name: str) -> None:
    with pytest.raises(TypeError, match=name):
        mixer.configure_meter(**{name: 1})


def test_true_peak_off_leaves_the_true_peak_fields_unmeasured(mixer) -> None:
    block = (0.5 * np.sin(2 * math.pi * 997.0 * np.arange(256) / 48000)).astype(np.float32)
    mixer.configure_meter(measure_true_peak=False, measure_lufs=False)
    mixer.process_stereo([block], [block])
    off = mixer.meter_snapshot()
    mixer.configure_meter(measure_lufs=False)
    mixer.process_stereo([block], [block])
    on = mixer.meter_snapshot()
    assert off.true_peak_dbtp == (None, None)
    assert off.max_true_peak_dbtp is None
    assert on.true_peak_dbtp[0] is not None
    assert math.isfinite(on.true_peak_dbtp[0])
    assert on.peak_db[0] == pytest.approx(off.peak_db[0], abs=1e-4)
    for field in (
        "peak_db_l",
        "peak_db_r",
        "rms_db_l",
        "rms_db_r",
        "true_peak_dbtp_l",
        "true_peak_dbtp_r",
    ):
        assert not hasattr(on, field)
