"""RealtimeVoiceChanger buffer semantics shared with the C and C++ contracts."""

from __future__ import annotations

import dataclasses
import math
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import ErrorCode, SonareError, SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SAMPLE_RATE = 48_000
FRAME_COUNT = 2_048
BLOCK_SIZE = 128

# Explicit profiles make additions to RealtimeVoiceChanger.process_* visible: a
# test below requires this registry to equal the public process_* method set.
REALTIME_VOICE_PROCESS_PROFILES: dict[str, tuple[str, bool]] = {
    "RealtimeVoiceChanger.process_interleaved": ("interleaved", False),
    "RealtimeVoiceChanger.process_interleaved_with_carrier": ("interleaved", True),
    "RealtimeVoiceChanger.process_mono": ("mono", False),
    "RealtimeVoiceChanger.process_mono_with_carrier": ("mono", True),
    "RealtimeVoiceChanger.process_planar_stereo": ("planar_stereo", False),
    "RealtimeVoiceChanger.process_planar_stereo_with_carrier": ("planar_stereo", True),
}
_CARRIER_PROFILES = tuple(
    name for name, (_, with_carrier) in REALTIME_VOICE_PROCESS_PROFILES.items() if with_carrier
)


def _tone(count: int = FRAME_COUNT, hz: float = 220.0) -> np.ndarray:
    return (0.2 * np.sin(2 * math.pi * hz * np.arange(count) / SAMPLE_RATE)).astype(np.float32)


def _neutral() -> libsonare.RealtimeVoiceChangerConfig:
    return libsonare.realtime_voice_changer_preset_config("neutral-monitor")


def _vocoder(source: str) -> libsonare.RealtimeVoiceChangerConfig:
    return dataclasses.replace(
        _neutral(), vocoder_enabled=1, vocoder_carrier_source=source, vocoder_level=1.0
    )


def _flat(config: libsonare.RealtimeVoiceChangerConfig) -> dict[str, object]:
    """Serialize the public config fields to the mapping accepted by the handle."""
    out: dict[str, object] = {}
    for field in dataclasses.fields(config):
        camel = "".join(
            part if index == 0 else part.capitalize()
            for index, part in enumerate(field.name.split("_"))
        )
        value = getattr(config, field.name)
        if field.name == "harmony_steps":
            for index, step in enumerate(value, start=1):
                out[f"harmonyStep{index}"] = step
        elif camel in ("limiterEnableIspLimiter", "vocoderEnabled"):
            out[camel] = bool(value)
        else:
            out[camel] = value
    return out


def _new_changer(
    config: libsonare.RealtimeVoiceChangerConfig, layout: str
) -> libsonare.RealtimeVoiceChanger:
    channels = 1 if layout == "mono" else 2
    return libsonare.RealtimeVoiceChanger(
        SAMPLE_RATE,
        _flat(config),
        max_block_size=BLOCK_SIZE,
        channels=channels,
    )


def _stereo_right(samples: np.ndarray) -> np.ndarray:
    return np.asarray(samples * np.float32(0.7), dtype=np.float32)


def _interleaved(samples: np.ndarray) -> np.ndarray:
    result = np.empty(samples.size * 2, dtype=np.float32)
    result[0::2] = samples
    result[1::2] = _stereo_right(samples)
    return result


def _process(
    changer: libsonare.RealtimeVoiceChanger,
    profile_name: str,
    samples: Any,
    carrier: Any | None = None,
    *,
    include_carrier: bool | None = None,
    raw_main: bool = False,
) -> np.ndarray | tuple[np.ndarray, np.ndarray]:
    layout, profile_uses_carrier = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    use_carrier = profile_uses_carrier if include_carrier is None else include_carrier
    audio = samples if raw_main or layout != "interleaved" else _interleaved(samples)
    if layout == "mono":
        args: list[object] = [audio]
    elif layout == "interleaved":
        args = [audio]
    else:
        right = np.zeros(FRAME_COUNT, dtype=np.float32) if raw_main else _stereo_right(samples)
        args = [audio, right]
    if use_carrier:
        args.append(carrier)

    method = f"process_{layout}" + ("_with_carrier" if use_carrier else "")
    return getattr(changer, method)(*args)


def _output_arrays(output: np.ndarray | tuple[np.ndarray, np.ndarray]) -> tuple[np.ndarray, ...]:
    return output if isinstance(output, tuple) else (output,)


def _assert_render(
    output: np.ndarray | tuple[np.ndarray, np.ndarray], layout: str, frames: int
) -> None:
    arrays = _output_arrays(output)
    expected_count = frames * 2 if layout == "interleaved" else frames
    expected_arrays = 2 if layout == "planar_stereo" else 1
    assert len(arrays) == expected_arrays
    for array in arrays:
        assert array.shape == (expected_count,)
        assert array.dtype == np.float32
        assert np.isfinite(array).all()


def _assert_external_config(changer: libsonare.RealtimeVoiceChanger) -> None:
    config = changer.config_pod()
    assert config.vocoder_enabled == 1
    assert config.vocoder_carrier_source == "external"


def test_realtime_process_profiles_match_public_methods() -> None:
    """New public process methods need an explicit layout and carrier profile."""
    discovered = {
        f"RealtimeVoiceChanger.{name}"
        for name in dir(libsonare.RealtimeVoiceChanger)
        if name.startswith("process_") and callable(getattr(libsonare.RealtimeVoiceChanger, name))
    }
    assert discovered == set(REALTIME_VOICE_PROCESS_PROFILES)


@pytest.mark.parametrize("profile_name", sorted(REALTIME_VOICE_PROCESS_PROFILES))
def test_zero_frame_process_is_a_no_op_and_preserves_state(profile_name: str) -> None:
    """All six calls return empty output without changing later rendering state."""
    layout, _ = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    config = _neutral()
    empty = np.zeros(0, dtype=np.float32)
    samples = _tone()
    carrier = _tone(hz=110.0)

    with _new_changer(config, layout) as after_empty:
        empty_output = _process(after_empty, profile_name, empty, empty)
        _assert_render(empty_output, layout, 0)
        assert after_empty.non_finite_discard_count() == 0
        actual = _process(after_empty, profile_name, samples, carrier)
        _assert_render(actual, layout, FRAME_COUNT)
        assert after_empty.non_finite_discard_count() == 0

    with _new_changer(config, layout) as untouched:
        expected = _process(untouched, profile_name, samples, carrier)
        _assert_render(expected, layout, FRAME_COUNT)
        assert untouched.non_finite_discard_count() == 0

    for actual_array, expected_array in zip(
        _output_arrays(actual), _output_arrays(expected), strict=True
    ):
        np.testing.assert_array_equal(actual_array, expected_array)


@pytest.mark.parametrize("profile_name", sorted(REALTIME_VOICE_PROCESS_PROFILES))
@pytest.mark.parametrize(
    "bad_value", [pytest.param(np.nan, id="nan"), pytest.param(np.inf, id="inf")]
)
def test_non_finite_main_audio_is_sanitized_and_reported(
    profile_name: str, bad_value: float
) -> None:
    """Finite controls stay clean; non-finite main audio yields finite output and telemetry."""
    layout, _ = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    config = _neutral()
    samples = _tone()
    carrier = _tone(hz=110.0)
    bad_samples = samples.copy()
    bad_samples[128] = bad_value

    with _new_changer(config, layout) as clean:
        clean_output = _process(clean, profile_name, samples, carrier)
        _assert_render(clean_output, layout, FRAME_COUNT)
        assert clean.non_finite_discard_count() == 0

    with _new_changer(config, layout) as degraded:
        before = degraded.non_finite_discard_count()
        output = _process(degraded, profile_name, bad_samples, carrier)
        _assert_render(output, layout, FRAME_COUNT)
        assert degraded.non_finite_discard_count() > before


@pytest.mark.parametrize("profile_name", _CARRIER_PROFILES)
@pytest.mark.parametrize("mode", ["disabled", "internal"])
def test_unused_non_finite_carrier_is_ignored(profile_name: str, mode: str) -> None:
    """A non-finite carrier is ignored unless the external carrier source is active."""
    layout, _ = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    config = _neutral() if mode == "disabled" else _vocoder("internal")
    samples = _tone()
    carrier = np.full(FRAME_COUNT, np.nan, dtype=np.float32)

    with _new_changer(config, layout) as with_unused_carrier:
        actual = _process(with_unused_carrier, profile_name, samples, carrier)
        _assert_render(actual, layout, FRAME_COUNT)
        assert any(np.any(array != 0) for array in _output_arrays(actual))
        assert with_unused_carrier.non_finite_discard_count() == 0

    with _new_changer(config, layout) as without_carrier:
        expected = _process(without_carrier, profile_name, samples, carrier, include_carrier=False)
        _assert_render(expected, layout, FRAME_COUNT)
        assert any(np.any(array != 0) for array in _output_arrays(expected))
        assert without_carrier.non_finite_discard_count() == 0

    for actual_array, expected_array in zip(
        _output_arrays(actual), _output_arrays(expected), strict=True
    ):
        np.testing.assert_array_equal(actual_array, expected_array)


@pytest.mark.parametrize("profile_name", _CARRIER_PROFILES)
def test_external_carrier_changes_the_render(profile_name: str) -> None:
    """The configured external carrier affects nonzero output in every layout."""
    layout, _ = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    config = _vocoder("external")
    samples = _tone()
    outputs = []
    for frequency in (110.0, 440.0):
        with _new_changer(config, layout) as changer:
            _assert_external_config(changer)
            output = _process(changer, profile_name, samples, _tone(hz=frequency))
            _assert_render(output, layout, FRAME_COUNT)
            assert any(np.any(array != 0) for array in _output_arrays(output))
            assert changer.non_finite_discard_count() == 0
            outputs.append(_output_arrays(output))

    assert any(
        not np.array_equal(first, second)
        for first, second in zip(outputs[0], outputs[1], strict=True)
    )


@pytest.mark.parametrize("profile_name", _CARRIER_PROFILES)
def test_external_mode_requires_a_carrier_on_carrierless_calls(profile_name: str) -> None:
    """The live external config makes the corresponding carrierless API invalid state."""
    layout, _ = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    with _new_changer(_vocoder("external"), layout) as changer:
        _assert_external_config(changer)
        with pytest.raises(SonareError) as excinfo:
            _process(
                changer,
                profile_name,
                _tone(),
                _tone(hz=110.0),
                include_carrier=False,
            )
        assert not isinstance(excinfo.value, SonareValueError)
        assert excinfo.value.code == int(ErrorCode.INVALID_STATE)


@pytest.mark.parametrize("profile_name", _CARRIER_PROFILES)
def test_external_nan_carrier_is_sanitized_and_recovery_is_finite(profile_name: str) -> None:
    """An external NaN carrier follows realtime sanitization and recovery semantics."""
    layout, _ = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    samples = _tone()
    carrier = _tone(hz=110.0)
    carrier[0] = np.nan
    clean_carrier = _tone(hz=110.0)

    with _new_changer(_vocoder("external"), layout) as changer:
        _assert_external_config(changer)
        before = changer.non_finite_discard_count()
        degraded = _process(changer, profile_name, samples, carrier)
        _assert_render(degraded, layout, FRAME_COUNT)
        after_degraded = changer.non_finite_discard_count()
        assert after_degraded > before

        recovered = _process(changer, profile_name, samples, clean_carrier)
        _assert_render(recovered, layout, FRAME_COUNT)
        assert changer.non_finite_discard_count() == after_degraded


@pytest.mark.parametrize("profile_name", sorted(REALTIME_VOICE_PROCESS_PROFILES))
@pytest.mark.parametrize(
    "bad_main",
    [
        pytest.param(np.zeros((FRAME_COUNT, 1), dtype=np.float32), id="rank-2"),
        pytest.param(["bad"] * FRAME_COUNT, id="non-numeric"),
    ],
)
def test_every_process_method_keeps_main_buffer_type_and_rank_checks(
    profile_name: str, bad_main: Any
) -> None:
    """The realtime value policy still rejects inputs it cannot marshal as 1-D audio."""
    layout, _ = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    carrier = _tone(hz=110.0)
    parameter_name = "left" if layout == "planar_stereo" else "samples"

    with _new_changer(_neutral(), layout) as changer:
        with pytest.raises(SonareValueError) as excinfo:
            _process(changer, profile_name, bad_main, carrier, raw_main=True)
        assert parameter_name in str(excinfo.value), str(excinfo.value)


@pytest.mark.parametrize("profile_name", _CARRIER_PROFILES)
@pytest.mark.parametrize(
    "bad_carrier",
    [
        pytest.param(np.zeros(FRAME_COUNT - 1, dtype=np.float32), id="wrong-length"),
        pytest.param(np.zeros((FRAME_COUNT, 1), dtype=np.float32), id="rank-2"),
        pytest.param(["bad"] * FRAME_COUNT, id="non-numeric"),
    ],
)
def test_external_carrier_keeps_shape_and_type_checks(profile_name: str, bad_carrier: Any) -> None:
    """Length, rank and type refusals identify the active external carrier argument."""
    layout, _ = REALTIME_VOICE_PROCESS_PROFILES[profile_name]
    with _new_changer(_vocoder("external"), layout) as changer:
        _assert_external_config(changer)
        with pytest.raises(SonareValueError) as excinfo:
            _process(changer, profile_name, _tone(), bad_carrier)
        assert "carrier" in str(excinfo.value), str(excinfo.value)
