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

import numpy as np
import pytest

from libsonare import SonareError, SonareValueError, StreamingRetune

SR = 48000
BLOCK = 512


def _sine(count: int, hz: float, offset: int = 0) -> list[float]:
    return [math.sin(2.0 * math.pi * hz * (i + offset) / SR) for i in range(count)]


def test_streaming_retune_lifecycle() -> None:
    with StreamingRetune(pitch_semitones=12.0, mix=1.0, grain_size_samples=512) as retune:
        assert retune.grain_size_samples == 0
        assert retune.latency_samples == 0
        retune.prepare(SR, BLOCK)
        assert retune.grain_size_samples == 512
        assert retune.latency_samples == 512
        assert retune.config() == {"pitch_semitones": 12.0, "mix": 1.0, "grain_size_samples": 512}
        retune.reset()
        assert retune.grain_size_samples == 512


def test_streaming_retune_derives_grain_from_sample_rate() -> None:
    with StreamingRetune() as retune:
        retune.prepare(SR, BLOCK)
        assert retune.grain_size_samples == 2232


def test_streaming_retune_applies_grain_requested_after_prepare() -> None:
    with StreamingRetune() as retune:
        retune.prepare(SR, BLOCK)
        derived = retune.grain_size_samples
        assert retune.config()["grain_size_samples"] is None
        retune.set_config(grain_size_samples=512)
        # Structural: config() reports the configured grain at once, and the
        # grain in use follows at the next prepare.
        assert retune.config()["grain_size_samples"] == 512
        assert retune.grain_size_samples == derived
        retune.prepare(SR, BLOCK)
        assert retune.grain_size_samples == 512


def test_streaming_retune_config_round_trips_through_set_config() -> None:
    with StreamingRetune(pitch_semitones=3.0) as retune:
        retune.prepare(SR, BLOCK)
        before = retune.config()
        assert before["grain_size_samples"] is None
        retune.set_config(**before)  # type: ignore[arg-type]
        assert retune.config() == before
        with pytest.raises(
            SonareValueError,
            match="^StreamingRetune.set_config: grain_size_samples must not be 0; "
            "omit it for the automatic grain$",
        ):
            retune.set_config(grain_size_samples=0)


def test_streaming_retune_rederives_grain_after_an_unrelated_set_config() -> None:
    # set_config seeded every field from config(), and config()["grain_size_samples"] is
    # the EFFECTIVE grain once prepared -- so touching any other control froze
    # the 0 "derive from the sample rate" sentinel into a literal, and a host
    # re-preparing at 22.05 kHz kept the 48 kHz grain: double the latency and
    # double the transient smear, with no diagnostic saying why.
    with StreamingRetune() as retune:
        retune.prepare(SR, BLOCK)
        assert retune.grain_size_samples == 2232
        retune.set_config(pitch_semitones=5.0)
        retune.prepare(22050, BLOCK)
        assert retune.grain_size_samples == 1024
        # An explicitly requested grain still survives an unrelated update.
        retune.set_config(grain_size_samples=512)
        retune.set_config(mix=0.5)
        retune.prepare(22050, BLOCK)
        assert retune.grain_size_samples == 512


def test_streaming_retune_set_config_keeps_unmentioned_keywords() -> None:
    with StreamingRetune(pitch_semitones=5.0, mix=0.25) as retune:
        retune.prepare(SR, BLOCK)
        retune.set_config(mix=0.75)
        config = retune.config()
        assert config["pitch_semitones"] == pytest.approx(5.0)
        assert config["mix"] == pytest.approx(0.75)


def test_streaming_retune_shifts_pitch_without_mutating_the_input() -> None:
    with StreamingRetune(pitch_semitones=12.0, mix=1.0, grain_size_samples=512) as retune:
        retune.prepare(SR, BLOCK)
        out = np.zeros(0, dtype=np.float32)
        for pass_index in range(8):
            block = _sine(BLOCK, 220.0, pass_index * BLOCK)
            before = list(block)
            out = retune.process_mono(block)
            # The facade copies before the in-place C call.
            assert block == before
        assert isinstance(out, np.ndarray)
        assert out.dtype == np.float32
        # Past the one-grain latency the output carries energy; silence would
        # mean the block never reached the core.
        assert sum(value * value for value in out) > 1.0


def test_streaming_retune_reports_rejections() -> None:
    with pytest.raises(SonareError):
        StreamingRetune(pitch_semitones=float("nan"))
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
            retune.set_config(pitch_semitones=float("nan"))

        block = [0.0] * 64
        block[7] = float("nan")
        with pytest.raises(SonareError):
            retune.process_mono(block)


def test_streaming_retune_close_is_idempotent() -> None:
    retune = StreamingRetune()
    retune.close()
    retune.close()


def test_streaming_retune_refuses_an_explicit_zero_grain() -> None:
    with pytest.raises(SonareValueError, match="grain_size_samples"):
        StreamingRetune(grain_size_samples=0)
    with StreamingRetune() as retune, pytest.raises(SonareValueError, match="grain_size_samples"):
        retune.set_config(grain_size_samples=0)
