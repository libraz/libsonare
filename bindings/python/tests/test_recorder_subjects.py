"""Structured refusals name the C argument while Python renders its public spelling."""

from __future__ import annotations

from collections.abc import Callable

import numpy as np
import pytest

import libsonare
from libsonare._runtime import _get_lib

_SR = 48000
_CALLS: dict[str, Callable[..., object]] = {
    "decompose_stems": libsonare.decompose_stems,
    "decompose_stems_linked": libsonare.decompose_stems_linked,
}


def _assert_record(entry: str, subject: str) -> None:
    lib = _get_lib()
    assert lib.sonare_last_refusal_entry() == entry.encode()
    assert lib.sonare_last_refusal_subject_count() == 1
    assert lib.sonare_last_refusal_subject(0) == subject.encode()


@pytest.mark.parametrize("entry", tuple(_CALLS))
def test_stem_work_budget_records_config(entry: str) -> None:
    shape = (8192,) if entry == "decompose_stems" else (2, 8192)
    with pytest.raises(libsonare.SonareValueError, match=r": n_components must keep"):
        _CALLS[entry](
            np.ones(shape, dtype=np.float32), _SR, n_components=500000001, n_fft=4, hop_length=1
        )
    _assert_record(entry, "config.n_components")


@pytest.mark.parametrize("format", ("flac", "mp3"))
def test_unavailable_encoder_records_format(format: str) -> None:
    if format in libsonare.capabilities().encode.ffmpeg:
        pytest.skip(f"{format} encoder available")
    with pytest.raises(libsonare.SonareError) as exc:
        libsonare.encode_audio([[0.0]], _SR, format)
    _assert_record("encode_audio", "options.format")
    assert exc.value.code == libsonare.ErrorCode.NOT_SUPPORTED
    assert exc.value.reason == "notCompiledIn"
    assert str(exc.value) == "[6] encode_audio: format is unavailable for encoding in this build"


@pytest.mark.parametrize("key", ("a=b", "private_key"))
def test_encoder_records_tag_key_beneath_options(key: str) -> None:
    with pytest.raises(libsonare.SonareValueError) as exc:
        libsonare.encode_audio([[0.0]], _SR, "wav", tags={key: "value"})
    _assert_record("encode_audio", f"options.tags.{key}")
    assert str(exc.value) == f"encode_audio: tags.{key} is not writable as WAV INFO"


def test_engine_records_binding_patch_preset() -> None:
    with libsonare.RealtimeEngine(sample_rate=_SR, max_block_size=64) as engine:
        for patch, subject in (
            ("no-such-preset", "patch"),
            (libsonare.SynthPatch(preset="no-such-preset"), "patch.preset"),
        ):
            with pytest.raises(libsonare.SonareValueError) as exc:
                engine.set_synth_instrument(patch, destination_id=3)
            _assert_record("engine_set_synth_instrument", "binding.patch.preset")
            assert str(exc.value).startswith(f"RealtimeEngine.set_synth_instrument: {subject} ")


def test_istft_records_options_length() -> None:
    magnitude = np.ones((3, 2), dtype=np.float32)
    phase = np.zeros_like(magnitude)
    with pytest.raises(libsonare.SonareValueError, match=r"^istft: length_samples must"):
        libsonare.istft(magnitude, phase, n_fft=4, hop_length=1, length_samples=500000001)
    _assert_record("istft", "options.length_samples")


def test_gate_records_config_range() -> None:
    with pytest.raises(
        libsonare.SonareValueError,
        match=r"^mastering_dynamics_gate: range_db must be finite and at most 0$",
    ):
        libsonare.mastering_dynamics_gate([0.0] * 128, _SR, range_db=1.0)
    _assert_record("mastering_dynamics_gate", "config.range_db")


@pytest.mark.parametrize(
    "field",
    (
        "cents_per_bin",
        "min_duration_ms",
        "min_frame_peak_ratio",
        "min_ridge_peak_ratio",
        "min_separation_cents",
    ),
)
def test_polyphonic_analysis_records_caller_config(field: str) -> None:
    option = "min_ridge_duration_ms" if field == "min_duration_ms" else field
    samples = np.sin(np.arange(4096, dtype=np.float32) * (2.0 * np.pi * 440.0 / _SR))
    with pytest.raises(libsonare.SonareValueError) as exc:
        libsonare.PolyphonicAnalysis(samples, _SR, n_fft=256, hop_length=64, **{option: -1.0})
    _assert_record("polyphonic_analyze", f"config.{option}")
    assert str(exc.value).startswith(f"PolyphonicAnalysis: {option} must ")
