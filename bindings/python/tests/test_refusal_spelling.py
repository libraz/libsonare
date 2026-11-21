"""Every row of the generated spelling table reaches a refusal that names the caller's keyword.

The rows come from ``_field_spelling.OVERRIDES``, so a row the generator adds is swept
without a pin. Each row's keyword is given values outside any domain until the library
refuses one; every refusal on the way must spell the subject as the caller wrote it.
"""

from __future__ import annotations

import inspect
import math
import re
from collections.abc import Callable
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import SonareError
from libsonare._field_spelling import OVERRIDES

_SR = 22050
_TONE = (0.5 * np.sin(2 * np.pi * 440 * np.arange(_SR // 2) / _SR)).astype(np.float32)

# A valid value for each required argument the swept entries take.
_REQUIRED: dict[str, Any] = {
    "samples": _TONE,
    "reference": _TONE,
    "take": _TONE,
    "onset_envelope": np.abs(_TONE[:2048]),
    "mel": np.ones((128, 4), np.float32),
    "coefficients": np.ones((20, 4), np.float32),
    "values": np.ones((8, 4), np.float32),
    "tempogram": np.ones((384, 4), np.float32),
    "times": [0.1],
    "frames": 1,
    "time_sec": 0.1,
    "intervals": np.array([[0, 100]]),
    "data": b"MThd",
    "sample_rate": 48000,
}

_OUTSIDE: tuple[object, ...] = (0, -1, -1.5, math.nan, 1e30, "nope")

# A buffer no entry accepts, tried as well where the spelling takes samples or bytes.
_MALFORMED = np.ones((2, 2, 2), np.float32)
_BUFFERS = frozenset({"samples", "audio", "data", "reference", "take"})

# The closed set of reasons a row may go undriven.
_REASONS = frozenset(
    {"no value refuses it", "needs live state", "content refusal carries no subject"}
)

# Rows no facade call can make the library refuse under their subject.
_UNDRIVEN = {
    ("note_targets_from_smf", "bytes"): "content refusal carries no subject",
}


def _project_with_track() -> tuple[libsonare.Project, int]:
    project = libsonare.Project()
    return project, project.add_track("audio")


def _mixer_with_send() -> libsonare.Mixer:
    mixer = libsonare.Mixer(sample_rate=48000)
    mixer.add_strip("a")
    mixer.add_bus("b")
    mixer.add_send("a", "s", "b")
    return mixer


def _loop_takes() -> tuple[Callable[..., Any], dict[str, Any]]:
    project, track = _project_with_track()
    return project.add_loop_recording_takes, {
        "track_id": track,
        "start_ppq": 0.0,
        "loop_length_ppq": 4.0,
        "audio": _TONE,
        "audio_channels": 1,
        "audio_sample_rate": 48000,
    }


def _clip() -> tuple[Callable[..., Any], dict[str, Any]]:
    project, track = _project_with_track()
    return project.add_clip, {
        "track_id": track,
        "start_ppq": 0.0,
        "length_ppq": 4.0,
        "audio_channels": 1,
        "audio_sample_rate": 48000,
    }


def _voice_changer(method: str, **required: Any) -> Callable[[], tuple[Callable[..., Any], dict]]:
    def make() -> tuple[Callable[..., Any], dict[str, Any]]:
        changer = libsonare.RealtimeVoiceChanger(48000, num_channels=1, max_block_size=128)
        return getattr(changer, method), required

    return make


def _playback(method: str, **required: Any) -> Callable[[], tuple[Callable[..., Any], dict]]:
    def make() -> tuple[Callable[..., Any], dict[str, Any]]:
        renderer = libsonare.PlaybackRenderer({}, sample_rate=48000, max_block_size=128)
        return getattr(renderer, method), dict(required)

    return make


def _engine() -> libsonare.RealtimeEngine:
    return libsonare.RealtimeEngine(sample_rate=48000, max_block_size=128)


def _insert_param(method: str, ref: Any) -> Callable[[], tuple[Callable[..., Any], dict[str, Any]]]:
    """A strip insert parameter edit: ``ref`` is a track id, ``"master"`` or a bus id."""

    def make() -> tuple[Callable[..., Any], dict[str, Any]]:
        target = {"bus_id": ref} if "bus" in method else {"ref": ref}
        return getattr(_engine(), method), {**target, "insert_index": 0, "value": 0.0}

    return make


def _mixer_stereo() -> tuple[Callable[..., Any], dict[str, Any]]:
    mixer = libsonare.Mixer(sample_rate=48000)
    mixer.add_strip("a")
    plane = [_TONE[:128]]
    return mixer.process_stereo, {"left_inputs": plane, "right_inputs": plane}


# A C entry a handle method reaches: the bound method and a valid value for each argument.
_METHODS: dict[str, Callable[[], tuple[Callable[..., Any], dict[str, Any]]]] = {
    "engine_apply_bus_strip_insert_param_by_name_now": _insert_param(
        "apply_bus_insert_param_now", 1
    ),
    "engine_apply_master_strip_insert_param_by_name_now": _insert_param(
        "apply_strip_insert_param_now", "master"
    ),
    "engine_apply_track_strip_insert_param_by_name_now": _insert_param(
        "apply_strip_insert_param_now", 1
    ),
    "engine_drain_external_midi": lambda: (_engine().drain_external_midi, {}),
    "engine_resolve_master_automation_id": lambda: (
        _engine().resolve_strip_automation_id,
        {"ref": "master"},
    ),
    "engine_resolve_track_lane_automation_id": lambda: (
        _engine().resolve_strip_automation_id,
        {"ref": 1},
    ),
    "engine_restore_bus_strip_insert_param_by_name": _insert_param("restore_bus_insert_param", 1),
    "engine_restore_master_strip_insert_param_by_name": _insert_param(
        "restore_strip_insert_param", "master"
    ),
    "engine_restore_track_strip_insert_param_by_name": _insert_param(
        "restore_strip_insert_param", 1
    ),
    "engine_set_bus_strip_insert_param_by_name": _insert_param("set_bus_insert_param", 1),
    "engine_set_capture_buffer": lambda: (
        _engine().configure_capture,
        {"capacity_frames": 1024},
    ),
    "engine_set_master_strip_insert_param_by_name": _insert_param(
        "set_strip_insert_param", "master"
    ),
    "engine_set_record_offset_samples": lambda: (
        _engine().configure_capture,
        {"capacity_frames": 1024},
    ),
    "engine_set_track_output_bus": lambda: (_engine().set_track_output_bus, {"track_id": 1}),
    "engine_set_track_strip_insert_param_by_name": _insert_param("set_strip_insert_param", 1),
    "mixer_add_strip": lambda: (libsonare.Mixer(sample_rate=48000).add_strip, {"id": "a"}),
    "mixer_process_stereo": _mixer_stereo,
    "mixing_scene_preset_json": lambda: (libsonare.mixing_scene_preset, {}),
    "playback_loudness_meter_push_interleaved": lambda: (
        libsonare.PlaybackLoudnessMeter(num_channels=1, sample_rate=48000).push_interleaved,
        {},
    ),
    "playback_render_interleaved": lambda: (
        libsonare.render_playback,
        {"samples": _TONE[:128], "num_channels": 1, "sample_rate": 48000, "config": {}},
    ),
    "playback_renderer_process_interleaved": _playback(
        "process_interleaved", samples=_TONE[:128], num_input_channels=1
    ),
    "playback_renderer_process_planar": _playback("process_planar"),
    "playback_renderer_render_offline": _playback(
        "render_offline", samples=_TONE[:128], num_input_channels=1
    ),
    "project_add_clip": _clip,
    "project_add_loop_recording_takes": _loop_takes,
    "project_import_clip_file": lambda: (libsonare.Project().import_clip_file, {}),
    "project_import_smf": lambda: (libsonare.Project().import_smf, {}),
    "project_set_marker": lambda: (libsonare.Project().set_marker, {"ppq": 0.0}),
    "project_set_marker_ex_name": lambda: (
        libsonare.Project().set_marker,
        {"ppq": 0.0, "name": "m"},
    ),
    "project_set_overlap_policy": lambda: (libsonare.Project().set_overlap_policy, {}),
    "project_set_source_audio": lambda: (
        libsonare.Project().set_source_audio,
        {"source_id": 1, "audio": _TONE, "sample_rate": 48000},
    ),
    "project_set_warp_map": lambda: (
        libsonare.Project().set_warp_map,
        {"anchors": [(0.0, 0.0), (1.0, 1.0)]},
    ),
    "realtime_voice_changer_process_interleaved": _voice_changer("process_interleaved"),
    "realtime_voice_changer_process_interleaved_with_carrier": _voice_changer(
        "process_interleaved_with_carrier", carrier=_TONE[:128]
    ),
    "realtime_voice_changer_process_mono": _voice_changer("process_mono"),
    "realtime_voice_changer_process_mono_with_carrier": _voice_changer(
        "process_mono_with_carrier", carrier=_TONE[:128]
    ),
    "stream_analyzer_process_with_offset": lambda: (
        libsonare.StreamAnalyzer(sample_rate=22050).process_with_offset,
        {"samples": _TONE[:512]},
    ),
    "stream_analyzer_reset": lambda: (libsonare.StreamAnalyzer(sample_rate=22050).reset, {}),
    "strip_add_send": lambda: (
        _mixer_with_send().add_send,
        {"strip": "a", "destination_bus_id": "b"},
    ),
    "strip_set_dual_pan": lambda: (
        _mixer_with_send().set_dual_pan,
        {"strip": "a", "left": 0.0, "right": 0.0},
    ),
    "strip_set_send_db": lambda: (
        _mixer_with_send().set_send_db,
        {"strip": "a", "index": 0, "db": 0.0},
    ),
}


def _call(entry: str, spelling: str) -> tuple[Callable[..., Any], dict[str, Any]]:
    """The facade call reaching ``entry``, with every argument but ``spelling`` given."""
    if entry in _METHODS:
        fn, given = _METHODS[entry]()
        return fn, {k: v for k, v in given.items() if k != spelling}
    fn = getattr(libsonare, entry)
    params = inspect.signature(fn).parameters
    given = {
        name: _REQUIRED[name]
        for name, p in params.items()
        if p.default is p.empty and p.kind is p.POSITIONAL_OR_KEYWORD and name != spelling
    }
    return fn, given


def _refusals(entry: str, spelling: str) -> list[str]:
    texts = []
    for value in (*_OUTSIDE, *((_MALFORMED,) if spelling in _BUFFERS else ())):
        fn, given = _call(entry, spelling)
        try:
            fn(**given, **{spelling: value})
        except (SonareError, TypeError, ValueError) as exc:
            texts.append(str(exc))
    return texts


def _names(text: str, word: str) -> bool:
    """Whether ``text`` names ``word`` where a refusal puts its subject, not inside its prose."""
    return re.search(rf"(^[A-Za-z_.]+: |unknown key |, |\{{){re.escape(word)}\b", text) is not None


_ROWS = [(*k, v) for k, v in OVERRIDES.items()]


@pytest.mark.parametrize(
    ("entry", "subject", "spelling"), [r for r in _ROWS if r[:2] not in _UNDRIVEN]
)
def test_a_table_row_is_refused_under_the_callers_spelling(
    entry: str, subject: str, spelling: str
) -> None:
    assert spelling in inspect.signature(_call(entry, spelling)[0]).parameters
    texts = _refusals(entry, spelling)
    assert [t for t in texts if _names(t, subject)] == []
    assert any(_names(t, spelling) for t in texts), texts


@pytest.mark.parametrize(("entry", "subject", "spelling"), [r for r in _ROWS if r[:2] in _UNDRIVEN])
def test_an_undriven_row_never_leaks_its_subject(entry: str, subject: str, spelling: str) -> None:
    assert [t for t in _refusals(entry, spelling) if _names(t, subject)] == []


def test_every_table_row_is_driven_or_undriven_for_a_closed_reason() -> None:
    driven = [r for r in _ROWS if r[:2] not in _UNDRIVEN]
    print(f"refusal spelling sweep: driven={len(driven)} undriven={len(_UNDRIVEN)}")
    assert driven
    assert set(_UNDRIVEN) <= set(OVERRIDES), "an undriven key is no longer a table row"
    assert set(_UNDRIVEN.values()) <= _REASONS
