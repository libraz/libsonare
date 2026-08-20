"""Bounce content, loudness-target and dither behaviour of the offline export.

The shape of a bounce result (frame count, channel count, sample rate, length)
says nothing about whether the scheduled material reached the export, whether a
requested loudness target was applied, or whether the requested dither ran.
These cases assert the audible content instead.
"""

from __future__ import annotations

import math

from libsonare import EngineBounceOptions, EngineClip, RealtimeEngine

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
    with RealtimeEngine(sample_rate=float(SAMPLE_RATE), max_block_size=BLOCK_SIZE) as engine:
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


def test_bounce_target_lufs_zero_resolves_to_shared_default() -> None:
    # 0.0 is the documented "use default" sentinel, not a literal 0 LUFS target.
    _, lufs = _bounce(normalize_lufs=True, target_lufs=0.0)
    assert abs(lufs - (-14.0)) < 0.25


def test_bounce_dither_types_apply_their_documented_behaviour() -> None:
    plain, _ = _bounce()
    none, _ = _bounce(dither=0, dither_bits=DITHER_BITS, dither_seed=1)
    rpdf, _ = _bounce(dither=1, dither_bits=DITHER_BITS, dither_seed=1)
    tpdf, _ = _bounce(dither=2, dither_bits=DITHER_BITS, dither_seed=1)
    shaped, _ = _bounce(dither=3, dither_bits=DITHER_BITS, dither_seed=1)

    # Identifying each type by what it does keeps a remapped integer from
    # passing: the two noise types perturb without quantizing, the triangular
    # one being the wider of the two, and only the shaped type snaps every
    # sample onto the target-depth grid.
    assert _max_abs_diff(none, plain) == 0.0
    assert _max_abs_diff(rpdf, none) > LSB / 4
    assert _max_abs_diff(tpdf, none) > _max_abs_diff(rpdf, none)
    assert _on_grid(rpdf) < len(rpdf) // 2
    assert _on_grid(shaped) == len(shaped)


def test_bounce_dataclass_defaults_match_the_native_defaults() -> None:
    """The dataclass defaults are the authority, so they must track the C ones.

    ``bounce_offline`` seeds the raw options from
    ``sonare_engine_bounce_options_default`` and then overwrites every field it
    knows about, so a caller who leaves a field alone gets the dataclass value,
    not the C one. Nothing else notices when the two drift apart.
    """
    import ctypes
    import dataclasses

    from libsonare._ffi_types_core import SonareEngineBounceOptions
    from libsonare._runtime import _get_lib

    native = SonareEngineBounceOptions()
    lib = _get_lib()
    assert lib.sonare_engine_bounce_options_default(ctypes.byref(native)) == 0

    defaults = {
        field.name: field.default
        for field in dataclasses.fields(EngineBounceOptions)
        if field.default is not dataclasses.MISSING
    }
    for name, expected in defaults.items():
        actual = getattr(native, name)
        if isinstance(expected, bool):
            actual = bool(actual)
        elif isinstance(expected, float):
            actual = float(actual)
        assert actual == expected, name


def test_bounce_dither_seed_is_reproducible_and_effective() -> None:
    first, _ = _bounce(dither=2, dither_bits=DITHER_BITS, dither_seed=1)
    repeat, _ = _bounce(dither=2, dither_bits=DITHER_BITS, dither_seed=1)
    other, _ = _bounce(dither=2, dither_bits=DITHER_BITS, dither_seed=2)
    assert first == repeat
    assert _max_abs_diff(other, first) > 0.0
