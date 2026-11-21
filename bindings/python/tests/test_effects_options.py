"""Tests for configurable effect entry points."""

from __future__ import annotations

from types import SimpleNamespace

import numpy as np
import pytest

import libsonare._effects_level as level
import libsonare._effects_repair_dereverb as repair_dereverb
import libsonare._effects_repair_noise as repair_noise
import libsonare._effects_separation as separation
import libsonare._effects_spectral as spectral
import libsonare._effects_timepitch as timepitch
from libsonare import ErrorCode, SonareValueError
from libsonare._runtime import _get_lib


class _Call:
    def __init__(self) -> None:
        self.calls: list[tuple[object, ...]] = []

    def __call__(self, *args: object) -> int:
        self.calls.append(args)
        return 0


def _samples() -> np.ndarray:
    return np.ones(32, dtype=np.float32)


def test_transform_options_use_new_symbols(monkeypatch: pytest.MonkeyPatch) -> None:
    calls: list[tuple[str, tuple[object, ...]]] = []

    def fake_transform(name: str, *args: object, flattened: str | None = None) -> np.ndarray:
        assert flattened == "options"
        calls.append((name, args))
        return np.zeros(1, dtype=np.float32)

    stretch = SimpleNamespace(sonare_time_stretch=object())
    shift = SimpleNamespace(sonare_pitch_shift=object())
    monkeypatch.setattr(timepitch, "_call_float_array_transform", fake_transform)

    monkeypatch.setattr(timepitch, "_get_lib", lambda: stretch)
    np.testing.assert_array_equal(
        timepitch.time_stretch(
            _samples(), speed_ratio=1.0, n_fft=1024, hop_length=256, sample_rate=22050
        ),
        [0.0],
    )
    monkeypatch.setattr(timepitch, "_get_lib", lambda: shift)
    np.testing.assert_array_equal(
        timepitch.pitch_shift(
            _samples(), pitch_semitones=0.0, n_fft=1024, hop_length=256, sample_rate=22050
        ),
        [0.0],
    )

    for index, name in enumerate(("sonare_time_stretch", "sonare_pitch_shift")):
        assert calls[index][0] == name
        options = calls[index][1][3]._obj
        assert (options.n_fft, options.hop_length) == (1024, 256)


def test_hpss_options_reach_the_native_call(monkeypatch: pytest.MonkeyPatch) -> None:
    new_call = _Call()
    new = SimpleNamespace(
        sonare_hpss=new_call,
        sonare_free_hpss_result=lambda *_args: None,
    )
    monkeypatch.setattr(separation, "_get_lib", lambda: new)
    result = separation.hpss(
        _samples(), n_fft=1024, hop_length=256, hard_mask=True, sample_rate=22050
    )
    assert result.length == 0
    assert len(new_call.calls) == 1
    options = new_call.calls[0][5]._obj
    assert (options.n_fft, options.hop_length, options.use_soft_mask) == (1024, 256, 0)
    assert new_call.calls[0][7] is None


def test_an_omitted_hpss_hop_reaches_the_native_call_as_absent(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    new_call = _Call()
    new = SimpleNamespace(
        sonare_hpss=new_call,
        sonare_free_hpss_result=lambda *_args: None,
    )
    monkeypatch.setattr(separation, "_get_lib", lambda: new)
    separation.hpss(_samples(), n_fft=1024, sample_rate=22050)
    # A hop of 0 in the options struct is "absent": the core derives it.
    assert new_call.calls[0][5]._obj.hop_length == 0


def test_hpss_with_residual_options_reach_the_native_call(monkeypatch: pytest.MonkeyPatch) -> None:
    new_call = _Call()
    new = SimpleNamespace(
        sonare_hpss=new_call,
        sonare_free_hpss_result=lambda *_args: None,
        sonare_free_floats=lambda *_args: None,
    )
    monkeypatch.setattr(separation, "_get_lib", lambda: new)
    result = separation.hpss(
        _samples(),
        n_fft=1024,
        hop_length=256,
        hard_mask=True,
        with_residual=True,
        sample_rate=22050,
    )
    assert result.residual is not None
    assert len(new_call.calls) == 1
    options = new_call.calls[0][5]._obj
    assert (options.n_fft, options.hop_length, options.use_soft_mask) == (1024, 256, 0)
    assert new_call.calls[0][7] is not None


def test_trim_and_rms_normalization_symbols(monkeypatch: pytest.MonkeyPatch) -> None:
    trim_call = _Call()
    trim_lib = SimpleNamespace(sonare_trim=trim_call)
    monkeypatch.setattr(level, "_get_lib", lambda: trim_lib)
    assert level.trim(_samples(), frame_length=64, hop_length=16, sample_rate=22050).size == 0
    options = trim_call.calls[0][4]._obj
    assert (options.frame_length, options.hop_length) == (64, 16)

    rms_call = _Call()
    rms_lib = SimpleNamespace(sonare_normalize_rms=rms_call)
    monkeypatch.setattr(level, "_get_lib", lambda: rms_lib)
    assert level.normalize(_samples(), sample_rate=22050, mode="rms", target_db=-12.0).size == 0
    assert float(rms_call.calls[0][3].value) == pytest.approx(-12.0)


def test_invalid_options_are_rejected_before_library_lookup(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def fail_lookup() -> object:
        raise AssertionError("native library lookup must not happen")

    # Each entry point resolves _get_lib through its own module, so the guard
    # has to be installed on every module whose refusal is asserted below --
    # one left unpatched would still raise, from a path the guard never saw.
    monkeypatch.setattr(timepitch, "_get_lib", fail_lookup)
    with pytest.raises(ValueError, match="n_fft"):
        timepitch.time_stretch(_samples(), speed_ratio=1.0, n_fft=2**32, sample_rate=22050)

    monkeypatch.setattr(separation, "_get_lib", fail_lookup)
    with pytest.raises(ValueError, match="kernel_harmonic"):
        separation.hpss(_samples(), kernel_harmonic=2**32 + 1, sample_rate=22050)
    with pytest.raises(ValueError, match="hop_length"):
        separation.hpss(_samples(), hop_length=2**32, with_residual=True, sample_rate=22050)

    monkeypatch.setattr(level, "_get_lib", fail_lookup)
    with pytest.raises(ValueError, match="frame_length"):
        level.trim(_samples(), frame_length=0, sample_rate=22050)
    with pytest.raises(ValueError, match="hop_length"):
        level.trim(_samples(), hop_length=2**32, sample_rate=22050)
    with pytest.raises(
        ValueError, match=r"target_db must be finite and in \[-3\.4028235e38, 3\.4028235e38\]"
    ):
        level.normalize(_samples(), sample_rate=22050, mode="rms", target_db=-1e100)


def _has_extended_effects() -> bool:
    try:
        lib = _get_lib()
    except Exception:
        return False
    return all(hasattr(lib, name) for name in ("sonare_normalize_rms",))


@pytest.mark.skipif(not _has_extended_effects(), reason="extended effects symbols unavailable")
def test_extended_effects_work_with_fresh_library() -> None:
    samples = np.sin(np.linspace(0.0, 8.0 * np.pi, 4096, dtype=np.float32)) * 0.25
    stretched = timepitch.time_stretch(
        samples, speed_ratio=1.1, n_fft=1024, hop_length=256, sample_rate=22050
    )
    shifted = timepitch.pitch_shift(
        samples, pitch_semitones=2.0, n_fft=1024, hop_length=256, sample_rate=22050
    )
    separated = separation.hpss(
        samples, n_fft=1024, hop_length=256, hard_mask=True, sample_rate=22050
    )
    separated_residual = separation.hpss(
        samples,
        n_fft=1024,
        hop_length=256,
        hard_mask=True,
        with_residual=True,
        sample_rate=22050,
    )
    normalized = level.normalize(samples, sample_rate=22050, mode="rms", target_db=-12.0)
    trimmed = level.trim(
        samples, threshold_db=-40.0, frame_length=1024, hop_length=256, sample_rate=22050
    )

    assert len(stretched) > 0
    assert len(shifted) > 0
    assert separated.length == len(samples)
    assert len(separated_residual.residual) == len(samples)
    assert len(normalized) == len(samples)
    assert len(trimmed) > 0


@pytest.mark.skipif(not _has_extended_effects(), reason="extended effects symbols unavailable")
def test_effects_reject_a_hop_below_the_half_window_overlap_contract() -> None:
    samples = np.sin(np.linspace(0.0, 8.0 * np.pi, 4096, dtype=np.float32)) * 0.25

    for call in (
        lambda: separation.hpss(samples, n_fft=1024, hop_length=1024, sample_rate=22050),
        lambda: separation.hpss(
            samples, n_fft=1024, hop_length=1024, with_residual=True, sample_rate=22050
        ),
        lambda: timepitch.time_stretch(
            samples, speed_ratio=1.2, n_fft=512, hop_length=2048, sample_rate=22050
        ),
        lambda: timepitch.pitch_shift(
            samples, pitch_semitones=3.0, n_fft=1024, hop_length=1024, sample_rate=22050
        ),
        lambda: separation.phase_vocoder(
            samples, speed_ratio=1.2, n_fft=1024, hop_length=1024, sample_rate=22050
        ),
    ):
        with pytest.raises(SonareValueError) as error:
            call()
        assert error.value.code == ErrorCode.INVALID_PARAMETER


@pytest.mark.skipif(not _has_extended_effects(), reason="extended effects symbols unavailable")
def test_effects_accept_an_even_n_fft_that_is_not_a_power_of_two() -> None:
    # The core FFT is mixed-radix; the facade used to require a power of two,
    # which made the same call succeed on the C ABI and fail here.
    samples = np.sin(np.linspace(0.0, 8.0 * np.pi, 4096, dtype=np.float32)) * 0.25

    separated = separation.hpss(samples, n_fft=1500, hop_length=250, sample_rate=22050)
    assert separated.length == len(samples)
    assert (
        len(
            timepitch.time_stretch(
                samples, speed_ratio=1.2, n_fft=1500, hop_length=250, sample_rate=22050
            )
        )
        > 0
    )
    assert (
        len(
            timepitch.pitch_shift(
                samples, pitch_semitones=3.0, n_fft=1500, hop_length=250, sample_rate=22050
            )
        )
        > 0
    )
    assert (
        len(
            separation.phase_vocoder(
                samples, speed_ratio=1.2, n_fft=1500, hop_length=250, sample_rate=22050
            )
        )
        > 0
    )


@pytest.mark.skipif(not _has_extended_effects(), reason="extended effects symbols unavailable")
def test_the_n_fft_verdict_matches_the_core_for_both_families() -> None:
    # The accepted n_fft domain is a cross-surface contract anchored on the core,
    # and the core draws it in two places, not one. The COLA family takes any
    # even size because the FFT is mixed-radix; spectral_edit and the two repair
    # entry points additionally require a power of two, each in its own core
    # source. A facade must not narrow the first set (that is a divergence) and
    # must not widen the second (that turns an eager, named rejection into a
    # generic invalid-parameter return from the core). 1500 is even and not a
    # power of two, so one size separates the two families.
    samples = np.sin(np.linspace(0.0, 8.0 * np.pi, 8192, dtype=np.float32)) * 0.25

    # Mixed-radix family: accepted.
    assert separation.hpss(samples, n_fft=1500, hop_length=250, sample_rate=22050).length == len(
        samples
    )
    assert (
        len(
            separation.phase_vocoder(
                samples, speed_ratio=1.2, n_fft=1500, hop_length=250, sample_rate=22050
            )
        )
        > 0
    )

    # Power-of-two family: rejected, by the facade rather than by the core, so
    # the message names the rule.
    with pytest.raises(ValueError, match="power of two"):
        spectral.spectral_edit(samples, 22050, [], n_fft=1500, hop_length=250)
    with pytest.raises(ValueError, match="power of two"):
        repair_noise.mastering_repair_denoise_classical(samples, 22050, n_fft=1500, hop_length=250)
    with pytest.raises(ValueError, match="power of two"):
        repair_dereverb.mastering_repair_dereverb_classical(
            samples, 22050, n_fft=1500, hop_length=250
        )

    # The same three accept a power of two, so the rejection above is the rule
    # and not the entry point refusing everything.
    assert len(spectral.spectral_edit(samples, 22050, [], n_fft=1024, hop_length=256)) == len(
        samples
    )
    assert (
        repair_noise.mastering_repair_denoise_classical(
            samples, 22050, n_fft=1024, hop_length=256
        ).samples.shape
        == samples.shape
    )
    assert (
        repair_dereverb.mastering_repair_dereverb_classical(
            samples, 22050, n_fft=1024, hop_length=256
        ).samples.shape
        == samples.shape
    )


@pytest.mark.skipif(not _has_extended_effects(), reason="extended effects symbols unavailable")
def test_hpss_residual_request_leaves_the_components_unchanged() -> None:
    samples = np.sin(np.linspace(0.0, 8.0 * np.pi, 4096, dtype=np.float32)) * 0.25
    plain = separation.hpss(samples, sample_rate=22050)
    with_residual = separation.hpss(samples, sample_rate=22050, with_residual=True)
    assert plain.residual is None
    assert with_residual.residual is not None
    # The residual path masks in a different order, so agreement is to float rounding.
    atol = 1e-6 * float(np.max(np.abs(samples)))
    np.testing.assert_allclose(plain.harmonic, with_residual.harmonic, rtol=0, atol=atol)
    np.testing.assert_allclose(plain.percussive, with_residual.percussive, rtol=0, atol=atol)
