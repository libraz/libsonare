"""Effect results are numpy arrays: one-shot transforms, their Audio mirrors and records."""

from __future__ import annotations

import dataclasses

import numpy as np
import pytest

import libsonare

_SR = 22050


@pytest.fixture(scope="module")
def tone() -> np.ndarray:
    t = np.arange(int(0.25 * _SR), dtype=np.float32) / _SR
    return (0.3 * np.sin(2.0 * np.pi * 220.0 * t)).astype(np.float32)


def _assert_float32_vector(value: object) -> None:
    assert isinstance(value, np.ndarray)
    assert value.dtype == np.float32
    assert value.ndim == 1


def test_one_shot_transforms_return_float32_arrays(tone: np.ndarray) -> None:
    f0 = np.full(1 + len(tone) // 512, 220.0, dtype=np.float32)
    results = [
        libsonare.harmonic(tone, _SR),
        libsonare.percussive(tone, _SR),
        libsonare.time_stretch(tone, _SR, 1.25),
        libsonare.pitch_shift(tone, _SR, 2.0),
        libsonare.pitch_correct_to_midi(tone, _SR, 57.0, 58.0),
        libsonare.pitch_correct_timevarying(tone, _SR, f0),
        libsonare.note_stretch(tone, _SR, 0, len(tone) // 2, 1.5),
        libsonare.note_move(tone, _SR, 0, len(tone) // 2, 100),
        libsonare.voice_change(tone, _SR, 3.0),
        libsonare.normalize(tone, _SR, target_db=-3.0),
        libsonare.normalize(tone, _SR, mode="rms", target_db=-20.0),
        libsonare.trim(tone, _SR),
    ]
    for result in results:
        _assert_float32_vector(result)
        assert result.size > 0


def test_audio_methods_return_float32_arrays(tone: np.ndarray) -> None:
    audio = libsonare.Audio.from_buffer(tone, _SR)
    results = [
        audio.harmonic(),
        audio.percussive(),
        audio.time_stretch(1.25),
        audio.pitch_shift(2.0),
        audio.pitch_correct_to_midi(57.0, 58.0),
        audio.note_stretch(0, len(tone) // 2, 1.5),
        audio.note_move(0, len(tone) // 2, 100),
        audio.voice_change(3.0),
        audio.voice_change_realtime(),
        audio.normalize(target_db=-3.0),
        audio.trim(),
    ]
    for result in results:
        _assert_float32_vector(result)


def test_remix_intervals_are_n_by_2_arrays(tone: np.ndarray) -> None:
    intervals = np.array([[0, 1000], [2000, 3000]])
    resolved = libsonare.remix_aligned_intervals(tone, _SR, intervals, align_zeros=False)
    assert isinstance(resolved, np.ndarray)
    assert resolved.dtype == np.int32
    np.testing.assert_array_equal(resolved, intervals)
    np.testing.assert_allclose(
        libsonare.remix(tone, resolved, align_zeros=False),
        np.concatenate([tone[0:1000], tone[2000:3000]]),
    )


@pytest.mark.parametrize(
    "intervals",
    [[0, 1000, 2000, 3000], [[0, 1000, 2000]], [[[0, 1000]]]],
    ids=["flat", "three-columns", "three-dimensional"],
)
def test_remix_refuses_intervals_not_shaped_n_by_2(tone: np.ndarray, intervals: object) -> None:
    with pytest.raises(libsonare.SonareValueError, match=r"shape \(n, 2\)"):
        libsonare.remix_aligned_intervals(tone, _SR, intervals)
    with pytest.raises(libsonare.SonareValueError, match=r"shape \(n, 2\)"):
        libsonare.remix(tone, intervals)


def test_noise_band_bins_are_an_int32_array() -> None:
    bins = libsonare.mastering_repair_noise_band_bins(48000, n_fft=1024)
    assert isinstance(bins, np.ndarray)
    assert bins.dtype == np.int32
    assert bins.shape == (33,)
    assert int(bins[-1]) == 1024 // 2 + 1
    assert np.all(np.diff(bins) >= 0)


# Every record whose sample or per-band payload is a float32 array, with those
# fields. A record holding arrays compares by identity, so none defines __eq__.
_ARRAY_RECORDS = {
    "AutoTuneResult": ("samples",),
    "DeclickStereoResult": ("left", "right"),
    "DeclipStereoResult": ("left", "right"),
    "DecrackleStereoResult": ("left", "right"),
    "DehumStereoResult": ("left", "right"),
    "DenoiseStereoResult": ("left", "right"),
    "DereverbStereoResult": ("left", "right"),
    "DynamicRangeReport": ("window_rms_db",),
    "HpssResult": ("harmonic", "percussive"),
    "HumDetection": ("harmonic_dbfs",),
    "MasteringChainResult": ("samples",),
    "MasteringChainStereoResult": ("left", "right"),
    "MasteringReport": ("band_energy_delta_db",),
    "MasteringResult": ("samples",),
    "MasteringStereoResult": ("left", "right"),
    "MeterTelemetryRecordWide": ("peak_db", "rms_db", "true_peak_dbtp", "input_peak_db"),
    "MixResult": ("left", "right"),
    "MixerStereoResult": ("left", "right"),
    "NoiseDetection": ("band_floor_dbfs",),
    "NormalizeStereoResult": ("left", "right"),
    "RirResult": ("rir",),
    "RoomEstimate": ("band_absorption", "rt60_bands"),
    "RoomMorphResult": ("audio",),
    "RoomMorphStereoResult": ("left", "right"),
    "ScopeTelemetryRecord": ("bands",),
    "TrimSilenceStereoResult": ("left", "right"),
}


@pytest.mark.parametrize("name", sorted(_ARRAY_RECORDS))
def test_array_records_declare_ndarray_fields_and_compare_by_identity(name: str) -> None:
    cls = getattr(libsonare, name)
    assert dataclasses.is_dataclass(cls)
    assert cls.__dataclass_params__.frozen
    declared = {field.name: field.type for field in dataclasses.fields(cls)}
    for field_name in _ARRAY_RECORDS[name]:
        assert declared[field_name] == "np.ndarray", (name, field_name)
    assert cls.__eq__ is object.__eq__


def test_records_built_by_the_library_hold_float32_arrays(tone: np.ndarray) -> None:
    hpss = libsonare.hpss(tone, _SR)
    _assert_float32_vector(hpss.harmonic)
    _assert_float32_vector(hpss.percussive)

    stereo = libsonare.normalize_stereo(tone, tone, _SR, target_db=-3.0)
    _assert_float32_vector(stereo.left)
    _assert_float32_vector(stereo.right)
