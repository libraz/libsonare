"""Realtime engine offline render tests: bounce defaults and chunked rendering."""

from __future__ import annotations

import json
import math

import numpy as np
import pytest

from libsonare import (
    BuiltinSynthConfig,
    EngineBounceOptions,
    EngineClip,
    EngineFreezeOptions,
    EngineMidiClipSchedule,
    EngineMidiEvent,
    ErrorCode,
    RealtimeEngine,
    SonareError,
)

from ._helpers import _midi1_word


def test_bounce_options_default_seeded_from_native_layer() -> None:
    """Leaving ``target_lufs`` at its sentinel still tracks the C default.

    The C ``sonare_engine_bounce_options_default`` maps ``target_lufs == 0.0``
    to the -14 LUFS default; the Python wrapper seeds the raw options from it,
    so a bounce request runs without raising regardless of the dataclass copy.
    """
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.play()
        bounced = engine.bounce_offline(
            EngineBounceOptions(
                total_frames=128,
                block_size=128,
                num_channels=2,
                source_sample_rate=48000,
                target_sample_rate=48000,
                normalize_lufs=True,
            )
        )
    assert bounced.num_channels == 2
    assert bounced.frames == 128
    assert bounced.sample_rate == 48000


def test_realtime_engine_offline_render_matches_process() -> None:
    frames = 256
    left = [math.sin(i * 0.01) for i in range(frames)]
    right = [-sample for sample in left]

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as realtime:
        realtime.play()
        rt_left: list[float] = []
        rt_right: list[float] = []
        for offset in range(0, frames, 128):
            block = realtime.process([left[offset : offset + 128], right[offset : offset + 128]])
            rt_left.extend(block[0])
            rt_right.extend(block[1])

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as offline:
        offline.play()
        rendered = offline.render_offline([left, right], block_size=128)

    assert rendered[0] == rt_left
    assert rendered[1] == rt_right

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as bounce_engine:
        bounce_engine.play()
        bounced = bounce_engine.bounce_offline(
            EngineBounceOptions(
                total_frames=256,
                block_size=128,
                num_channels=2,
                source_sample_rate=48000,
                target_sample_rate=24000,
            )
        )

    assert bounced.frames == 128
    assert bounced.num_channels == 2
    assert bounced.sample_rate == 24000
    assert len(bounced.interleaved) == 256
    # Integrated LUFS is finite for audible material, or -inf (the LUFS floor)
    # for signals below the gating threshold (e.g. this very short bounce).
    assert math.isfinite(bounced.integrated_lufs) or bounced.integrated_lufs == float("-inf")

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as freeze_engine:
        freeze_engine.set_clips(
            [
                EngineClip(
                    id=7,
                    channels=[[0.125] * 128, [-0.25] * 128],
                    start_ppq=0.0,
                    length_samples=128,
                )
            ]
        )
        freeze_engine.play()
        frozen = freeze_engine.freeze_offline(
            EngineFreezeOptions(total_frames=128, block_size=128, num_channels=2, clip_id=77)
        )
        assert frozen.clip_id == 77
        assert frozen.frames == 128
        assert frozen.num_channels == 2
        assert freeze_engine.clip_count() == 1
        freeze_engine.seek_sample(0)
        frozen_render = freeze_engine.render_offline([[0.0] * 128, [0.0] * 128], block_size=128)

    assert frozen_render[0][0] == pytest.approx(0.125, abs=0.0001)
    assert frozen_render[1][0] == pytest.approx(-0.25, abs=0.0001)


def _pad_engine() -> RealtimeEngine:
    """An engine holding a pad that outlives any render span.

    Note-on at frame 0 and no note-off, so whether it is still sounding at the
    end of a chunk is exactly the state a non-finalizing render must preserve.
    """
    engine = RealtimeEngine(sample_rate=48000.0, max_block_size=128)
    engine.set_builtin_instrument(BuiltinSynthConfig(gain=0.5), 6)
    engine.set_midi_clips(
        [
            EngineMidiClipSchedule(
                id=1,
                track_id=6,
                destination_id=6,
                length_samples=1 << 20,
                events=[
                    EngineMidiEvent(0, word0=_midi1_word(0x9, 0, 60, 100), word_count=1),
                ],
            )
        ]
    )
    engine.play()
    return engine


def test_render_offline_chunks_concatenate_to_one_continuous_render() -> None:
    chunk = 4096
    chunks = 3
    total = chunk * chunks

    with _pad_engine() as continuous_engine:
        continuous = np.asarray(
            continuous_engine.render_offline([[0.0] * total, [0.0] * total], block_size=128)[0],
            dtype=np.float32,
        )
    # Non-vacuity: comparing two silent renders would prove nothing.
    assert float(np.max(np.abs(continuous))) > 0.0

    def render_chunks(*, finalize: bool) -> np.ndarray:
        joined: list[float] = []
        with _pad_engine() as engine:
            for _ in range(chunks):
                rendered = engine.render_offline(
                    [[0.0] * chunk, [0.0] * chunk], block_size=128, finalize=finalize
                )
                joined.extend(rendered[0])
            engine.finish_offline_render()
        return np.asarray(joined, dtype=np.float32)

    assert float(np.max(np.abs(render_chunks(finalize=False) - continuous))) < 1e-6

    # Non-vacuity: finalizing every chunk is the defect the flag exists to fix.
    # The pad's note-off fires at the end of chunk 1 and no note-on is re-sent,
    # so the tail decays away instead of holding and the join diverges.
    finalized = render_chunks(finalize=True)
    assert float(np.max(np.abs(finalized - continuous))) > 1e-3
    tail = slice(total - chunk, total)
    assert float(np.sqrt(np.mean(finalized[tail] ** 2))) < 0.5 * float(
        np.sqrt(np.mean(continuous[tail] ** 2))
    )


_RESET_BLOCK = 128
_RESET_FRAMES = 9600
_PLATE_STRIP = json.dumps(
    {
        "version": 1,
        "strips": [
            {
                "id": "s",
                "inserts": [
                    {
                        "slot": "pre",
                        "processor": "effects.reverb.plate",
                        "params": json.dumps({"decaySec": 2.0, "dryWet": 0.5}),
                    }
                ],
            }
        ],
    }
)


def _plate_engine() -> RealtimeEngine:
    """One lane of tone bursts with silent gaps through a plate reverb."""
    burst = [
        0.4 * math.sin(2.0 * math.pi * 330.0 * i / 48000.0) if i % 2400 < 1200 else 0.0
        for i in range(_RESET_FRAMES)
    ]
    engine = RealtimeEngine(sample_rate=48000.0, max_block_size=_RESET_BLOCK)
    engine.set_clips(
        [
            EngineClip(
                id=1,
                track_id=10,
                channels=[burst, burst],
                start_ppq=0.0,
                length_samples=_RESET_FRAMES,
            )
        ]
    )
    engine.set_track_lanes([10])
    engine.set_track_strip_json(10, _PLATE_STRIP)
    return engine


def _play_from(engine: RealtimeEngine, start: int, blocks: int, *, reset: bool) -> np.ndarray:
    engine.seek_sample(start)
    if reset:
        engine.reset_processor_state()
    engine.play()
    silence = [[0.0] * _RESET_BLOCK, [0.0] * _RESET_BLOCK]
    rendered: list[float] = []
    for _ in range(blocks):
        rendered.extend(engine.process(silence)[0])
    return np.asarray(rendered, dtype=np.float32)


def test_reset_processor_state_matches_a_fresh_engine_after_idle_advance() -> None:
    blocks = 24
    with _plate_engine() as fresh:
        expected = _play_from(fresh, 1200, blocks, reset=False)
    assert float(np.max(np.abs(expected))) > 0.0

    with _plate_engine() as engine:
        # Ring the plate, stop, then let the idle engine run on.
        engine.play()
        for _ in range(30):
            engine.process([[0.0] * _RESET_BLOCK, [0.0] * _RESET_BLOCK])
        engine.stop()
        for _ in range(3):
            engine.process([[0.0] * _RESET_BLOCK, [0.0] * _RESET_BLOCK])
        reset = _play_from(engine, 1200, blocks, reset=True)
    tolerance = 1e-6 * max(1.0, float(np.max(np.abs(expected))))
    assert float(np.max(np.abs(reset - expected))) <= tolerance

    # Non-vacuity: without the reset the ringing tail leaks into the render.
    with _plate_engine() as engine:
        engine.play()
        for _ in range(30):
            engine.process([[0.0] * _RESET_BLOCK, [0.0] * _RESET_BLOCK])
        engine.stop()
        for _ in range(3):
            engine.process([[0.0] * _RESET_BLOCK, [0.0] * _RESET_BLOCK])
        leaked = _play_from(engine, 1200, blocks, reset=False)
    assert float(np.max(np.abs(leaked - expected))) > tolerance


def test_prime_offline_parameters_accepts_the_prepared_shape_and_refuses_others() -> None:
    with _plate_engine() as engine:
        engine.prime_offline_parameters(2, _RESET_BLOCK)
        for channels, block in ((0, _RESET_BLOCK), (2, 0), (-1, 1), (4096, _RESET_BLOCK)):
            with pytest.raises(SonareError):
                engine.prime_offline_parameters(channels, block)


def test_offline_entry_points_refuse_a_block_above_the_prepared_block() -> None:
    frames = 256
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        with pytest.raises(SonareError) as render_error:
            engine.render_offline([[0.0] * frames, [0.0] * frames], block_size=129)
        assert render_error.value.code == ErrorCode.INVALID_PARAMETER
        with pytest.raises(SonareError):
            engine.prime_offline_parameters(2, 129)
        with pytest.raises(SonareError):
            engine.bounce_offline(EngineBounceOptions(total_frames=frames, block_size=129))
        with pytest.raises(SonareError):
            engine.freeze_offline(EngineFreezeOptions(total_frames=frames, block_size=129))
        # The prepared block and anything smaller still render.
        engine.render_offline([[0.0] * frames, [0.0] * frames], block_size=128)
        engine.render_offline([[0.0] * frames, [0.0] * frames], block_size=64)


def test_tail_and_latency_follow_the_configured_strips() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=_RESET_BLOCK) as engine:
        assert engine.tail_samples() == 0
        assert engine.graph_latency_samples_q8() == 0
        engine.set_track_lanes([10])
        # A 1 ms limiter lookahead is 48 samples at 48 kHz: 48 * 256 in q8.
        engine.set_track_strip_json(
            10,
            json.dumps(
                {
                    "version": 1,
                    "strips": [
                        {
                            "id": "s",
                            "inserts": [
                                {
                                    "slot": "pre",
                                    "processor": "dynamics.limiter",
                                    "params": json.dumps(
                                        {"thresholdDb": 24.0, "releaseMs": 50.0, "lookaheadMs": 1.0}
                                    ),
                                }
                            ],
                        }
                    ],
                }
            ),
        )
        assert engine.graph_latency_samples_q8() == 48 * 256


def test_tail_samples_reports_a_lane_channel_delay() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=_RESET_BLOCK) as engine:
        engine.set_track_lanes([10])
        engine.set_track_strip_json(
            10,
            json.dumps(
                {
                    "version": 1,
                    "strips": [{"id": "s", "inserts": []}],
                    "buses": [],
                    "connections": [],
                }
            ),
        )
        # An insert-free strip adds nothing, so the tail is the channel delay alone.
        assert engine.tail_samples() == 0
        engine.set_track_strip_channel_delay_samples(10, 300)
        assert engine.tail_samples() == 300
