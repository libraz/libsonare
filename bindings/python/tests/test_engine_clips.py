"""Realtime engine audio input tests: planar input, audio clips, paged clip providers and warp."""

from __future__ import annotations

import gc
import math

import numpy as np
import pytest

from libsonare import (
    ClipPageProvider,
    EngineClip,
    EngineTelemetryError,
    EngineTelemetryType,
    FileClipPageProvider,
    RealtimeEngine,
    SonareError,
)


def _process_through_clip(channels: object, frames: int) -> list[list[float]]:
    """Render one block over a fixed clip, so the result is derived, not echoed back."""
    clip_plane = [0.5, 0.25, 0.125, 0.0625][:frames]
    with RealtimeEngine(sample_rate=48000.0, max_block_size=frames) as engine:
        engine.set_clips(
            [EngineClip(id=401, channels=[clip_plane], start_ppq=0.0, length_samples=frames)]
        )
        engine.play()
        return engine.process(channels)


def test_process_accepts_planar_channels_as_a_2d_ndarray() -> None:
    """Nested lists and a (channels, frames) array must render identically."""
    nested = [[0.1, 0.2, 0.3, 0.4], [-0.1, -0.2, -0.3, -0.4]]
    planar = np.array(nested, dtype=np.float32)

    from_lists = _process_through_clip(nested, 4)
    from_array = _process_through_clip(planar, 4)

    assert from_array == from_lists
    # The clip mixed into channel 0, so this compares a render, not the input.
    assert from_lists[0] != pytest.approx(nested[0])
    assert np.array_equal(planar, np.array(nested, dtype=np.float32))


@pytest.mark.parametrize("value", [0.0, 0.5])
def test_process_accepts_a_single_frame_plane_whatever_it_stores(value: float) -> None:
    """A (1, 1) array is one channel of one frame; the sample it holds is not a size."""
    from_list = _process_through_clip([[value]], 1)
    from_array = _process_through_clip(np.full((1, 1), value, dtype=np.float32), 1)

    assert from_array == from_list
    assert from_list == [[pytest.approx(value + 0.5)]]


def test_set_clips_accepts_planar_channels_as_a_2d_ndarray() -> None:
    """A clip's channels take the same planar spellings the process path does."""
    nested = [[0.5, 0.25, 0.125, 0.0625]]

    def rendered(channels: object) -> list[list[float]]:
        with RealtimeEngine(sample_rate=48000.0, max_block_size=4) as engine:
            engine.set_clips(
                [EngineClip(id=402, channels=channels, start_ppq=0.0, length_samples=4)]
            )
            engine.play()
            return engine.process([[0.0] * 4])

    assert rendered(np.array(nested, dtype=np.float32)) == rendered(nested)
    assert rendered(nested)[0] == pytest.approx(nested[0])


def test_clip_page_provider_supply_accepts_a_2d_ndarray_page() -> None:
    """A page supplied as a (channels, frames) array renders as a nested-list page does."""
    page = np.array([[1.0, 2.0, 3.0, 4.0]], dtype=np.float32)
    with (
        RealtimeEngine(sample_rate=48000.0, max_block_size=8) as engine,
        ClipPageProvider(1, 8, 4) as provider,
    ):
        provider.supply(0, page)
        provider.supply(1, page * 2.0)
        engine.set_clips([EngineClip(id=403, channels=None, start_ppq=0.0, page_provider=provider)])
        engine.play()
        assert engine.process([[0.0] * 8])[0] == [1.0, 2.0, 3.0, 4.0, 2.0, 4.0, 6.0, 8.0]


def test_engine_streams_paged_clip_provider_and_drains_requests() -> None:
    with (
        RealtimeEngine(sample_rate=48000.0, max_block_size=8) as engine,
        ClipPageProvider(1, 8, 4) as provider,
    ):
        provider.supply(0, [[1.0, 2.0, 3.0, 4.0]])
        engine.set_clips(
            [
                EngineClip(
                    id=123,
                    channels=None,
                    start_ppq=0.0,
                    page_provider=provider,
                )
            ]
        )
        engine.play()
        first = engine.process([[0.0] * 8])
        assert first[0] == [1.0, 2.0, 3.0, 4.0, 0.0, 0.0, 0.0, 0.0]

        request = engine.pop_clip_page_request()
        assert request is not None
        assert request.clip_id == 123
        assert request.channel == 0
        assert request.sample == 4
        assert any(
            record.type == EngineTelemetryType.ERROR
            and record.error == EngineTelemetryError.CLIP_PAGE_UNDERRUN
            and record.value == 123
            for record in engine.drain_telemetry()
        )

        provider.supply(1, [[5.0, 6.0, 7.0, 8.0]])
        engine.seek_sample(0)
        second = engine.process([[0.0] * 8])
        assert second[0] == [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]


def test_engine_feeds_paged_clips_from_raw_float32_files(tmp_path) -> None:
    raw_path = tmp_path / "clip.f32"
    np.asarray([1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0], dtype="<f4").tofile(raw_path)

    with (
        RealtimeEngine(sample_rate=48000.0, max_block_size=8) as engine,
        FileClipPageProvider(raw_path, num_channels=1, num_samples=8, page_frames=4) as provider,
    ):
        assert provider.supply_page(0) is True
        engine.set_clips(
            [
                EngineClip(
                    id=124,
                    channels=None,
                    start_ppq=0.0,
                    page_provider=provider,
                )
            ]
        )
        engine.play()
        first = engine.process([[0.0] * 8])
        assert first[0] == [1.0, 2.0, 3.0, 4.0, 0.0, 0.0, 0.0, 0.0]

        request = engine.pop_clip_page_request()
        assert request is not None
        assert request.clip_id == 124
        assert request.sample == 4
        assert provider.supply_request(request) is True
        engine.seek_sample(0)
        second = engine.process([[0.0] * 8])
        assert second[0] == [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0]


def test_clip_page_provider_failed_construction_does_not_emit_del_noise(capsys) -> None:
    with pytest.raises(SonareError):
        ClipPageProvider(0, 0, 0)
    with pytest.raises(SonareError):
        ClipPageProvider(1, 1_000_000_000_000, 1)
    with pytest.raises(SonareError):
        ClipPageProvider(65, 8, 4)
    gc.collect()
    assert "Exception ignored in" not in capsys.readouterr().err


def test_file_clip_page_provider_failed_open_cleans_up_without_del_noise(tmp_path, capsys) -> None:
    missing = tmp_path / "missing.f32"
    with pytest.raises(FileNotFoundError):
        FileClipPageProvider(missing, num_channels=1, num_samples=8, page_frames=4)
    gc.collect()
    assert "Exception ignored in" not in capsys.readouterr().err


def test_file_clip_page_provider_truncated_page_returns_false(tmp_path) -> None:
    raw_path = tmp_path / "truncated.f32"
    np.asarray([1.0, 2.0, 3.0, 4.0, 5.0], dtype="<f4").tofile(raw_path)

    with FileClipPageProvider(raw_path, num_channels=1, num_samples=8, page_frames=4) as provider:
        assert provider.supply_page(0) is True
        assert provider.supply_page(1) is False


def test_realtime_engine_renders_repitch_warped_clip() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=4) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=303,
                    channels=[[0.0, 10.0, 20.0, 30.0]],
                    start_ppq=0.0,
                    length_samples=4,
                    warp_mode="repitch",
                    warp_anchors=[(0.0, 0.0), (3.0, 1.5)],
                )
            ]
        )
        engine.play()
        processed = engine.process([[0.0] * 4])
        assert math.isclose(processed[0][0], 0.0, abs_tol=0.0001)
        assert math.isclose(processed[0][1], 5.0, abs_tol=0.0001)
        assert math.isclose(processed[0][2], 10.0, abs_tol=0.0001)
        assert math.isclose(processed[0][3], 15.0, abs_tol=0.0001)
    tempo_source = [math.sin(i * 0.02) for i in range(4096)]
    with RealtimeEngine(sample_rate=48000.0, max_block_size=8192) as tempo_engine:
        tempo_engine.set_clips(
            [
                EngineClip(
                    id=304,
                    channels=[tempo_source],
                    start_ppq=0.0,
                    length_samples=8192,
                    warp_mode="tempo-sync",
                    warp_anchors=[
                        (0.0, 0.0),
                        (2048.0, 1024.0),
                        (8192.0, 4096.0),
                    ],
                )
            ]
        )
        tempo_engine.play()
        tempo_synced = tempo_engine.process([[0.0] * 8192])
        assert max(abs(sample) for sample in tempo_synced[0]) > 0.1


def test_realtime_engine_renders_time_stretch_warped_clip_at_source_pitch() -> None:
    sr = 48000
    source_samples = 12000
    output_samples = 24000
    source = [0.5 * math.sin(2.0 * math.pi * 440.0 * i / sr) for i in range(source_samples)]
    with RealtimeEngine(sample_rate=float(sr), max_block_size=output_samples) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=307,
                    channels=[source],
                    start_ppq=0.0,
                    length_samples=output_samples,
                    warp_mode="time-stretch",
                    warp_anchors=[(0.0, 0.0), (float(output_samples), float(source_samples))],
                )
            ]
        )
        engine.play()
        out = engine.process([[0.0] * output_samples])[0]

    def power(hz: float) -> float:
        """Goertzel: a resampling warp at half rate would move the tone to 220 Hz."""
        w = 2.0 * math.pi * hz / sr
        coeff = 2.0 * math.cos(w)
        s1 = 0.0
        s2 = 0.0
        for i in range(4096, output_samples - 4096):
            s0 = out[i] + coeff * s1 - s2
            s2 = s1
            s1 = s0
        real = s1 - s2 * math.cos(w)
        imag = s2 * math.sin(w)
        return real * real + imag * imag

    assert power(440.0) > 100.0 * power(220.0)


def test_realtime_engine_warp_mode_validation_matches_project_helper() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=4) as engine:
        engine.set_clips(
            [
                EngineClip(
                    id=305,
                    channels=[[0.0, 10.0, 20.0, 30.0]],
                    start_ppq=0.0,
                    length_samples=4,
                    warp_mode="Repitch",
                    warp_anchors=[(0.0, 0.0), (3.0, 1.5)],
                )
            ]
        )
        engine.play()
        processed = engine.process([[0.0] * 4])
        assert math.isclose(processed[0][1], 5.0, abs_tol=0.0001)

    with RealtimeEngine(sample_rate=48000.0, max_block_size=4) as engine:
        for mode in ("typo", 1.9, True):
            with pytest.raises(ValueError, match="unknown warp mode"):
                engine.set_clips(
                    [
                        EngineClip(
                            id=306,
                            channels=[[0.0, 10.0, 20.0, 30.0]],
                            start_ppq=0.0,
                            length_samples=4,
                            warp_mode=mode,  # type: ignore[arg-type]
                        )
                    ]
                )


def test_realtime_engine_process_zero_copy_matches_list_input() -> None:
    """The zero-copy ``_channel_arrays`` marshalling preserves process() output.

    A numpy float32 input and the equivalent Python-list input must yield the
    same processed result, and caller input buffers must not be mutated in-place.
    """
    left = [math.sin(i * 0.013) for i in range(128)]
    right = [-sample for sample in left]
    list_channels = [left, right]
    list_channels_before = [channel.copy() for channel in list_channels]
    np_left = np.asarray(left, dtype=np.float32)
    np_right = np.asarray(right, dtype=np.float32)
    np_left_before = np_left.copy()
    clip = EngineClip(
        id=1,
        channels=[[0.5] * 128, [0.5] * 128],
        start_ppq=0.0,
        length_samples=128,
    )

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as list_engine:
        list_engine.set_clips([clip])
        list_engine.play()
        list_out = list_engine.process(list_channels)

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as np_engine:
        np_engine.set_clips([clip])
        np_engine.play()
        np_out = np_engine.process([np_left, np_right])

    assert np_out[0] == pytest.approx(list_out[0])
    assert np_out[1] == pytest.approx(list_out[1])
    assert list_out[0][0] == pytest.approx(0.5)
    assert list_out[1][0] == pytest.approx(0.5)
    # The input buffers are copied, never aliased: the engine writes its output
    # into private buffers and returns new processed channel lists.
    assert list_channels == list_channels_before
    assert list_out is not list_channels
    assert all(out is not channel for out, channel in zip(list_out, list_channels, strict=True))
    assert np.array_equal(np_left, np_left_before)


def test_engine_warp_voice_capacity_default_and_round_trip() -> None:
    """Default capacity is 8, set/get round-trips, and 0 disables stretch cleanly."""
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        assert engine.warp_voice_capacity() == 8

        engine.set_warp_voice_capacity(12)
        assert engine.warp_voice_capacity() == 12

        engine.set_warp_voice_capacity(0)
        assert engine.warp_voice_capacity() == 0
        engine.play()
        engine.process([[0.0] * 128, [0.0] * 128])
        assert engine.warp_stretch_overflow_count() == 0


@pytest.mark.parametrize("bad_voices", [65, -1, 1.5])
def test_engine_set_warp_voice_capacity_rejects_out_of_range_values(bad_voices: object) -> None:
    """65, -1, and 1.5 are all refused, leaving the stored capacity unchanged."""
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_warp_voice_capacity(12)
        with pytest.raises(ValueError):
            engine.set_warp_voice_capacity(bad_voices)  # type: ignore[arg-type]
        assert engine.warp_voice_capacity() == 12
