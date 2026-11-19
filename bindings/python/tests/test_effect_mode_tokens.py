"""The repair and dynamics mode arguments are exact lowerCamel tokens.

Each takes only its tokens: another spelling, including a retired alias, is a
``SonareValueError`` naming the accepted ones, and a number (an old ordinal,
or a bool) is a ``TypeError``.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any, get_args

import numpy as np
import pytest

import libsonare
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 16000
_t = np.arange(SR // 4, dtype=np.float32) / SR
TONE = (0.3 * np.sin(2 * np.pi * 220.0 * _t)).astype(np.float32)

# (Literal name, tokens in C ordinal order, the C constant prefix)
SETS = [
    ("TrimSilenceMode", ("peak", "lufsGated"), "SONARE_TRIM_SILENCE_MODE_"),
    ("DecrackleMode", ("median", "waveletShrinkage"), "SONARE_DECRACKLE_MODE_"),
    (
        "DenoiseClassicalMode",
        ("logMmse", "mmseStsa", "spectralSubtraction"),
        "SONARE_DENOISE_MODE_",
    ),
    (
        "DenoiseClassicalNoiseEstimator",
        ("quantile", "mcra", "imcra", "spp"),
        "SONARE_DENOISE_NOISE_ESTIMATOR_",
    ),
    ("DehumMode", ("subtract", "notch"), "SONARE_DEHUM_MODE_"),
    ("CompressorDetector", ("peak", "rms", "logRms"), "SONARE_COMPRESSOR_DETECTOR_"),
]


def _c_constant(prefix: str, token: str) -> int:
    from libsonare import _ffi

    snake = "".join(f"_{c}" if c.isupper() else c for c in token).upper()
    value = getattr(_ffi, prefix + snake)
    assert isinstance(value, int)
    return value


@pytest.mark.parametrize(("name", "tokens", "prefix"), SETS)
def test_each_set_is_a_literal_in_c_ordinal_order(
    name: str, tokens: tuple[str, ...], prefix: str
) -> None:
    assert get_args(getattr(libsonare, name)) == tokens
    assert [_c_constant(prefix, token) for token in tokens] == list(range(len(tokens)))


def _mono(fn: Callable[..., Any]) -> Callable[..., Any]:
    return lambda **kw: fn(TONE, SR, **kw)


def _stereo(fn: Callable[..., Any]) -> Callable[..., Any]:
    return lambda **kw: fn(TONE, TONE, SR, **kw)


# (call, keyword, every token of its set, a retired spelling)
TRIM = ("peak", "lufsGated")
CRACKLE = ("median", "waveletShrinkage")
DENOISE = ("logMmse", "mmseStsa", "spectralSubtraction")
ESTIMATOR = ("quantile", "mcra", "imcra", "spp")
HUM = ("subtract", "notch")
SITES = [
    ("mastering_repair_trim_silence", _mono, "mode", TRIM, "lufs_gated"),
    ("mastering_repair_trim_silence_stereo", _stereo, "mode", TRIM, "lufs"),
    ("mastering_repair_detect_trim_range", _mono, "mode", TRIM, "lufs_gated"),
    ("mastering_repair_detect_trim_range_stereo", _stereo, "mode", TRIM, "lufs_gated"),
    ("mastering_repair_decrackle", _mono, "mode", CRACKLE, "wavelet"),
    ("mastering_repair_decrackle_stereo", _stereo, "mode", CRACKLE, "wavelet_shrinkage"),
    ("mastering_repair_detect_crackle", _mono, "mode", CRACKLE, "wavelet"),
    ("mastering_repair_denoise_classical", _mono, "mode", DENOISE, "lsa"),
    ("mastering_repair_denoise_classical_stereo", _stereo, "mode", DENOISE, "log_mmse"),
    ("mastering_repair_detect_noise_floor", _mono, "mode", DENOISE, "mmse_stsa"),
    ("mastering_repair_denoise_classical", _mono, "noise_estimator", ESTIMATOR, "MCRA"),
    ("mastering_repair_denoise_classical_stereo", _stereo, "noise_estimator", ESTIMATOR, " spp"),
    ("mastering_repair_detect_noise_floor", _mono, "noise_estimator", ESTIMATOR, "Quantile"),
    ("mastering_repair_dehum", _mono, "mode", HUM, "Notch"),
    ("mastering_repair_dehum_stereo", _stereo, "mode", HUM, "notch "),
    ("mastering_repair_detect_hum", _mono, "mode", HUM, "Subtract"),
    ("mastering_dynamics_compressor", _mono, "detector", ("peak", "rms", "logRms"), "log_rms"),
]


def _linked(**kw: Any) -> Any:
    return libsonare.mastering_repair_denoise_classical_linked([TONE, TONE], SR, **kw)


LINKED = [
    ("mode", DENOISE, "spectral_subtraction"),
    ("noise_estimator", ESTIMATOR, "imcra "),
]


def _calls() -> list[tuple[str, Callable[..., Any], str, tuple[str, ...], str]]:
    rows = [
        (name, shape(getattr(libsonare, name)), kw, toks, old)
        for name, shape, kw, toks, old in SITES
    ]
    rows += [
        ("mastering_repair_denoise_classical_linked", _linked, kw, toks, old)
        for kw, toks, old in LINKED
    ]
    return rows


CALLS = _calls()
IDS = [f"{name}({kw})" for name, _, kw, _, _ in CALLS]


@pytest.mark.parametrize(("name", "call", "keyword", "tokens", "retired"), CALLS, ids=IDS)
def test_every_token_runs(
    name: str, call: Callable[..., Any], keyword: str, tokens: tuple[str, ...], retired: str
) -> None:
    for token in tokens:
        call(**{keyword: token})


@pytest.mark.parametrize(("name", "call", "keyword", "tokens", "retired"), CALLS, ids=IDS)
def test_an_ordinal_is_a_type_error(
    name: str, call: Callable[..., Any], keyword: str, tokens: tuple[str, ...], retired: str
) -> None:
    for number in (0, 1, True):
        with pytest.raises(
            TypeError, match=f"^{name}: {keyword} must be one of {', '.join(tokens)}$"
        ):
            call(**{keyword: number})


@pytest.mark.parametrize(("name", "call", "keyword", "tokens", "retired"), CALLS, ids=IDS)
def test_a_retired_spelling_names_the_tokens(
    name: str, call: Callable[..., Any], keyword: str, tokens: tuple[str, ...], retired: str
) -> None:
    with pytest.raises(SonareValueError) as caught:
        call(**{keyword: retired})
    assert str(caught.value) == f"{name}: {keyword} must be one of {', '.join(tokens)}"
