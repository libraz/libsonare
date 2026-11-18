"""Refusals owned by the core function, pinned by the cross-surface corpus."""

from __future__ import annotations

import json
import re
from pathlib import Path

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

_CORPUS = json.loads(
    (
        Path(__file__).resolve().parents[3] / "tests/conformance/core_validation_refusals.json"
    ).read_text()
)

_ERROR_CLASS = {"argument": SonareValueError}

_MFCC = _CORPUS["mfcc_to_mel"]
_MEL = _CORPUS["mel_to_audio"]
_MFCC_AUDIO = _CORPUS["mfcc_to_audio"]
_RETUNE = _CORPUS["streaming_retune_prepare"]


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
