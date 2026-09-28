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
    HrtfSet,
    PlaybackLoudnessMeter,
    PlaybackRenderer,
    SonareError,
    SonareValueError,
    capabilities,
    render_playback,
)

SR = 48000
BLOCK = 128


def _speakers_5_1() -> dict[str, object]:
    return {"target": {"kind": "speakers", "layout": "5.1"}}


def _sine(count: int, hz: float = 220.0) -> list[float]:
    return [0.1 * math.sin(2.0 * math.pi * hz * i / SR) for i in range(count)]


def test_capabilities_report_playback() -> None:
    assert capabilities()["features"]["playback"] is True


def test_renderer_reports_its_shape() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        assert renderer.output_channels == 6
        assert renderer.input_channels == 2
        assert renderer.latency_samples == 1312


def test_headphones_default_config_is_stereo_out() -> None:
    with PlaybackRenderer({}, max_block_size=BLOCK) as renderer:
        assert renderer.output_channels == 2


def test_process_planar_renders_finite_output_of_the_right_shape() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        planes = [_sine(BLOCK), _sine(BLOCK, hz=440.0)]
        out = renderer.process_planar(planes)
        assert out.shape == (6, BLOCK)
        assert out.dtype == np.float32
        assert np.isfinite(out).all()


def test_process_planar_switches_auto_input_layout() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        assert renderer.input_channels == 2
        planes = [_sine(BLOCK) for _ in range(6)]
        renderer.process_planar(planes)
        assert renderer.input_channels == 6


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
        assert renderer.non_finite_discard_count == 0
        samples = np.zeros(BLOCK * 2, dtype=np.float32)
        samples[3] = float("nan")
        out = renderer.process_interleaved(samples, in_channels=2)
        assert np.isfinite(out).all()
        assert renderer.non_finite_discard_count == 1


def test_config_roundtrip_and_prepare_key_rejection() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        config = renderer.config
        assert config["target"]["kind"] == "speakers"
        assert config["target"]["layout"] == "5.1"

        # A realtime-only key on a document with the same prepare shape is
        # accepted.
        config["night_mode"] = {"amount": 1}
        renderer.set_config(config)
        assert renderer.config["night_mode"]["amount"] == 1

        # A document that changes the prepare shape (target.kind) is rejected.
        with pytest.raises(SonareError, match="requires a new renderer"):
            renderer.set_config({})


def test_set_config_accepts_a_json_string() -> None:
    with PlaybackRenderer(_speakers_5_1(), max_block_size=BLOCK) as renderer:
        renderer.set_config('{"target": {"kind": "speakers", "layout": "5.1"}}')


def test_set_head_orientation_is_accepted_including_non_finite() -> None:
    with PlaybackRenderer({}, max_block_size=BLOCK) as renderer:
        renderer.set_head_orientation(30.0, -10.0, 5.0)
        renderer.set_head_orientation(float("nan"))


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
    with pytest.raises(SonareError):
        renderer.reset()
    renderer.close()  # idempotent


def test_render_playback_is_time_aligned_and_finite() -> None:
    samples = np.array(_sine(256 * 2), dtype=np.float32)
    out = render_playback(samples, channels=2, sample_rate=SR, config={})
    assert out.shape == (256 * 2,)
    assert np.isfinite(out).all()


def test_render_playback_rejects_an_unknown_key() -> None:
    with pytest.raises(SonareError):
        render_playback([0.0] * 4, channels=2, sample_rate=SR, config={"bad_key": 1})


def test_hrtf_set_default_and_lifecycle() -> None:
    with (
        HrtfSet.default() as hrtf,
        PlaybackRenderer({}, hrtf=hrtf, max_block_size=BLOCK) as renderer,
    ):
        assert renderer.output_channels == 2


def test_hrtf_set_from_bytes_rejects_malformed_data() -> None:
    with pytest.raises(SonareError):
        HrtfSet.from_bytes(b"SHRF\x00\x00\x00\x00")


def test_loudness_meter_integrates_pushed_frames() -> None:
    with PlaybackLoudnessMeter(2, SR) as meter:
        block = _sine(4096 * 2, hz=200.0)
        meter.push_interleaved(block)
        lufs = meter.integrated_lufs
        assert np.isfinite(lufs)
        assert lufs < 0.0


def test_loudness_meter_rejects_an_unsupported_channel_count() -> None:
    with pytest.raises(SonareError):
        PlaybackLoudnessMeter(3, SR)
