"""Bounce content, loudness-target and dither behaviour of the offline export.

The shape of a bounce result (frame count, channel count, sample rate, length)
says nothing about whether the scheduled material reached the export, whether a
requested loudness target was applied, or whether the requested dither ran.
These cases assert the audible content instead.
"""

from __future__ import annotations

import ctypes
import math

import pytest

from libsonare import EngineBounceOptions, EngineClip, RealtimeEngine, SonareError

SAMPLE_RATE = 48000
BLOCK_SIZE = 128
FRAMES = 256
AMPLITUDE = 0.5
# Depth at which dither is unmistakable in a float32 buffer: one LSB is 1/128,
# far above the float resolution of a 0.5-amplitude sample.
DITHER_BITS = 8
LSB = 1.0 / (1 << (DITHER_BITS - 1))


def _tone() -> list[float]:
    return [AMPLITUDE * math.sin(2.0 * math.pi * 440.0 * i / SAMPLE_RATE) for i in range(FRAMES)]


def _bounce(**overrides: object) -> tuple[list[float], float]:
    tone = _tone()
    clip = EngineClip(id=1, channels=[tone, list(tone)], start_ppq=0.0, length_samples=FRAMES)
    with RealtimeEngine(sample_rate=SAMPLE_RATE, max_block_size=BLOCK_SIZE) as engine:
        engine.set_clips([clip])
        engine.play()
        result = engine.bounce_offline(
            EngineBounceOptions(
                total_frames=FRAMES,
                block_size=BLOCK_SIZE,
                num_channels=2,
                source_sample_rate=SAMPLE_RATE,
                target_sample_rate=SAMPLE_RATE,
                **overrides,  # type: ignore[arg-type]
            )
        )
    return list(result.interleaved), result.integrated_lufs


def _peak(samples: list[float]) -> float:
    return max(abs(sample) for sample in samples)


def _max_abs_diff(a: list[float], b: list[float]) -> float:
    return max(abs(x - y) for x, y in zip(a, b, strict=True))


def _on_grid(samples: list[float]) -> int:
    return sum(1 for s in samples if abs(s / LSB - round(s / LSB)) < 1e-4)


def test_bounce_defaults_both_rates_to_the_prepared_rate() -> None:
    with RealtimeEngine(sample_rate=44100, max_block_size=BLOCK_SIZE) as engine:
        result = engine.bounce_offline(EngineBounceOptions(total_frames=441, num_channels=2))
        assert result.sample_rate == 44100
        assert result.frames == 441
        resampled = engine.bounce_offline(
            EngineBounceOptions(total_frames=44100, num_channels=2, target_sample_rate=48000)
        )
        assert resampled.sample_rate == 48000
        assert resampled.frames == 48000


def test_bounce_refuses_a_source_rate_other_than_the_prepared_rate() -> None:
    with (
        RealtimeEngine(sample_rate=44100, max_block_size=BLOCK_SIZE) as engine,
        pytest.raises(SonareError),
    ):
        engine.bounce_offline(
            EngineBounceOptions(total_frames=441, num_channels=2, source_sample_rate=48000)
        )


def test_bounce_exports_scheduled_clip_content() -> None:
    samples, lufs = _bounce()
    assert len(samples) == FRAMES * 2
    # A bounce that rendered silence would still satisfy every shape assertion,
    # and would report -inf here rather than a real loudness.
    assert abs(_peak(samples) - AMPLITUDE) < 1e-3
    assert any(sample != 0.0 for sample in samples)
    assert math.isfinite(lufs)


def test_bounce_normalizes_to_requested_loudness() -> None:
    for target in (-20.0, -9.0):
        _, lufs = _bounce(normalize_lufs=True, target_lufs=target)
        assert abs(lufs - target) < 0.25


def test_bounce_omitted_target_lufs_is_the_shared_default_and_zero_is_literal() -> None:
    _, omitted = _bounce(normalize_lufs=True)
    assert abs(omitted - (-14.0)) < 0.25
    _, zero = _bounce(normalize_lufs=True, target_lufs=0.0)
    assert abs(zero) < 0.25


@pytest.mark.parametrize("target", [math.nan, math.inf, -math.inf])
def test_bounce_non_finite_target_lufs_is_refused_by_name(target: float) -> None:
    # A given non-finite target must not be read as the -14 LUFS default; omitting it is.
    with pytest.raises(ValueError, match="target_lufs"):
        _bounce(normalize_lufs=True, target_lufs=target)


def test_bounce_dither_types_apply_their_documented_behaviour() -> None:
    plain, _ = _bounce()
    none, _ = _bounce(dither=None, dither_bits=DITHER_BITS, dither_seed=1)
    rpdf, _ = _bounce(dither="rpdf", dither_bits=DITHER_BITS, dither_seed=1)
    tpdf, _ = _bounce(dither="tpdf", dither_bits=DITHER_BITS, dither_seed=1)
    shaped, _ = _bounce(dither="noiseShaped", dither_bits=DITHER_BITS, dither_seed=1)

    # Identifying each type by what it does keeps a remapped integer from
    # passing. Every non-None type lands the output on the target-depth grid --
    # that is what the shared bit-depth field means -- so the type selects only
    # the noise, the triangular one being wider than the rectangular.
    assert _max_abs_diff(none, plain) == 0.0
    assert _max_abs_diff(rpdf, none) > LSB / 4
    assert _max_abs_diff(tpdf, none) > _max_abs_diff(rpdf, none)
    assert _on_grid(rpdf) == len(rpdf)
    assert _on_grid(tpdf) == len(tpdf)
    assert _on_grid(shaped) == len(shaped)


def test_bounce_dataclass_defaults_match_the_native_defaults() -> None:
    """A literal dataclass default must equal what ``sonare_engine_bounce_options_init`` writes.

    A field left None keeps the init's value by construction, so only the literal defaults can
    drift; ``dither``'s None is the C "none", ordinal 0.
    """
    import dataclasses

    from libsonare._ffi_types_core import SonareEngineBounceOptions

    native = SonareEngineBounceOptions()
    assert native.struct_size == ctypes.sizeof(SonareEngineBounceOptions)
    assert native.dither == 0

    defaults = {
        field.name: field.default
        for field in dataclasses.fields(EngineBounceOptions)
        if field.default is not dataclasses.MISSING and field.default is not None
    }
    assert "dither_bits" in defaults
    for name, expected in defaults.items():
        actual = getattr(native, name)
        if isinstance(expected, bool):
            actual = bool(actual)
        elif isinstance(expected, float):
            actual = float(actual)
        assert actual == expected, name


def test_bounce_omitted_dither_seed_is_the_built_in_one_and_zero_is_literal() -> None:
    built_in = 0x51A7E5  # SONARE_ENGINE_DEFAULT_DITHER_SEED
    omitted, _ = _bounce(dither="tpdf", dither_bits=DITHER_BITS)
    explicit, _ = _bounce(dither="tpdf", dither_bits=DITHER_BITS, dither_seed=built_in)
    zero, _ = _bounce(dither="tpdf", dither_bits=DITHER_BITS, dither_seed=0)
    assert _max_abs_diff(omitted, explicit) == 0.0
    assert _max_abs_diff(zero, omitted) > 0.0


def test_bounce_dither_seed_is_reproducible_and_effective() -> None:
    first, _ = _bounce(dither="tpdf", dither_bits=DITHER_BITS, dither_seed=1)
    repeat, _ = _bounce(dither="tpdf", dither_bits=DITHER_BITS, dither_seed=1)
    other, _ = _bounce(dither="tpdf", dither_bits=DITHER_BITS, dither_seed=2)
    assert first == repeat
    assert _max_abs_diff(other, first) > 0.0


def test_bounce_reports_clipped_samples_only_when_it_dithered() -> None:
    # A 2.0-amplitude tone sits past full scale on about two thirds of its samples.
    hot = [2.0 * math.sin(2.0 * math.pi * 440.0 * i / SAMPLE_RATE) for i in range(FRAMES)]
    past_full_scale = 2 * sum(1 for sample in hot if abs(sample) > 1.0)

    def render(**overrides: object) -> int | None:
        clip = EngineClip(id=1, channels=[hot, list(hot)], start_ppq=0.0, length_samples=FRAMES)
        with RealtimeEngine(sample_rate=SAMPLE_RATE, max_block_size=BLOCK_SIZE) as engine:
            engine.set_clips([clip])
            engine.play()
            result = engine.bounce_offline(
                EngineBounceOptions(
                    total_frames=FRAMES,
                    block_size=BLOCK_SIZE,
                    num_channels=2,
                    **overrides,  # type: ignore[arg-type]
                )
            )
        return result.quantize_clipped_samples

    assert render() is None
    dithered = render(dither="tpdf", dither_bits=16)
    assert dithered is not None
    # The strip's smoothing and the bounce's pre-roll may move a sample or two across 1.0.
    assert abs(dithered - past_full_scale) <= 8
