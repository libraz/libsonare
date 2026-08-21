"""StreamingRetune: the Python half of the C-ABI mirror.

The class reached only WASM before -- no C ABI, no Node, no Python -- while its
siblings in the same module family (StreamingMasteringChain, StreamingEqualizer)
were mirrored to all three surfaces. These pin this surface and the rejection
policy the four now share: a non-finite control or sample is refused rather than
substituted or zeroed, because either would enter the grain history and persist
into every later block with nothing to tell the caller its input was altered.
"""

from __future__ import annotations

import math

import pytest

from libsonare import SonareError, StreamingRetune

SR = 48000
BLOCK = 512


def _sine(count: int, hz: float, offset: int = 0) -> list[float]:
    return [math.sin(2.0 * math.pi * hz * (i + offset) / SR) for i in range(count)]


def test_streaming_retune_lifecycle() -> None:
    with StreamingRetune(semitones=12.0, mix=1.0, grain_size=512) as retune:
        assert retune.grain_size() == 0
        assert retune.latency_samples() == 0
        retune.prepare(SR, BLOCK)
        assert retune.grain_size() == 512
        assert retune.latency_samples() == 512
        assert retune.config() == {"semitones": 12.0, "mix": 1.0, "grain_size": 512}
        retune.reset()
        assert retune.grain_size() == 512


def test_streaming_retune_derives_grain_from_sample_rate() -> None:
    with StreamingRetune() as retune:
        retune.prepare(SR, BLOCK)
        assert retune.grain_size() == 2232


def test_streaming_retune_applies_grain_requested_after_prepare() -> None:
    with StreamingRetune() as retune:
        retune.prepare(SR, BLOCK)
        derived = retune.grain_size()
        retune.set_config(grain_size=512)
        # Structural: the request takes effect at the next prepare, and config()
        # keeps reporting the effective grain until then.
        assert retune.grain_size() == derived
        assert retune.config()["grain_size"] == derived
        retune.prepare(SR, BLOCK)
        assert retune.grain_size() == 512


def test_streaming_retune_set_config_keeps_unmentioned_keywords() -> None:
    with StreamingRetune(semitones=5.0, mix=0.25) as retune:
        retune.prepare(SR, BLOCK)
        retune.set_config(mix=0.75)
        config = retune.config()
        assert config["semitones"] == pytest.approx(5.0)
        assert config["mix"] == pytest.approx(0.75)


def test_streaming_retune_shifts_pitch_without_mutating_the_input() -> None:
    with StreamingRetune(semitones=12.0, mix=1.0, grain_size=512) as retune:
        retune.prepare(SR, BLOCK)
        out: list[float] = []
        for pass_index in range(8):
            block = _sine(BLOCK, 220.0, pass_index * BLOCK)
            before = list(block)
            out = retune.process_mono(block)
            # The facade copies before the in-place C call.
            assert block == before
        # Past the one-grain latency the output carries energy; silence would
        # mean the block never reached the core.
        assert sum(value * value for value in out) > 1.0


def test_streaming_retune_reports_rejections() -> None:
    with pytest.raises(SonareError):
        StreamingRetune(semitones=float("nan"))
    with pytest.raises(SonareError):
        StreamingRetune(mix=float("inf"))

    with StreamingRetune() as retune:
        # Before prepare the core answers a violated precondition with a silent
        # no-op to stay audio-thread callable, so without the C ABI guard this
        # would look like a successful render of silence.
        with pytest.raises(SonareError):
            retune.process_mono([0.0] * 64)

        retune.prepare(SR, 64)
        with pytest.raises(SonareError):
            retune.process_mono([0.0] * 128)
        with pytest.raises(SonareError):
            retune.set_config(semitones=float("nan"))

        block = [0.0] * 64
        block[7] = float("nan")
        with pytest.raises(SonareError):
            retune.process_mono(block)


def test_streaming_retune_close_is_idempotent() -> None:
    retune = StreamingRetune()
    retune.close()
    retune.close()
