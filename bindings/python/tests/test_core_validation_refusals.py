"""Refusals owned by the core function, pinned by the cross-surface corpus."""

from __future__ import annotations

import ctypes
import json
import re
from collections.abc import Callable
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

_ERROR_CLASS = {"argument": SonareValueError, "missing_argument": SonareValueError}

_MFCC = _CORPUS["mfcc_to_mel"]
_MEL = _CORPUS["mel_to_audio"]
_MFCC_AUDIO = _CORPUS["mfcc_to_audio"]
_GRIFFIN_LIM = _CORPUS["griffin_lim"]
_STRETCH = _CORPUS["native_spectral_stretch"]
_CHAIN = _CORPUS["streaming_mastering_chain_prepare"]
_EQ = _CORPUS["equalizer_prepare"]
_RETUNE = _CORPUS["streaming_retune_prepare"]
_RETUNE_CONFIG = _CORPUS["streaming_retune_config"]
_RESAMPLE = _CORPUS["resample"]
_ZERO_CROSSINGS = _CORPUS["zero_crossings"]
_MASTER = _CORPUS["master_audio"]

# Every function the C headers declare: the oracle for "this prefix is a C entry name".
_C_ENTRIES = frozenset(
    name
    for header in (Path(__file__).resolve().parents[3] / "include/sonare").glob("*.h")
    for name in re.findall(r"\b(sonare_[a-z0-9_]+)\s*\(", header.read_text())
)
_DOUBLE_PREFIX = re.compile(r"^(?:\[\d+\] )?([a-z][a-z0-9_]*): ([a-z][a-z0-9_]*): ")


def _without_c_entry(message: str) -> str:
    """The core text as the binding raises it: a leading C entry name is dropped."""
    head, sep, rest = message.partition(": ")
    return rest if sep and f"sonare_{head}" in _C_ENTRIES else message


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
    with pytest.raises(error_class) as caught:
        libsonare.mfcc_to_mel(_mfcc(), n_mels=case["n_mels"])
    assert str(caught.value) == f"mfcc_to_mel: {case['message']}"
    with pytest.raises(error_class) as caught:
        libsonare.mfcc_to_audio(_mfcc(), n_mels=case["n_mels"], n_iter=1)
    assert str(caught.value) == f"mfcc_to_audio: {case['message']}"


def _mel() -> np.ndarray:
    shape = (_MEL["input"]["n_mels"], _MEL["input"]["n_frames"])
    return np.full(shape, _MEL["input"]["fill"], dtype=np.float32)


@pytest.mark.parametrize("case", _MEL["accepted"], ids=lambda case: case["id"])
def test_mel_to_audio_accepts(case: dict) -> None:
    assert libsonare.mel_to_audio(_mel(), n_iter=case["n_iter"]).size > 0


@pytest.mark.parametrize("case", _MEL["refused"], ids=lambda case: case["id"])
def test_mel_to_audio_refuses(case: dict) -> None:
    with pytest.raises(_ERROR_CLASS[case["cause"]]) as caught:
        libsonare.mel_to_audio(_mel(), n_iter=case["n_iter"])
    assert str(caught.value) == f"mel_to_audio: {case['message']}"


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
    with pytest.raises(_ERROR_CLASS[case["cause"]]) as caught:
        libsonare.mfcc_to_audio(_mfcc_audio(), n_mels=n_mels, n_iter=case["n_iter"])
    assert str(caught.value) == f"mfcc_to_audio: {case['message']}"


def _griffin_lim_magnitude(n_fft: int) -> np.ndarray:
    shape = (n_fft // 2 + 1, _GRIFFIN_LIM["input"]["n_frames"])
    return np.full(shape, _GRIFFIN_LIM["input"]["fill"], dtype=np.float32)


def _inverse_routes(n_fft: int) -> dict[str, Callable[[int | None], np.ndarray]]:
    n_mels = _MFCC_AUDIO["input"]["n_mels"]
    sample_rate = _GRIFFIN_LIM["input"]["sample_rate"]
    return {
        "mel_to_audio": lambda hop: libsonare.mel_to_audio(
            _mel(), n_fft=n_fft, hop_length=hop, n_iter=1
        ),
        "mfcc_to_audio": lambda hop: libsonare.mfcc_to_audio(
            _mfcc_audio(), n_mels=n_mels, n_fft=n_fft, hop_length=hop, n_iter=1
        ),
        "griffin_lim": lambda hop: libsonare.griffin_lim(
            _griffin_lim_magnitude(n_fft), sample_rate, hop_length=hop, n_iter=1
        ),
    }


def _inverse_cases(key: str) -> list[tuple[str, dict]]:
    return [(route, case) for route in _inverse_routes(4) for case in _CORPUS[route][key]]


@pytest.mark.parametrize(
    ("route", "case"), _inverse_cases("absent_hop"), ids=lambda value: str(value)[:24]
)
def test_inverse_transform_derives_an_absent_hop(route: str, case: dict) -> None:
    # The facade spells an absent hop by omission; the C 0 the corpus names is refused.
    call = _inverse_routes(case["n_fft"])[route]
    np.testing.assert_array_equal(call(None), call(case["derived_hop_length"]))
    with pytest.raises(
        SonareValueError, match=rf"^{route}: hop_length must be an integer in \[1, "
    ):
        call(case["hop_length"])


@pytest.mark.parametrize(
    ("route", "case"), _inverse_cases("negative_hop"), ids=lambda value: str(value)[:24]
)
def test_inverse_transform_refuses_a_negative_hop(route: str, case: dict) -> None:
    # The facade refuses before the core does, so only the error class is pinned here.
    with pytest.raises(_ERROR_CLASS[case["cause"]]):
        _inverse_routes(case["n_fft"])[route](case["hop_length"])


def _stretch_samples() -> np.ndarray:
    count = _STRETCH["input"]["length"]
    return np.full(count, _STRETCH["input"]["fill"], dtype=np.float32)


@pytest.mark.parametrize("case", _STRETCH["accepted"], ids=lambda case: case["id"])
def test_native_spectral_stretch_accepts(case: dict) -> None:
    args = (_stretch_samples(), _STRETCH["input"]["sample_rate"])
    geometry = {"n_fft": case["n_fft"], "hop_length": case["hop_length"]}
    assert libsonare.time_stretch(*args, _STRETCH["input"]["speed_ratio"], **geometry).size > 0
    assert libsonare.pitch_shift(*args, _STRETCH["input"]["pitch_semitones"], **geometry).size > 0


@pytest.mark.parametrize("case", _STRETCH["absent_hop"], ids=lambda case: case["id"])
def test_native_spectral_stretch_derives_an_absent_hop(case: dict) -> None:
    # The facade spells an absent hop by omission; the C 0 the corpus names is refused.
    args = (_stretch_samples(), _STRETCH["input"]["sample_rate"])
    derived = {"n_fft": case["n_fft"], "hop_length": case["derived_hop_length"]}
    omitted = {"n_fft": case["n_fft"]}
    for fn, amount in (
        (libsonare.time_stretch, _STRETCH["input"]["speed_ratio"]),
        (libsonare.pitch_shift, _STRETCH["input"]["pitch_semitones"]),
    ):
        np.testing.assert_array_equal(fn(*args, amount, **omitted), fn(*args, amount, **derived))
        with pytest.raises(SonareValueError, match=r"hop_length must be an integer in \[1, "):
            fn(*args, amount, n_fft=case["n_fft"], hop_length=case["hop_length"])


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


_RETUNE_KEYWORD = {
    "semitones": "pitch_semitones",
    "mix": "mix",
    "grain_size": "grain_size_samples",
}


def _retune_controls(case: dict) -> dict[str, float | int | None]:
    """The corpus row as keywords; grain 0 is the C spelling of an omitted grain."""
    grain = case["grain_size"]
    return {
        "pitch_semitones": float(case["semitones"]),
        "mix": float(case["mix"]),
        "grain_size_samples": None if grain == 0 else grain,
    }


@pytest.mark.parametrize("case", _RETUNE_CONFIG["accepted"], ids=lambda case: case["id"])
def test_streaming_retune_config_accepts(case: dict) -> None:
    controls = _retune_controls(case)
    with libsonare.StreamingRetune(**controls) as retune:  # type: ignore[arg-type]
        assert retune.config() == pytest.approx(controls)
        retune.set_config(**controls)  # type: ignore[arg-type]


@pytest.mark.parametrize("case", _RETUNE_CONFIG["refused"], ids=lambda case: case["id"])
def test_streaming_retune_config_refuses(case: dict) -> None:
    """The constructor and set_config refuse by name, and a refused update changes nothing."""
    controls = _retune_controls(case)
    message = f"{_RETUNE_KEYWORD[case['field']]} {case['predicate']}"
    error_class = _ERROR_CLASS[case["cause"]]
    with pytest.raises(error_class, match=re.escape(f"StreamingRetune: {message}")):
        libsonare.StreamingRetune(**controls)  # type: ignore[arg-type]
    with libsonare.StreamingRetune(pitch_semitones=3.0, mix=0.5) as retune:
        before = retune.config()
        with pytest.raises(error_class, match=re.escape(f"StreamingRetune.set_config: {message}")):
            retune.set_config(**controls)  # type: ignore[arg-type]
        assert retune.config() == before


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
    # read through the ctypes wrapper that reaches the C entry directly, without its C name.
    with pytest.raises(error_class, match=re.escape(_without_c_entry(case["message"]))):
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
    # The facade's buffer guard names a non-finite sample first, so the C entry's message is
    # read through a direct call to it, without its C name.
    expected = case.get("c_message", case["message"])
    with pytest.raises(error_class, match=re.escape(_without_c_entry(expected))):
        _zero_crossings_core(samples)


def _master_tone() -> np.ndarray:
    spec = _MASTER["input"]
    t = np.arange(spec["length"]) / spec["sample_rate"]
    return (spec["amplitude"] * np.sin(2 * np.pi * spec["frequency_hz"] * t)).astype(np.float32)


@pytest.mark.parametrize("case", _MASTER["refused"], ids=lambda case: case["id"])
@pytest.mark.parametrize("prefix", ["master_audio", "master_audio_stereo"])
def test_master_audio_refuses(case: dict, prefix: str) -> None:
    tone, rate = _master_tone(), _MASTER["input"]["sample_rate"]
    call = {
        "master_audio": lambda: libsonare.master_audio(tone, rate),
        "master_audio_stereo": lambda: libsonare.master_audio_stereo(tone, tone, rate),
    }[prefix]
    with pytest.raises(
        _ERROR_CLASS[case["cause"]], match=re.escape(f"{prefix}: {case['message']}")
    ):
        call()
    if prefix == "master_audio":
        with pytest.raises(
            _ERROR_CLASS[case["cause"]], match=re.escape(f"{prefix}: {case['message']}")
        ):
            libsonare.Audio.from_buffer(tone, rate).master_audio()


@pytest.mark.parametrize("case", _MASTER["accepted"], ids=lambda case: case["id"])
def test_master_audio_accepts(case: dict) -> None:
    assert case["returns"] == "input"
    tone, rate = _master_tone(), _MASTER["input"]["sample_rate"]
    np.testing.assert_array_equal(
        libsonare.master_audio(tone, rate, overrides=case["overrides"]).samples, tone
    )
    pair = libsonare.master_audio_stereo(tone, tone, rate, overrides=case["overrides"])
    np.testing.assert_array_equal(pair.left, tone)
    np.testing.assert_array_equal(pair.right, tone)
    audio = libsonare.Audio.from_buffer(tone, rate)
    np.testing.assert_array_equal(audio.master_audio(overrides=case["overrides"]).samples, tone)


def _public_refusals() -> list[tuple[str, dict, Callable[[], object]]]:
    def retune(case: dict) -> None:
        with libsonare.StreamingRetune() as stream:
            stream.prepare(case["sample_rate"], case["max_block_size"])

    def chain(case: dict) -> None:
        libsonare.StreamingMasteringChain(
            sample_rate=case["sample_rate"],
            num_channels=_CHAIN["input"]["num_channels"],
            max_block_size=case["max_block_size"],
        )

    n_mels = _MFCC_AUDIO["input"]["n_mels"]
    tone, rate = _master_tone(), _MASTER["input"]["sample_rate"]
    routes: dict[str, Callable[[dict], object]] = {
        "mfcc_to_mel": lambda c: libsonare.mfcc_to_mel(_mfcc(), n_mels=c["n_mels"]),
        "mel_to_audio": lambda c: libsonare.mel_to_audio(_mel(), n_iter=c["n_iter"]),
        "mfcc_to_audio": lambda c: libsonare.mfcc_to_audio(
            _mfcc_audio(), n_mels=n_mels, n_iter=c["n_iter"]
        ),
        "streaming_mastering_chain_prepare": chain,
        "equalizer_prepare": lambda c: libsonare.StreamingEqualizer(
            sample_rate=c["sample_rate"], max_block_size=c["max_block_size"]
        ),
        "streaming_retune_prepare": retune,
        "master_audio": lambda c: libsonare.master_audio(tone, rate),
    }
    return [
        (key, case, (lambda route=route, case=case: route(case)))
        for key, route in routes.items()
        for case in _CORPUS[key]["refused"]
    ]


_PUBLIC_REFUSALS = _public_refusals()


@pytest.mark.parametrize(
    ("key", "case", "call"),
    _PUBLIC_REFUSALS,
    ids=[f"{key}-{case['id']}" for key, case, _ in _PUBLIC_REFUSALS],
)
def test_refusal_names_no_c_entry_after_the_public_name(
    key: str, case: dict, call: Callable[[], object]
) -> None:
    # The public entry names itself; the C entry it reached must not follow that name.
    with pytest.raises((SonareValueError, TypeError)) as caught:
        call()
    match = _DOUBLE_PREFIX.match(str(caught.value))
    if match is not None:
        token = match.group(2)
        assert f"sonare_{token}" not in _C_ENTRIES and token not in _C_ENTRIES, str(caught.value)
    assert _without_c_entry(case["message"]) in str(caught.value)
