"""A closed handle reports the coded InvalidState error on every public member.

Placeholder data from a released handle (0, an empty list) reads as a real
measurement downstream, so each member must raise instead.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import numpy as np
import pytest

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library not available")

_INVALID_STATE = 7
_SCENE = '{"version":1,"strips":[{"id":"strip0"}],"buses":[],"connections":[]}'
_BLOCK = [0.0] * 128


def _call(obj: Any, name: str, *args: Any) -> Any:
    value = getattr(obj, name)
    return value(*args) if callable(value) else value


def _assert_invalid_state(fn: Callable[[], object]) -> None:
    from libsonare import SonareError

    with pytest.raises(SonareError) as caught:
        fn()
    assert caught.value.code == _INVALID_STATE


def _make_voice_changer() -> Any:
    from libsonare import RealtimeVoiceChanger

    return RealtimeVoiceChanger(48000)


def _make_retune() -> Any:
    from libsonare import StreamingRetune

    retune = StreamingRetune(semitones=2.0)
    retune.prepare(48000.0, 128)
    return retune


def _make_chain() -> Any:
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain({"eq.tilt.tiltDb": 1.0})
    chain.prepare(48000, 128, 1)
    return chain


def _make_mixer() -> Any:
    from libsonare import Mixer

    return Mixer.from_scene_json(_SCENE)


_MEMBERS: dict[str, tuple[Callable[[], Any], list[tuple[str, tuple[Any, ...]]]]] = {
    "RealtimeVoiceChanger": (
        _make_voice_changer,
        [
            ("reset", ()),
            ("set_config", ("neutral-monitor",)),
            ("non_finite_discard_count", ()),
            ("config_json", ()),
            ("config_pod", ()),
            ("process_mono", (_BLOCK,)),
            ("process_interleaved", (_BLOCK,)),
            ("process_planar_stereo", (_BLOCK, _BLOCK)),
            ("latency_samples", ()),
        ],
    ),
    "StreamingRetune": (
        _make_retune,
        [
            ("prepare", (48000.0, 128)),
            ("reset", ()),
            ("set_config", ()),
            ("config", ()),
            ("grain_size", ()),
            ("latency_samples", ()),
            ("process_mono", (_BLOCK,)),
        ],
    ),
    "StreamingMasteringChain": (
        _make_chain,
        [
            ("prepare", (48000, 128, 1)),
            ("process_mono", (_BLOCK,)),
            ("flush_mono", ()),
            ("reset", ()),
            ("set_parameter", ("eq.tilt.tiltDb", 1.0)),
            ("latency_samples", ()),
            ("non_finite_substitution_count", ()),
            ("non_finite_discard_count", ()),
            ("stage_names", ()),
        ],
    ),
    "Mixer": (
        _make_mixer,
        [
            ("compile", ()),
            ("strip_count", ()),
            ("bus_count", ()),
            ("bus_meter", ("master",)),
            ("strip_meter", (0,)),
            ("bus_non_finite_discard_count", ("master",)),
            ("tail_samples", ()),
            ("latency_samples", ()),
        ],
    ),
}

_CASES = [(owner, name, args) for owner, (_, members) in _MEMBERS.items() for name, args in members]


@pytest.mark.parametrize(("owner", "name", "args"), _CASES, ids=[f"{c[0]}.{c[1]}" for c in _CASES])
def test_closed_handle_member_raises_invalid_state(owner: str, name: str, args: tuple[Any, ...]):
    obj = _MEMBERS[owner][0]()
    obj.close()
    _assert_invalid_state(lambda: _call(obj, name, *args))


@pytest.mark.parametrize("owner", list(_MEMBERS))
def test_close_is_idempotent(owner: str) -> None:
    obj = _MEMBERS[owner][0]()
    obj.close()
    obj.close()


@pytest.mark.parametrize("owner", list(_MEMBERS))
def test_open_handle_is_not_refused_by_the_guard(owner: str) -> None:
    """Non-vacuity: the last listed member, a plain accessor, succeeds before close."""
    obj = _MEMBERS[owner][0]()
    try:
        name, args = _MEMBERS[owner][1][-1]
        _call(obj, name, *args)
    finally:
        obj.close()


def test_voice_changer_closed_process_mono_does_not_return_placeholder_audio() -> None:
    changer = _make_voice_changer()
    block = changer.process_mono(np.zeros(128, dtype=np.float32))
    assert len(block) == 128
    changer.close()
    _assert_invalid_state(lambda: changer.process_mono(np.zeros(128, dtype=np.float32)))


def test_voice_changer_closed_latency_is_not_zero_placeholder() -> None:
    changer = _make_voice_changer()
    changer.close()
    _assert_invalid_state(lambda: changer.latency_samples())
