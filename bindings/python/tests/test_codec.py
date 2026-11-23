from __future__ import annotations

import math

import numpy as np
import pytest

import libsonare


def _samples() -> np.ndarray:
    time = np.arange(4096, dtype=np.float32) / np.float32(48000)
    left = 0.25 * np.sin(2.0 * np.pi * 440.0 * time)
    right = 0.15 * np.sin(2.0 * np.pi * 660.0 * time)
    return np.stack((left, right)).astype(np.float32)


def test_stft_phase_is_optional_for_function_and_audio_method() -> None:
    samples = _samples()[0]
    plain = libsonare.stft(samples, sr=48000, n_fft=2048, hop_length=512)
    phased = libsonare.stft(samples, sr=48000, n_fft=2048, hop_length=512, phase=True)
    assert plain.phase is None
    assert phased.phase is not None
    assert phased.phase.dtype == np.float32
    assert phased.phase.shape == phased.magnitude.shape

    audio = libsonare.Audio.from_buffer(samples, sample_rate=48000)
    assert audio.stft().phase is None
    audio_phase = audio.stft(phase=True).phase
    assert audio_phase is not None
    assert audio_phase.shape == phased.magnitude.shape

    with pytest.raises(TypeError, match=r"^stft: phase must be a bool$"):
        libsonare.stft(samples, sr=48000, phase="true")  # type: ignore[arg-type]
    with pytest.raises(TypeError, match=r"^Audio\.stft: phase must be a bool$"):
        audio.stft(phase="true")  # type: ignore[arg-type]
    audio.close()
    with pytest.raises(libsonare.SonareError) as refused:
        audio.stft()
    assert refused.value.code == libsonare.ErrorCode.INVALID_STATE
    assert refused.value.entry == "Audio.stft"
    assert refused.value.reason == "releasedHandle"


def test_encode_wav_round_trips_samples_and_tags() -> None:
    channels = _samples()
    tags = {"title": "A test", "artist": "Sonare", "comment": "round trip"}
    encoded = libsonare.encode_audio(channels, 48000, "wav", tags=tags)

    assert isinstance(encoded, bytes)
    assert libsonare.read_audio_tags(encoded) == tags
    decoded = libsonare.decode_channels(encoded)
    assert decoded.sample_rate == 48000
    assert decoded.channels.shape == channels.shape
    # The default PCM16 encoder adds up to one LSB of dither before rounding.
    np.testing.assert_allclose(
        decoded.channels,
        channels,
        rtol=0.0,
        atol=1.5 / 32768.0 + np.finfo(np.float32).eps,
    )


def test_encode_refuses_unknown_format_and_explicit_default_sentinel() -> None:
    channels = _samples()
    with pytest.raises(libsonare.SonareValueError) as unknown:
        libsonare.encode_audio(channels, 48000, "WAV")
    assert unknown.value.code == libsonare.ErrorCode.INVALID_PARAMETER
    assert unknown.value.entry == "encode_audio"
    assert unknown.value.reason is None

    with pytest.raises(libsonare.SonareValueError, match="bit_depth must not be 0") as sentinel:
        libsonare.encode_audio(channels, 48000, "wav", bit_depth=0)
    assert sentinel.value.entry == "encode_audio"
    assert sentinel.value.reason is None

    with pytest.raises(libsonare.SonareValueError, match="quality must be finite"):
        libsonare.encode_audio(channels, 48000, "wav", quality=float("nan"))


def test_encode_refuses_tags_that_wav_cannot_store() -> None:
    with pytest.raises(libsonare.SonareValueError, match="tags.private_key") as refused:
        libsonare.encode_audio(_samples(), 48000, "wav", tags={"private_key": "value"})
    assert refused.value.entry == "encode_audio"


def test_encode_refuses_format_specific_values() -> None:
    channels = _samples()
    with pytest.raises(libsonare.SonareValueError, match="bit_depth") as depth:
        libsonare.encode_audio(channels, 48000, "wav", bit_depth=8)
    assert depth.value.entry == "encode_audio"

    with pytest.raises(libsonare.SonareValueError, match="quality") as quality:
        libsonare.encode_audio(channels, 48000, "wav", quality=0.5)
    assert quality.value.entry == "encode_audio"

    with pytest.raises(libsonare.SonareValueError, match="sample_rate") as sample_rate:
        libsonare.encode_audio(channels, 96000, "mp3")
    assert sample_rate.value.entry == "encode_audio"

    with pytest.raises(libsonare.SonareValueError, match=r"quality must be in \[0, 1\]"):
        libsonare.encode_audio(channels, 48000, "mp3", quality=math.nextafter(1.0, math.inf))


def test_encode_refuses_non_finite_samples() -> None:
    channels = _samples()
    channels[0, 17] = np.nan
    with pytest.raises(libsonare.SonareValueError, match=r"channels\[0\]") as refused:
        libsonare.encode_audio(channels, 48000, "wav")
    assert refused.value.entry == "encode_audio"


def test_read_tags_refuses_unknown_container() -> None:
    with pytest.raises(libsonare.SonareError) as refused:
        libsonare.read_audio_tags(b"not an audio container")
    assert refused.value.code == libsonare.ErrorCode.INVALID_FORMAT
    assert refused.value.entry == "read_audio_tags"
    assert refused.value.reason is None


def test_encode_unavailable_format_has_reason() -> None:
    formats = libsonare.capabilities().encode.ffmpeg
    unavailable = next(
        (token for token in ("flac", "mp3", "ogg", "opus", "m4a") if token not in formats),
        None,
    )
    if unavailable is None:
        pytest.skip("the linked FFmpeg provides every optional encoder")

    with pytest.raises(libsonare.SonareError) as refused:
        libsonare.encode_audio(_samples(), 48000, unavailable)
    assert refused.value.code == libsonare.ErrorCode.NOT_SUPPORTED
    assert refused.value.entry == "encode_audio"
    assert refused.value.reason == "notCompiledIn"


def test_error_reason_and_entry_are_exposed() -> None:
    with pytest.raises(libsonare.SonareError) as refused:
        libsonare.Project.from_json("{")
    assert refused.value.code == libsonare.ErrorCode.INVALID_FORMAT
    assert refused.value.entry == "Project.from_json"
    assert refused.value.reason == "malformedJson"


def test_capabilities_and_catalog_expose_encode_formats() -> None:
    capabilities = libsonare.capabilities()
    catalog = libsonare.capability_catalog()
    assert capabilities.encode.builtin == ("wav",)
    assert isinstance(capabilities.encode.ffmpeg, tuple)
    assert catalog.encode == capabilities.encode
