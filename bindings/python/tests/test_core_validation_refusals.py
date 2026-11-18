"""Refusals owned by the core function, pinned by the cross-surface corpus."""

from __future__ import annotations

import ctypes
import json
import re
from pathlib import Path

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError, _conversions
from libsonare._runtime import (
    _check,
    _from_c_int_array,
    _get_lib,
    _out_int_array,
    _to_c_float_array,
)

_CORPUS = json.loads(
    (
        Path(__file__).resolve().parents[3] / "tests/conformance/core_validation_refusals.json"
    ).read_text()
)

_ERROR_CLASS = {"argument": SonareValueError}

_MFCC = _CORPUS["mfcc_to_mel"]
_MEL = _CORPUS["mel_to_audio"]
_MFCC_AUDIO = _CORPUS["mfcc_to_audio"]
_STRETCH = _CORPUS["native_spectral_stretch"]
_CHAIN = _CORPUS["streaming_mastering_chain_prepare"]
_EQ = _CORPUS["equalizer_prepare"]
_RETUNE = _CORPUS["streaming_retune_prepare"]
_RESAMPLE = _CORPUS["resample"]
_ZERO_CROSSINGS = _CORPUS["zero_crossings"]


def _mfcc() -> np.ndarray:
    shape = (_MFCC["input"]["n_mfcc"], _MFCC["input"]["n_frames"])
    return np.full(shape, _MFCC["input"]["fill"], dtype=np.float32)


@pytest.mark.parametrize("case", _MFCC["accepted"], ids=lambda case: case["id"])
def test_mfcc_to_mel_accepts(case: dict) -> None:
    mel = libsonare.mfcc_to_mel(_mfcc(), n_mels=case["n_mels"])
    assert mel.shape[0] == case["n_mels"]


@pytest.mark.parametrize("case", _MFCC["refused"], ids=lambda case: case["id"])
def test_mfcc_to_mel_refuses(case: dict) -> None:
    error_class = _ERROR_CLASS[case["cause"]]
    with pytest.raises(error_class, match=case["message"]):
        libsonare.mfcc_to_mel(_mfcc(), n_mels=case["n_mels"])
    with pytest.raises(error_class, match=case["message"]):
        libsonare.mfcc_to_audio(_mfcc(), n_mels=case["n_mels"], n_iter=1)


def _mel() -> np.ndarray:
    shape = (_MEL["input"]["n_mels"], _MEL["input"]["n_frames"])
    return np.full(shape, _MEL["input"]["fill"], dtype=np.float32)


@pytest.mark.parametrize("case", _MEL["accepted"], ids=lambda case: case["id"])
def test_mel_to_audio_accepts(case: dict) -> None:
    assert libsonare.mel_to_audio(_mel(), n_iter=case["n_iter"]).size > 0


@pytest.mark.parametrize("case", _MEL["refused"], ids=lambda case: case["id"])
def test_mel_to_audio_refuses(case: dict) -> None:
    with pytest.raises(_ERROR_CLASS[case["cause"]], match=case["message"]):
        libsonare.mel_to_audio(_mel(), n_iter=case["n_iter"])


def _mfcc_audio() -> np.ndarray:
    shape = (_MFCC_AUDIO["input"]["n_mfcc"], _MFCC_AUDIO["input"]["n_frames"])
    return np.full(shape, _MFCC_AUDIO["input"]["fill"], dtype=np.float32)


@pytest.mark.parametrize("case", _MFCC_AUDIO["accepted"], ids=lambda case: case["id"])
def test_mfcc_to_audio_accepts(case: dict) -> None:
    n_mels = _MFCC_AUDIO["input"]["n_mels"]
    assert libsonare.mfcc_to_audio(_mfcc_audio(), n_mels=n_mels, n_iter=case["n_iter"]).size > 0


@pytest.mark.parametrize("case", _MFCC_AUDIO["refused"], ids=lambda case: case["id"])
def test_mfcc_to_audio_refuses(case: dict) -> None:
    n_mels = _MFCC_AUDIO["input"]["n_mels"]
    with pytest.raises(_ERROR_CLASS[case["cause"]], match=case["message"]):
        libsonare.mfcc_to_audio(_mfcc_audio(), n_mels=n_mels, n_iter=case["n_iter"])


def _stretch_samples() -> np.ndarray:
    count = _STRETCH["input"]["length"]
    return np.full(count, _STRETCH["input"]["fill"], dtype=np.float32)


@pytest.mark.parametrize("case", _STRETCH["accepted"], ids=lambda case: case["id"])
def test_native_spectral_stretch_accepts(case: dict) -> None:
    args = (_stretch_samples(), _STRETCH["input"]["sample_rate"])
    geometry = {"n_fft": case["n_fft"], "hop_length": case["hop_length"]}
    assert libsonare.time_stretch(*args, _STRETCH["input"]["speed_ratio"], **geometry).size > 0
    assert libsonare.pitch_shift(*args, _STRETCH["input"]["pitch_semitones"], **geometry).size > 0


@pytest.mark.parametrize("case", _STRETCH["refused"], ids=lambda case: case["id"])
def test_native_spectral_stretch_refuses(case: dict) -> None:
    # The facade validates the geometry pair itself, so only the error class is pinned here.
    error_class = _ERROR_CLASS[case["cause"]]
    args = (_stretch_samples(), _STRETCH["input"]["sample_rate"])
    geometry = {"n_fft": case["n_fft"], "hop_length": case["hop_length"]}
    with pytest.raises(error_class):
        libsonare.time_stretch(*args, _STRETCH["input"]["speed_ratio"], **geometry)
    with pytest.raises(error_class):
        libsonare.pitch_shift(*args, _STRETCH["input"]["pitch_semitones"], **geometry)


@pytest.mark.parametrize("case", _CHAIN["accepted"], ids=lambda case: case["id"])
def test_streaming_mastering_chain_prepare_accepts(case: dict) -> None:
    with libsonare.StreamingMasteringChain(
        sample_rate=case["sample_rate"],
        num_channels=_CHAIN["input"]["num_channels"],
        max_block_size=case["max_block_size"],
    ):
        pass


@pytest.mark.parametrize("case", _CHAIN["refused"], ids=lambda case: case["id"])
def test_streaming_mastering_chain_prepare_refuses(case: dict) -> None:
    with pytest.raises(_ERROR_CLASS[case["cause"]], match=re.escape(case["message"])):
        libsonare.StreamingMasteringChain(
            sample_rate=case["sample_rate"],
            num_channels=_CHAIN["input"]["num_channels"],
            max_block_size=case["max_block_size"],
        )


@pytest.mark.parametrize("case", _EQ["accepted"], ids=lambda case: case["id"])
def test_equalizer_prepare_accepts(case: dict) -> None:
    with libsonare.StreamingEqualizer(
        sample_rate=case["sample_rate"], max_block_size=case["max_block_size"]
    ):
        pass


@pytest.mark.parametrize("case", _EQ["refused"], ids=lambda case: case["id"])
def test_equalizer_prepare_refuses(case: dict) -> None:
    with pytest.raises(_ERROR_CLASS[case["cause"]], match=re.escape(case["message"])):
        libsonare.StreamingEqualizer(
            sample_rate=case["sample_rate"], max_block_size=case["max_block_size"]
        )


@pytest.mark.parametrize("case", _RETUNE["accepted"], ids=lambda case: case["id"])
def test_streaming_retune_prepare_accepts(case: dict) -> None:
    with libsonare.StreamingRetune() as retune:
        retune.prepare(case["sample_rate"], case["max_block_size"])


@pytest.mark.parametrize("case", _RETUNE["refused"], ids=lambda case: case["id"])
def test_streaming_retune_prepare_refuses(case: dict) -> None:
    error_class = _ERROR_CLASS[case["cause"]]
    with (
        libsonare.StreamingRetune() as retune,
        pytest.raises(error_class, match=re.escape(case["message"])),
    ):
        retune.prepare(case["sample_rate"], case["max_block_size"])


def _resample_input(case: dict) -> np.ndarray:
    samples = np.full(_RESAMPLE["input"]["length"], _RESAMPLE["input"]["fill"], dtype=np.float32)
    if "nan_at" in case:
        samples[case["nan_at"]] = np.nan
    return samples


@pytest.mark.parametrize("case", _RESAMPLE["accepted"], ids=lambda case: case["id"])
def test_resample_accepts(case: dict) -> None:
    out = libsonare.resample(_resample_input(case), case["src_sr"], case["target_sr"])
    assert out.shape[0] > 0


@pytest.mark.parametrize("case", _RESAMPLE["refused"], ids=lambda case: case["id"])
def test_resample_refuses(case: dict) -> None:
    error_class = _ERROR_CLASS[case["cause"]]
    samples = _resample_input(case)
    with pytest.raises(error_class):
        libsonare.resample(samples, case["src_sr"], case["target_sr"])
    # The facade's buffer guard names a non-finite sample first, so the core's message is
    # read through the ctypes wrapper that reaches the C entry directly.
    with pytest.raises(error_class, match=re.escape(case["message"])):
        _conversions._resample_float32(samples, case["src_sr"], case["target_sr"])


def _zero_crossings_input(case: dict) -> np.ndarray:
    samples = np.full(
        _ZERO_CROSSINGS["input"]["length"], _ZERO_CROSSINGS["input"]["fill"], dtype=np.float32
    )
    if "nan_at" in case:
        samples[case["nan_at"]] = np.nan
    return samples


def _zero_crossings_core(samples: np.ndarray) -> np.ndarray:
    lib = _get_lib()
    c_array, length = _to_c_float_array(samples)
    with _out_int_array(lib) as (out, out_count):
        rc = lib.sonare_zero_crossings(
            c_array,
            ctypes.c_size_t(length),
            ctypes.c_float(1e-10),
            ctypes.c_int(0),
            ctypes.c_int(1),
            ctypes.c_int(1),
            ctypes.byref(out),
            ctypes.byref(out_count),
        )
        _check(rc)
        return _from_c_int_array(out, out_count.value)


@pytest.mark.parametrize("case", _ZERO_CROSSINGS["accepted"], ids=lambda case: case["id"])
def test_zero_crossings_accepts(case: dict) -> None:
    samples = _zero_crossings_input(case)
    assert libsonare.zero_crossings(samples).shape[0] > 0
    assert _zero_crossings_core(samples).shape[0] > 0


@pytest.mark.parametrize("case", _ZERO_CROSSINGS["refused"], ids=lambda case: case["id"])
def test_zero_crossings_refuses(case: dict) -> None:
    error_class = _ERROR_CLASS[case["cause"]]
    samples = _zero_crossings_input(case)
    with pytest.raises(error_class):
        libsonare.zero_crossings(samples)
    # The facade's buffer guard names a non-finite sample first, so the core's message is
    # read through a direct call to the C entry.
    with pytest.raises(error_class, match=re.escape(case["message"])):
        _zero_crossings_core(samples)
