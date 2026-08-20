"""Tests for configurable effect entry points and legacy-library fallback."""

from __future__ import annotations

from types import SimpleNamespace

import numpy as np
import pytest

import libsonare._effects_editing as editing
import libsonare._effects_mastering as mastering
import libsonare._effects_separation as separation
from libsonare import ErrorCode, SonareError
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

    def fake_transform(name: str, *args: object) -> list[float]:
        calls.append((name, args))
        return [0.0]

    stretch = SimpleNamespace(sonare_time_stretch_ex=object())
    shift = SimpleNamespace(sonare_pitch_shift_ex=object())
    monkeypatch.setattr(editing, "_call_float_transform", fake_transform)

    monkeypatch.setattr(editing, "_get_lib", lambda: stretch)
    assert editing.time_stretch(_samples(), n_fft=1024, hop_length=256) == [0.0]
    monkeypatch.setattr(editing, "_get_lib", lambda: shift)
    assert editing.pitch_shift(_samples(), n_fft=1024, hop_length=256) == [0.0]

    assert calls[0][0] == "sonare_time_stretch_ex"
    assert [int(value.value) for value in calls[0][1][3:]] == [1024, 256]
    assert calls[1][0] == "sonare_pitch_shift_ex"
    assert [int(value.value) for value in calls[1][1][3:]] == [1024, 256]


def test_legacy_transform_defaults_fallback_but_custom_options_are_not_ignored(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[str] = []

    def fake_transform(name: str, *args: object) -> list[float]:
        calls.append(name)
        return [0.0]

    legacy = SimpleNamespace(
        sonare_time_stretch=object(),
        sonare_pitch_shift=object(),
    )
    monkeypatch.setattr(editing, "_call_float_transform", fake_transform)
    monkeypatch.setattr(editing, "_get_lib", lambda: legacy)

    editing.time_stretch(_samples())
    editing.pitch_shift(_samples())
    assert calls == ["sonare_time_stretch", "sonare_pitch_shift"]

    with pytest.raises(SonareError, match="sonare_time_stretch_ex") as stretch_error:
        editing.time_stretch(_samples(), n_fft=1024)
    assert stretch_error.value.code == ErrorCode.NOT_SUPPORTED
    with pytest.raises(SonareError, match="sonare_pitch_shift_ex") as shift_error:
        editing.pitch_shift(_samples(), hop_length=256)
    assert shift_error.value.code == ErrorCode.NOT_SUPPORTED


def test_hpss_options_and_legacy_fallback(monkeypatch: pytest.MonkeyPatch) -> None:
    new_call = _Call()
    new = SimpleNamespace(
        sonare_hpss_ex=new_call,
        sonare_free_hpss_result=lambda *_args: None,
    )
    monkeypatch.setattr(editing, "_get_lib", lambda: new)
    result = editing.hpss(_samples(), n_fft=1024, hop_length=256, hard_mask=True)
    assert result.length == 0
    assert len(new_call.calls) == 1
    assert [int(new_call.calls[0][index].value) for index in (5, 6, 7, 8)] == [1024, 256, 0, 0]

    legacy = SimpleNamespace(sonare_hpss=object())
    fallback: list[tuple[object, ...]] = []

    def fake_legacy(*args: object) -> object:
        fallback.append(args)
        return "legacy-result"

    monkeypatch.setattr(editing, "_get_lib", lambda: legacy)
    monkeypatch.setattr(editing, "_hpss_legacy", fake_legacy)
    assert editing.hpss(_samples()) == "legacy-result"
    assert len(fallback) == 1
    with pytest.raises(SonareError, match="sonare_hpss_ex"):
        editing.hpss(_samples(), hard_mask=True)


def test_hpss_with_residual_options_and_legacy_fallback(monkeypatch: pytest.MonkeyPatch) -> None:
    new_call = _Call()
    new = SimpleNamespace(
        sonare_hpss_ex=new_call,
        sonare_free_hpss_result=lambda *_args: None,
        sonare_free_floats=lambda *_args: None,
    )
    monkeypatch.setattr(separation, "_get_lib", lambda: new)
    result = separation.hpss_with_residual(_samples(), n_fft=1024, hop_length=256, hard_mask=True)
    assert set(result) == {"harmonic", "percussive", "residual", "sampleRate"}
    assert len(new_call.calls) == 1
    assert [int(new_call.calls[0][index].value) for index in (5, 6, 7, 8)] == [1024, 256, 0, 1]

    legacy = SimpleNamespace(sonare_hpss_with_residual=object())
    monkeypatch.setattr(separation, "_get_lib", lambda: legacy)
    monkeypatch.setattr(
        separation,
        "_hpss_with_residual_legacy",
        lambda *args: "legacy-result",
    )
    assert separation.hpss_with_residual(_samples()) == "legacy-result"
    with pytest.raises(SonareError, match="sonare_hpss_ex"):
        separation.hpss_with_residual(_samples(), hard_mask=True)


def test_trim_and_rms_normalization_symbols(monkeypatch: pytest.MonkeyPatch) -> None:
    trim_call = _Call()
    trim_lib = SimpleNamespace(sonare_trim_ex=trim_call)
    monkeypatch.setattr(mastering, "_get_lib", lambda: trim_lib)
    assert mastering.trim(_samples(), frame_length=64, hop_length=16) == []
    assert [int(trim_call.calls[0][index].value) for index in (4, 5)] == [64, 16]

    rms_call = _Call()
    rms_lib = SimpleNamespace(sonare_normalize_rms=rms_call)
    monkeypatch.setattr(mastering, "_get_lib", lambda: rms_lib)
    assert mastering.normalize_rms(_samples(), target_db=-12.0) == []
    assert float(rms_call.calls[0][3].value) == pytest.approx(-12.0)


def test_legacy_trim_fallback_and_rms_normalization_is_explicitly_unsupported(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    trim_call = _Call()
    legacy = SimpleNamespace(sonare_trim=trim_call)
    monkeypatch.setattr(mastering, "_get_lib", lambda: legacy)
    assert mastering.trim(_samples()) == []
    assert len(trim_call.calls) == 1
    with pytest.raises(SonareError, match="sonare_trim_ex"):
        mastering.trim(_samples(), frame_length=64)

    with pytest.raises(SonareError, match="sonare_normalize_rms") as error:
        mastering.normalize_rms(_samples())
    assert error.value.code == ErrorCode.NOT_SUPPORTED


def test_invalid_options_are_rejected_before_library_lookup(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def fail_lookup() -> object:
        raise AssertionError("native library lookup must not happen")

    monkeypatch.setattr(editing, "_get_lib", fail_lookup)
    with pytest.raises(ValueError, match="n_fft"):
        editing.time_stretch(_samples(), n_fft=1001)
    with pytest.raises(ValueError, match="n_fft"):
        editing.time_stretch(_samples(), n_fft=2**32)
    with pytest.raises(ValueError, match="kernel_harmonic"):
        editing.hpss(_samples(), kernel_harmonic=2**32 + 1)

    monkeypatch.setattr(separation, "_get_lib", fail_lookup)
    with pytest.raises(ValueError, match="hop_length"):
        separation.hpss_with_residual(_samples(), hop_length=2**32)

    monkeypatch.setattr(mastering, "_get_lib", fail_lookup)
    with pytest.raises(ValueError, match="frame_length"):
        mastering.trim(_samples(), frame_length=0)
    with pytest.raises(ValueError, match="hop_length"):
        mastering.trim(_samples(), hop_length=2**32)
    with pytest.raises(ValueError, match="float32"):
        mastering.normalize_rms(_samples(), target_db=-1e100)


def _has_extended_effects() -> bool:
    try:
        lib = _get_lib()
    except Exception:
        return False
    return all(
        hasattr(lib, name)
        for name in (
            "sonare_hpss_ex",
            "sonare_time_stretch_ex",
            "sonare_pitch_shift_ex",
            "sonare_normalize_rms",
            "sonare_trim_ex",
        )
    )


@pytest.mark.skipif(not _has_extended_effects(), reason="extended effects symbols unavailable")
def test_extended_effects_work_with_fresh_library() -> None:
    samples = np.sin(np.linspace(0.0, 8.0 * np.pi, 4096, dtype=np.float32)) * 0.25
    stretched = editing.time_stretch(samples, rate=1.1, n_fft=1024, hop_length=256)
    shifted = editing.pitch_shift(samples, semitones=2.0, n_fft=1024, hop_length=256)
    separated = editing.hpss(samples, n_fft=1024, hop_length=256, hard_mask=True)
    separated_residual = separation.hpss_with_residual(
        samples, n_fft=1024, hop_length=256, hard_mask=True
    )
    normalized = mastering.normalize_rms(samples, target_db=-12.0)
    trimmed = mastering.trim(samples, threshold_db=-40.0, frame_length=1024, hop_length=256)

    assert len(stretched) > 0
    assert len(shifted) > 0
    assert separated.length == len(samples)
    assert len(separated_residual["residual"]) == len(samples)
    assert len(normalized) == len(samples)
    assert len(trimmed) > 0


@pytest.mark.skipif(not _has_extended_effects(), reason="extended effects symbols unavailable")
def test_effects_reject_a_hop_below_the_half_window_overlap_contract() -> None:
    samples = np.sin(np.linspace(0.0, 8.0 * np.pi, 4096, dtype=np.float32)) * 0.25

    for call in (
        lambda: editing.hpss(samples, n_fft=1024, hop_length=1024),
        lambda: separation.hpss_with_residual(samples, n_fft=1024, hop_length=1024),
        lambda: editing.time_stretch(samples, rate=1.2, n_fft=512, hop_length=2048),
        lambda: editing.pitch_shift(samples, semitones=3.0, n_fft=1024, hop_length=1024),
        lambda: separation.phase_vocoder(samples, rate=1.2, n_fft=1024, hop_length=1024),
    ):
        with pytest.raises(SonareError) as error:
            call()
        assert error.value.code == ErrorCode.INVALID_PARAMETER


@pytest.mark.skipif(not _has_extended_effects(), reason="extended effects symbols unavailable")
def test_effects_accept_an_even_n_fft_that_is_not_a_power_of_two() -> None:
    # The core FFT is mixed-radix; the facade used to require a power of two,
    # which made the same call succeed on the C ABI and fail here.
    samples = np.sin(np.linspace(0.0, 8.0 * np.pi, 4096, dtype=np.float32)) * 0.25

    separated = editing.hpss(samples, n_fft=1500, hop_length=250)
    assert separated.length == len(samples)
    assert len(editing.time_stretch(samples, rate=1.2, n_fft=1500, hop_length=250)) > 0
    assert len(editing.pitch_shift(samples, semitones=3.0, n_fft=1500, hop_length=250)) > 0
    assert len(separation.phase_vocoder(samples, rate=1.2, n_fft=1500, hop_length=250)) > 0


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
    assert editing.hpss(samples, n_fft=1500, hop_length=250).length == len(samples)
    assert len(separation.phase_vocoder(samples, rate=1.2, n_fft=1500, hop_length=250)) > 0

    # Power-of-two family: rejected, by the facade rather than by the core, so
    # the message names the rule.
    with pytest.raises(ValueError, match="power of two"):
        editing.spectral_edit(samples, 22050, [], n_fft=1500, hop_length=250)
    with pytest.raises(ValueError, match="power of two"):
        mastering.mastering_repair_denoise_classical(samples, 22050, n_fft=1500, hop_length=250)
    with pytest.raises(ValueError, match="power of two"):
        mastering.mastering_repair_dereverb_classical(samples, 22050, n_fft=1500, hop_length=250)

    # The same three accept a power of two, so the rejection above is the rule
    # and not the entry point refusing everything.
    assert len(editing.spectral_edit(samples, 22050, [], n_fft=1024, hop_length=256)) == len(
        samples
    )
    assert (
        mastering.mastering_repair_denoise_classical(
            samples, 22050, n_fft=1024, hop_length=256
        ).shape
        == samples.shape
    )
    assert (
        mastering.mastering_repair_dereverb_classical(
            samples, 22050, n_fft=1024, hop_length=256
        ).shape
        == samples.shape
    )
