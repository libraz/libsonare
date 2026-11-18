"""An omitted block size or capacity is the library's; an explicit 0 is refused.

Each constructor and prepare below reaches a C entry that reads 0 as "use the
default". Python spells that default as ``None`` only, so an explicit zero or a
negative value is refused by name rather than forwarded as the sentinel. Each
case also builds with the argument omitted and with an explicit value, so a guard
that refused everything cannot pass.
"""

from __future__ import annotations

import json
from collections.abc import Callable

import numpy as np
import pytest

import libsonare as ls
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 48000
_EMPTY_SCENE = '{"version":1,"strips":[],"buses":[],"connections":[]}'


def _voice_changer(size: int | None) -> object:
    return ls.RealtimeVoiceChanger(SR, num_channels=1, max_block_size=size)


def _mastering_chain(size: int | None) -> object:
    return ls.StreamingMasteringChain(sample_rate=SR, num_channels=1, max_block_size=size)


def _equalizer(size: int | None) -> object:
    return ls.StreamingEqualizer(sample_rate=SR, max_block_size=size)


def _playback(size: int | None) -> object:
    return ls.PlaybackRenderer({}, sample_rate=SR, max_block_size=size)


def _mixer(size: int | None) -> object:
    return ls.Mixer.from_scene_json(_EMPTY_SCENE, SR, max_block_size=size)


def _engine(size: int | None) -> object:
    return ls.RealtimeEngine(sample_rate=float(SR), max_block_size=size)


_BUILDERS: dict[str, Callable[[int | None], object]] = {
    "voice_changer": _voice_changer,
    "mastering_chain": _mastering_chain,
    "equalizer": _equalizer,
    "playback": _playback,
    "mixer": _mixer,
    "engine": _engine,
}


def _close(handle: object) -> None:
    close = getattr(handle, "close", None)
    if close is not None:
        close()


@pytest.mark.parametrize("name", sorted(_BUILDERS))
def test_an_omitted_block_size_builds_and_an_explicit_one_is_kept(name: str) -> None:
    build = _BUILDERS[name]
    _close(build(None))
    _close(build(64))


@pytest.mark.parametrize("name", sorted(_BUILDERS))
@pytest.mark.parametrize("value", [0, -1])
def test_an_explicit_non_positive_block_size_is_refused(name: str, value: int) -> None:
    with pytest.raises(SonareValueError, match="max_block_size"):
        _BUILDERS[name](value)


@pytest.mark.parametrize("name", ["voice_changer", "mastering_chain"])
def test_a_default_block_size_still_chunks_or_flushes_a_long_buffer(name: str) -> None:
    """The facade sizes its own loop from the handle, not from a Python literal."""
    samples = np.zeros(SR // 10, dtype=np.float32)
    if name == "voice_changer":
        with ls.RealtimeVoiceChanger(SR, num_channels=1) as changer:
            assert changer.process_mono(samples).shape == samples.shape
    else:
        with ls.StreamingMasteringChain(sample_rate=SR, num_channels=1) as chain:
            assert chain.flush_mono().ndim == 1


def test_engine_capacities_take_none_and_refuse_zero() -> None:
    with ls.RealtimeEngine(sample_rate=float(SR)) as engine:
        engine.prepare(float(SR), None, command_capacity=None, telemetry_capacity=None)
        engine.prepare(float(SR), 128, command_capacity=256, max_channels=2)
        for field in ("command_capacity", "telemetry_capacity", "max_channels"):
            with pytest.raises(SonareValueError, match=field):
                engine.prepare(float(SR), 128, **{field: 0})


def test_engine_offline_block_size_takes_none_and_refuses_zero() -> None:
    with ls.RealtimeEngine(sample_rate=float(SR), max_block_size=128) as engine:
        planes = [[0.0] * 256, [0.0] * 256]
        assert len(engine.render_offline(planes)) == 2
        with pytest.raises(SonareValueError, match="block_size"):
            engine.render_offline(planes, block_size=0)
        with pytest.raises(SonareValueError, match="block_size"):
            engine.bounce_offline(
                ls.EngineBounceOptions(total_frames=256, num_channels=2, block_size=0)
            )


@pytest.mark.parametrize(
    "build",
    [
        lambda: ls.RealtimeVoiceChanger(SR),
        lambda: ls.StreamingMasteringChain(sample_rate=SR),
        lambda: ls.PlaybackRenderer({}),
        lambda: ls.Mixer.from_scene_json(_EMPTY_SCENE),
        lambda: ls.RealtimeEngine(),
        lambda: ls.EngineBounceOptions(total_frames=256),
        lambda: ls.EngineFreezeOptions(total_frames=256),
    ],
    ids=[
        "voice_changer_num_channels",
        "mastering_chain_num_channels",
        "playback_sample_rate",
        "mixer_sample_rate",
        "engine_sample_rate",
        "bounce_options_num_channels",
        "freeze_options_num_channels",
    ],
)
def test_a_required_channel_count_or_sample_rate_has_no_default(
    build: Callable[[], object],
) -> None:
    with pytest.raises(TypeError):
        build()


def test_a_project_bounce_takes_none_for_every_omitted_size_and_refuses_zero() -> None:
    project = ls.Project()
    try:
        project.set_sample_rate(float(SR))
        assert project.bounce(total_frames=64).ndim == 2
        for field in ("total_frames", "block_size", "num_channels", "sample_rate"):
            with pytest.raises(SonareValueError, match=field):
                project.bounce(**{field: 0})
    finally:
        project.close()


def test_a_clip_states_its_audio_format_only_alongside_audio() -> None:
    project = ls.Project()
    try:
        track = project.add_track("audio", "a")
        audio = [0.1, 0.2, 0.1, 0.0]
        project.add_clip(track, 0.0, 480.0, audio=audio, audio_channels=1, audio_sample_rate=SR)
        project.add_clip(track, 480.0, 480.0)
        with pytest.raises(SonareValueError, match="audio_channels"):
            project.add_clip(track, 0.0, 480.0, audio=audio, audio_sample_rate=SR)
        with pytest.raises(SonareValueError, match="audio_sample_rate"):
            project.add_clip(track, 0.0, 480.0, audio=audio, audio_channels=1)
        with pytest.raises(SonareValueError, match="omit them without audio"):
            project.add_clip(track, 0.0, 480.0, audio_channels=1)
    finally:
        project.close()


@pytest.mark.parametrize("name", sorted(_BUILDERS))
def test_max_block_size_reports_the_given_value_or_the_library_default(name: str) -> None:
    build = _BUILDERS[name]
    given = build(64)
    omitted = build(None)
    try:
        assert given.max_block_size == 64  # type: ignore[attr-defined]
        assert omitted.max_block_size > 0  # type: ignore[attr-defined]
    finally:
        _close(given)
        _close(omitted)


def _accepts(process: Callable[[int], object], limit: int) -> None:
    """``limit`` frames go through and one more is refused, so the attribute is the bound."""
    process(limit)
    with pytest.raises(ls.SonareError):
        process(limit + 1)


def test_the_reported_block_is_the_one_each_handle_accepts() -> None:
    def zeros(n: int) -> np.ndarray:
        return np.zeros(n, dtype=np.float32)

    with ls.StreamingMasteringChain(sample_rate=SR, num_channels=1) as chain:
        _accepts(lambda n: chain.process_mono(zeros(n)), chain.max_block_size)
    with ls.StreamingEqualizer(sample_rate=SR) as eq:
        _accepts(lambda n: eq.process_mono(zeros(n)), eq.max_block_size)
    with ls.PlaybackRenderer({}, sample_rate=SR) as renderer:
        planes = renderer.num_input_channels
        _accepts(lambda n: renderer.process_planar([zeros(n)] * planes), renderer.max_block_size)
    with ls.RealtimeEngine(sample_rate=float(SR)) as engine:
        # The audio thread never raises: an over-long block is published as telemetry.
        limit = engine.max_block_size
        engine.process([zeros(limit), zeros(limit)])
        assert all(record.error != "maxBlockExceeded" for record in engine.drain_telemetry())
        engine.process([zeros(limit + 1), zeros(limit + 1)])
        assert any(record.error == "maxBlockExceeded" for record in engine.drain_telemetry())
    scene = json.dumps(ls.mixing_scene_preset(ls.mixing_scene_preset_names()[0]))
    mixer = ls.Mixer.from_scene_json(scene, SR)
    try:
        strips = mixer.strip_count
        _accepts(
            lambda n: mixer.process_stereo([zeros(n)] * strips, [zeros(n)] * strips),
            mixer.max_block_size,
        )
    finally:
        mixer.close()
