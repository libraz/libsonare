"""Python binding for the playback renderer (upmix, binaural, night mode, ...).

Mirrors the C ABI behaviour pinned in ``tests/api/sonare_c_playback_test.cpp``:
this file exercises only what the Python facade adds (config-as-JSON keyword
handling, numpy marshalling, context-manager lifetime), not the DSP itself.
"""

from __future__ import annotations

import math

import numpy as np
import pytest

from libsonare import (
    ErrorCode,
    HrtfSet,
    PlaybackLoudnessMeter,
    PlaybackRenderer,
    SonareError,
    SonareValueError,
    capabilities,
    render_playback,
)

from ._helpers import raises_coded

SR = 48000
BLOCK = 128


def _speakers_5_1() -> dict[str, object]:
    return {"target": {"kind": "speakers", "layout": "5.1"}}


def _sine(count: int, hz: float = 220.0) -> list[float]:
    return [0.1 * math.sin(2.0 * math.pi * hz * i / SR) for i in range(count)]


def test_capabilities_report_playback() -> None:
    assert capabilities()["features"]["playback"] is True


class _LibWithoutPlayback:
    """A stand-in for a ctypes.CDLL from a dylib built without BUILD_PLAYBACK.

    Not a real CDLL: hasattr() on the real thing performs a dlsym lookup and
    raises AttributeError for a genuinely absent symbol, which is exactly what
    this object reproduces without needing two actual dylib builds.
    """


def test_require_playback_names_the_missing_capability() -> None:
    """The internal guard raises a descriptive RuntimeError, not AttributeError.

    _ffi_signatures_playback.py's hasattr guards mean a playback-less but
    ABI-matched dylib loads cleanly; this is the other half -- an actual call
    into playback on that dylib must fail by naming what is missing, not with
    ctypes' own "object has no attribute" message.
    """
    from libsonare._playback import _require_playback

    with raises_coded(ErrorCode.NOT_SUPPORTED, match="playback"):
        _require_playback(_LibWithoutPlayback())


def test_require_playback_is_silent_when_the_symbol_is_present() -> None:
    from libsonare._playback import _require_playback
    from libsonare._runtime import _get_lib

    _require_playback(_get_lib())  # does not raise on this build, which has playback


def test_renderer_reports_its_shape() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        assert renderer.output_channels() == 6
        assert renderer.input_channels() == 2
        assert renderer.latency_samples() == 1312


def test_headphones_default_config_is_stereo_out() -> None:
    with PlaybackRenderer({}, max_block_size=BLOCK) as renderer:
        assert renderer.output_channels() == 2


def test_process_planar_renders_finite_output_of_the_right_shape() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        planes = [_sine(BLOCK), _sine(BLOCK, hz=440.0)]
        out = renderer.process_planar(planes)
        assert out.shape == (6, BLOCK)
        assert out.dtype == np.float32
        assert np.isfinite(out).all()


def test_process_planar_allocates_the_output_once() -> None:
    import tracemalloc

    frames = 16384
    with PlaybackRenderer(_speakers_5_1(), max_block_size=frames) as renderer:
        planes = np.zeros((2, frames), dtype=np.float32)
        renderer.process_planar(planes)
        tracemalloc.start()
        try:
            out = renderer.process_planar(planes)
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        assert out.shape == (6, frames)
        assert out.flags.c_contiguous and out.flags.owndata
        # One output-sized buffer plus marshalling, not a staging copy and a stack beside it.
        assert peak < 2 * out.nbytes


def test_process_planar_switches_auto_input_layout() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        assert renderer.input_channels() == 2
        planes = [_sine(BLOCK) for _ in range(6)]
        renderer.process_planar(planes)
        assert renderer.input_channels() == 6


def test_process_interleaved_renders_finite_output() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        samples = np.array(_sine(BLOCK * 2), dtype=np.float32)
        out = renderer.process_interleaved(samples, in_channels=2)
        assert out.shape == (BLOCK * 6,)
        assert out.dtype == np.float32
        assert np.isfinite(out).all()


def test_process_interleaved_rejects_indivisible_length() -> None:
    with (
        PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer,
        pytest.raises(SonareValueError),
    ):
        renderer.process_interleaved([0.0] * 7, in_channels=2)


def test_process_interleaved_rejects_nonpositive_channels() -> None:
    with (
        PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer,
        pytest.raises(SonareValueError),
    ):
        renderer.process_interleaved([0.0] * 8, in_channels=0)


def test_non_finite_input_is_discarded_and_counted() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        assert renderer.non_finite_discard_count() == 0
        samples = np.zeros(BLOCK * 2, dtype=np.float32)
        samples[3] = float("nan")
        out = renderer.process_interleaved(samples, in_channels=2)
        assert np.isfinite(out).all()
        assert renderer.non_finite_discard_count() == 1


def test_config_roundtrip_and_prepare_key_rejection() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        config = renderer.config()
        assert config["target"]["kind"] == "speakers"
        assert config["target"]["layout"] == "5.1"

        # A realtime-only key on a document with the same prepare shape is
        # accepted.
        config["night_mode"] = {"amount": 1}
        renderer.set_config(config)
        assert renderer.config()["night_mode"]["amount"] == 1

        # A document that changes the prepare shape (target.kind) is rejected.
        with pytest.raises(SonareError, match="requires a new renderer"):
            renderer.set_config({})


def test_set_config_accepts_a_json_string() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        renderer.set_config('{"target": {"kind": "speakers", "layout": "5.1"}}')


def test_set_head_orientation_validates_each_axis_and_float32_range() -> None:
    float32_max = 3.4028234663852886e38
    float32_overflow = float32_max * 2.0
    with PlaybackRenderer({}, max_block_size=BLOCK) as renderer:
        renderer.set_head_orientation(30.0, -10.0, 5.0)
        invalid_values = (
            float("nan"),
            float("inf"),
            float("-inf"),
            float32_overflow,
            -float32_overflow,
        )
        for value in invalid_values:
            with pytest.raises(SonareValueError, match="yaw_deg"):
                renderer.set_head_orientation(value, 0.0, 0.0)
            with pytest.raises(SonareValueError, match="pitch_deg"):
                renderer.set_head_orientation(0.0, value, 0.0)
            with pytest.raises(SonareValueError, match="roll_deg"):
                renderer.set_head_orientation(0.0, 0.0, value)

        for value in (float32_max, -float32_max, 0.0, -0.0, 720.0, -1080.0, 360.0):
            renderer.set_head_orientation(value, 0.0, 0.0)
            renderer.set_head_orientation(0.0, value, 0.0)
            renderer.set_head_orientation(0.0, 0.0, value)

        for value in (None, "wrong", True, object()):
            with pytest.raises(TypeError, match="yaw_deg"):
                renderer.set_head_orientation(value, 0.0, 0.0)  # type: ignore[arg-type]
            with pytest.raises(TypeError, match="pitch_deg"):
                renderer.set_head_orientation(0.0, value, 0.0)  # type: ignore[arg-type]
            with pytest.raises(TypeError, match="roll_deg"):
                renderer.set_head_orientation(0.0, 0.0, value)  # type: ignore[arg-type]
        with pytest.raises(SonareValueError, match="yaw_deg"):
            renderer.set_head_orientation(float("nan"), float("inf"), "roll")  # type: ignore[arg-type]
        with pytest.raises(SonareValueError, match="pitch_deg"):
            renderer.set_head_orientation(0.0, float("nan"), float("inf"))


def test_reset_clears_state() -> None:
    with PlaybackRenderer({}, max_block_size=BLOCK) as renderer:
        renderer.reset()


def test_diagnostics_reports_active_input_layout() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        diagnostics = renderer.diagnostics()
        assert "active_input_layout" in diagnostics


def test_create_json_rejects_an_unknown_key() -> None:
    with pytest.raises(SonareError):
        PlaybackRenderer({"not_a_key": 1})


def test_closed_renderer_rejects_further_calls() -> None:
    renderer = PlaybackRenderer({}, max_block_size=BLOCK)
    renderer.close()
    operations = {
        "reset": lambda: renderer.reset(),
        "set_config": lambda: renderer.set_config(None),  # type: ignore[arg-type]
        "config": lambda: renderer.config(),
        "set_head_orientation": lambda: renderer.set_head_orientation(float("nan")),
        "input_channels": lambda: renderer.input_channels(),
        "output_channels": lambda: renderer.output_channels(),
        "latency_samples": lambda: renderer.latency_samples(),
        "non_finite_discard_count": lambda: renderer.non_finite_discard_count(),
        "diagnostics": lambda: renderer.diagnostics(),
        "process_planar": lambda: renderer.process_planar(None),  # type: ignore[arg-type]
        "process_interleaved": lambda: renderer.process_interleaved(None, 0),  # type: ignore[arg-type]
        "render_offline": lambda: renderer.render_offline(None, 0),  # type: ignore[arg-type]
    }
    for name, operation in operations.items():
        with pytest.raises(SonareError) as raised:
            operation()
        assert raised.value.code == 7, name
    renderer.close()  # idempotent


def test_closed_loudness_meter_rejects_further_calls_before_buffer_validation() -> None:
    meter = PlaybackLoudnessMeter(2, SR)
    meter.close()
    operations = {
        "push_interleaved": lambda: meter.push_interleaved(None),  # type: ignore[arg-type]
        "integrated_lufs": lambda: meter.integrated_lufs(),
    }
    for name, operation in operations.items():
        with pytest.raises(SonareError) as raised:
            operation()
        assert raised.value.code == 7, name
    meter.close()


def test_render_playback_is_time_aligned_and_finite() -> None:
    samples = np.array(_sine(256 * 2), dtype=np.float32)
    out = render_playback(samples, channels=2, sample_rate=SR, config={})
    # (frames, channels) -- the shape itself names the output channel count,
    # which the caller cannot know in advance (see render_playback's own
    # docstring): a 2-channel speakers target here keeps 2 channels out.
    assert out.shape == (256, 2)
    assert np.isfinite(out).all()


def test_render_playback_names_the_output_channel_count_via_shape() -> None:
    """Headphones renders binaural stereo regardless of the source width."""
    samples = np.array(_sine(256 * 6), dtype=np.float32)
    out = render_playback(
        samples, channels=6, sample_rate=SR, config={"target": {"kind": "headphones"}}
    )
    assert out.shape == (256, 2)


def test_render_playback_rejects_an_unknown_key() -> None:
    with pytest.raises(SonareError):
        render_playback([0.0] * 4, channels=2, sample_rate=SR, config={"bad_key": 1})


def test_hrtf_set_default_and_lifecycle() -> None:
    with (
        HrtfSet.default() as hrtf,
        PlaybackRenderer({}, hrtf=hrtf, max_block_size=BLOCK) as renderer,
    ):
        assert renderer.output_channels() == 2


def test_hrtf_set_from_bytes_rejects_malformed_data() -> None:
    with pytest.raises(SonareError):
        HrtfSet.from_bytes(b"SHRF\x00\x00\x00\x00")


def test_a_closed_hrtf_set_is_refused_rather_than_silently_falling_back_to_the_default() -> None:
    """A closed handle must never reach the C ABI as an ordinary NULL.

    HrtfSet.close() leaves a NULL handle behind, and the C ABI's hrtf
    parameter treats NULL as an explicit "use the embedded default" -- the one
    handle in this binding where NULL is not simply refused. Passing a closed
    set into either construction path used to silently switch the render to
    the built-in SADIE II set instead of raising, with nothing to say a custom
    HRTF was ever asked for.
    """
    hrtf = HrtfSet.default()
    hrtf.close()

    with raises_coded(ErrorCode.INVALID_STATE, match="closed"):
        PlaybackRenderer({}, hrtf=hrtf, max_block_size=BLOCK)

    with raises_coded(ErrorCode.INVALID_STATE, match="closed"):
        render_playback(
            np.zeros(4, dtype=np.float32), channels=2, sample_rate=SR, config={}, hrtf=hrtf
        )


def test_loudness_meter_integrates_pushed_frames() -> None:
    with PlaybackLoudnessMeter(2, SR) as meter:
        block = _sine(4096 * 2, hz=200.0)
        meter.push_interleaved(block)
        lufs = meter.integrated_lufs()
        assert np.isfinite(lufs)
        assert lufs < 0.0


def test_loudness_meter_rejects_an_unsupported_channel_count() -> None:
    with pytest.raises(SonareError):
        PlaybackLoudnessMeter(3, SR)


def test_cli_render_trims_what_the_offline_render_trims_with_unequal_distances() -> None:
    """The CLI's block loop keeps each speaker's distance delay, like the offline render."""
    from libsonare._cli_playback import _OFFLINE_RENDER_BLOCK_FRAMES, _render_via_reporting_handle

    config = {
        "input": {"layout": "stereo"},
        "target": {
            "kind": "speakers",
            "layout": "stereo",
            "speakers": {"L": {"distance_m": 1.0}, "R": {"distance_m": 5.0}},
        },
        "output_limiter": {"enabled": False},
    }
    frames = 2400
    burst = np.zeros(frames, dtype=np.float32)
    burst[:480] = np.array(_sine(480, 1000.0), dtype=np.float32)
    interleaved = np.repeat(burst, 2)

    direct = render_playback(interleaved, channels=2, sample_rate=SR, config=config)
    with PlaybackRenderer(
        config, sample_rate=SR, max_block_size=_OFFLINE_RENDER_BLOCK_FRAMES
    ) as renderer:
        via_cli = _render_via_reporting_handle(renderer, interleaved, 2)

    np.testing.assert_array_equal(via_cli, direct)
    # Both planes keep the opening burst: the near L speaker is delayed, not cropped.
    energy = (direct.astype(np.float64) ** 2).sum(axis=0)
    expected = 480 * 0.1**2 / 2.0
    assert energy == pytest.approx([expected, expected], rel=1e-3)


_UNEQUAL_DISTANCE_CONFIG = {
    "input": {"layout": "stereo"},
    "target": {
        "kind": "speakers",
        "layout": "stereo",
        "speakers": {"L": {"distance_m": 1.0}, "R": {"distance_m": 5.0}},
    },
    "output_limiter": {"enabled": False},
}


def _offline_burst(frames: int = 2400) -> np.ndarray:
    burst = np.zeros(frames, dtype=np.float32)
    burst[:480] = np.array(_sine(480, 1000.0), dtype=np.float32)
    return np.repeat(burst, 2)


def test_render_offline_after_processing_equals_render_playback() -> None:
    interleaved = _offline_burst()
    direct = render_playback(
        interleaved, channels=2, sample_rate=SR, config=_UNEQUAL_DISTANCE_CONFIG
    )
    with PlaybackRenderer(_UNEQUAL_DISTANCE_CONFIG, sample_rate=SR, max_block_size=BLOCK) as r:
        r.process_interleaved(np.full(2 * BLOCK, 0.5, dtype=np.float32), 2)
        rendered = r.render_offline(interleaved, 2)
    assert rendered.dtype == np.float32
    np.testing.assert_array_equal(rendered.reshape(-1, 2), direct)


def test_render_offline_diagnostics_are_readable() -> None:
    with PlaybackRenderer(_UNEQUAL_DISTANCE_CONFIG, sample_rate=SR, max_block_size=BLOCK) as r:
        r.render_offline(_offline_burst(), 2)
        assert "latency" in r.diagnostics()


def test_render_offline_rejects_bad_input() -> None:
    good = _offline_burst(480)
    with PlaybackRenderer(_UNEQUAL_DISTANCE_CONFIG, sample_rate=SR, max_block_size=BLOCK) as r:
        # A layout mismatch is refused by the library, as render_playback refuses it.
        with pytest.raises(SonareValueError, match="does not match the input layout") as raised:
            r.render_offline(good, 3)
        assert raised.value.code == 4
        with pytest.raises(SonareValueError):
            r.render_offline(good, 0)
        with pytest.raises(SonareValueError):
            r.render_offline(np.array([0.0, float("nan")], dtype=np.float32), 2)
        with pytest.raises(SonareValueError):
            r.render_offline(np.empty(0, dtype=np.float32), 2)
