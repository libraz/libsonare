from __future__ import annotations

import numpy as np
import pytest

import libsonare
from libsonare import _runtime
from libsonare._errors import _entry


def _signal() -> np.ndarray:
    sample_rate = 22050
    length = sample_rate // 2
    time = np.arange(length, dtype=np.float32) / np.float32(sample_rate)
    noise = np.random.default_rng(73).normal(0.0, 0.03, length).astype(np.float32)
    return noise + 0.2 * np.sin(2.0 * np.pi * 440.0 * time)


def test_istft_reconstructs_stft_with_phase_with_scale_tolerance() -> None:
    samples = _signal()
    spectrum = libsonare.stft(samples, sr=22050, n_fft=2048, hop_length=512, phase=True)
    assert spectrum.phase is not None

    reconstructed = libsonare.istft(
        spectrum.magnitude,
        spectrum.phase,
        n_fft=2048,
        hop_length=512,
        length_samples=len(samples),
    )
    edge = spectrum.n_fft // 2
    assert reconstructed.shape == samples.shape
    scale = float(np.max(np.abs(samples)))
    np.testing.assert_allclose(
        reconstructed[edge:-edge],
        samples[edge:-edge],
        rtol=0.0,
        atol=1e-4 * scale,
    )


def test_istft_infers_fft_and_positive_quarter_hop() -> None:
    spectrum = libsonare.stft(_signal(), sr=22050, n_fft=2048, hop_length=512, phase=True)
    assert spectrum.phase is not None

    inferred = libsonare.istft(spectrum.magnitude, spectrum.phase)
    explicit = libsonare.istft(
        spectrum.magnitude,
        spectrum.phase,
        n_fft=2048,
        hop_length=512,
    )
    assert inferred.size > 0
    np.testing.assert_array_equal(inferred, explicit)


def test_istft_refuses_shape_mismatch_and_zero_length_sentinel() -> None:
    magnitude = np.ones((1025, 4), dtype=np.float32)
    phase = np.zeros_like(magnitude)
    with pytest.raises(
        libsonare.SonareValueError, match="magnitude and phase must have the same shape"
    ) as mismatch:
        libsonare.istft(magnitude, phase[:, :-1])
    assert mismatch.value.entry == "istft"

    with pytest.raises(libsonare.SonareValueError, match="length_samples") as zero:
        libsonare.istft(magnitude, phase, n_fft=2048, hop_length=512, length_samples=0)
    assert zero.value.entry == "istft"


def test_istft_refuses_length_above_limit_and_non_cola_pair() -> None:
    magnitude = np.ones((1025, 4), dtype=np.float32)
    phase = np.zeros_like(magnitude)
    with pytest.raises(libsonare.SonareValueError, match="length_samples") as long:
        libsonare.istft(
            magnitude,
            phase,
            n_fft=2048,
            hop_length=512,
            length_samples=500_000_001,
        )
    assert long.value.entry == "istft"

    with pytest.raises(libsonare.SonareValueError) as cola:
        libsonare.istft(magnitude, phase, n_fft=2048, hop_length=1025)
    assert "n_fft" in str(cola.value)
    assert "hop_length" in str(cola.value)
    assert cola.value.entry == "istft"


def test_native_reason_is_not_reused_for_a_facade_guard() -> None:
    with pytest.raises(libsonare.SonareError) as native:
        libsonare.Project.from_json("{")
    assert native.value.reason == "malformedJson"

    reasonless = _runtime._coded_error(int(native.value.code), "reasonless guard")
    assert reasonless.reason is None

    with pytest.raises(libsonare.SonareValueError) as guard:
        libsonare.istft(np.ones((3, 2)), np.ones((3, 1)))
    assert guard.value.reason is None
    assert guard.value.entry == "istft"


def test_unknown_error_reason_token_becomes_invalid_format(monkeypatch: pytest.MonkeyPatch) -> None:
    class ReasonLibrary:
        @staticmethod
        def sonare_last_error_reason() -> bytes:
            return b"futureReason"

    monkeypatch.setattr(_runtime, "_get_lib", lambda: ReasonLibrary())

    @_entry("reason_probe")
    def fail_with_unknown_reason() -> None:
        raise _runtime._coded_error(
            int(libsonare.ErrorCode.NOT_SUPPORTED), "native refusal", native_reason=True
        )

    with pytest.raises(libsonare.SonareError) as unknown:
        fail_with_unknown_reason()
    assert unknown.value.code == libsonare.ErrorCode.INVALID_FORMAT
    assert unknown.value.reason is None
    assert unknown.value.entry == "reason_probe"
    assert str(unknown.value) == "[2] reason_probe: unknown error reason token: futureReason"


def test_invalid_parameter_does_not_carry_a_native_reason(monkeypatch: pytest.MonkeyPatch) -> None:
    class ReasonLibrary:
        @staticmethod
        def sonare_last_error_reason() -> bytes:
            return b"malformedJson"

    monkeypatch.setattr(_runtime, "_get_lib", lambda: ReasonLibrary())
    error = _runtime._coded_error(
        int(libsonare.ErrorCode.INVALID_PARAMETER), "invalid argument", native_reason=True
    )
    assert isinstance(error, libsonare.SonareValueError)
    assert error.reason is None
