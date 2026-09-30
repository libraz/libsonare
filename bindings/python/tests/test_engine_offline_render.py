"""Realtime engine offline render tests: bounce defaults and chunked rendering."""

from __future__ import annotations

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
    RealtimeEngine,
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
