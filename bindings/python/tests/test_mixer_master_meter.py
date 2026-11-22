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
    "peakDbL": "peak_db_l",
    "peakDbR": "peak_db_r",
    "rmsDbL": "rms_db_l",
    "rmsDbR": "rms_db_r",
    "correlation": "correlation",
    "truePeakDbL": "true_peak_dbtp_l",
    "truePeakDbR": "true_peak_dbtp_r",
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
        assert getattr(snapshot, field) == pytest.approx(expected[key], abs=tolerance), key


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
        _assert_reading(mixer.meter_snapshot(), case["expected"])
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
    assert (off.true_peak_dbtp_l, off.true_peak_dbtp_r, off.max_true_peak_dbtp) == (
        None,
        None,
        None,
    )
    assert on.true_peak_dbtp_l is not None
    assert math.isfinite(on.true_peak_dbtp_l)
    assert on.peak_db_l == pytest.approx(off.peak_db_l, abs=1e-4)
