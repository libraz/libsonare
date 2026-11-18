"""An empty string or list is never read as "not given", and an input is never cut to fit.

Each case pairs the refusal with the spelling that does mean absence (``None``),
or with the real value an empty list stands for, so the test fails if either
half drifts back to the sentinel.
"""

from __future__ import annotations

import numpy as np
import pytest

import libsonare
from libsonare import (
    EngineGraphNode,
    EngineMarker,
    ParameterInfo,
    Project,
    RealtimeEngine,
    SonareValueError,
)
from libsonare._engine_conversions import _graph_node_to_c

SR = 48000


def _tone() -> np.ndarray:
    t = np.arange(SR // 2, dtype=np.float32) / SR
    return (0.3 * np.sin(2.0 * np.pi * 440.0 * t)).astype(np.float32)


def test_streaming_preview_refuses_an_empty_platform_list() -> None:
    tone = _tone()
    assert libsonare.mastering_streaming_preview(tone, SR, platforms=None) is not None
    with pytest.raises(
        SonareValueError, match="^mastering_streaming_preview: platforms must not be empty$"
    ):
        libsonare.mastering_streaming_preview(tone, SR, platforms=[])
    with pytest.raises(
        SonareValueError,
        match="^mastering_streaming_preview_stereo: platforms must not be empty$",
    ):
        libsonare.mastering_streaming_preview_stereo(tone, tone, SR, platforms=[])


def test_warp_map_name_is_none_or_a_real_name() -> None:
    with Project() as project:
        project.set_warp_map(1, [(0.0, 0.0), (100.0, 100.0)])
        project.set_warp_map(2, [(0.0, 0.0), (100.0, 100.0)], "w2")
        with pytest.raises(SonareValueError, match="^set_warp_map: name must not be empty"):
            project.set_warp_map(3, [(0.0, 0.0), (100.0, 100.0)], "")


def test_project_marker_name_is_none_when_unnamed_and_empty_is_refused() -> None:
    with Project() as project:
        unnamed = project.set_marker(1.0)
        named = project.set_marker(2.0, name="chorus")
        by_id = {marker.id: marker for marker in project.markers()}
        assert by_id[unnamed].name is None
        assert by_id[named].name == "chorus"
        with pytest.raises(SonareValueError, match="^marker name must not be empty"):
            project.set_marker(3.0, name="")


def test_engine_marker_name_is_none_when_unnamed_and_is_never_cut() -> None:
    assert EngineMarker(1, 0.0).name is None
    longest = "m" * 63
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.set_markers([EngineMarker(1, 0.0), EngineMarker(2, 1.0, longest)])
        names = {marker.id: marker.name for marker in engine.markers()}
        assert names == {1: None, 2: longest}
        with pytest.raises(SonareValueError, match="^marker name must not be empty"):
            engine.set_markers([EngineMarker(3, 0.0, "")])
        with pytest.raises(SonareValueError, match="^name must be at most 63 UTF-8 bytes$"):
            engine.set_markers([EngineMarker(4, 0.0, longest + "m")])
        # A multi-byte name that does not fit is refused, not cut at a codepoint.
        with pytest.raises(SonareValueError, match="at most 63 UTF-8 bytes"):
            engine.set_markers([EngineMarker(5, 0.0, "é" * 32)])


def test_engine_fixed_identifiers_are_refused_rather_than_cut() -> None:
    with (
        RealtimeEngine(sample_rate=SR, max_block_size=128) as engine,
        pytest.raises(SonareValueError, match="^unit must be at most 15 UTF-8 bytes$"),
    ):
        engine.add_parameter(
            ParameterInfo(
                id=7,
                name="gain",
                unit="u" * 16,
                min_value=0.0,
                max_value=1.0,
                default_value=0.0,
                rt_safe=True,
                default_curve="linear",
            )
        )
    with pytest.raises(SonareValueError, match="^id must be at most 63 UTF-8 bytes$"):
        _graph_node_to_c(EngineGraphNode(id="n" * 64))


def test_clip_take_and_comp_lists_are_required_and_empty_clears() -> None:
    with Project() as project:
        clip = project.add_midi_clip(0.0, 4.0).clip_id
        project.set_clip_takes(clip, [])
        project.set_clip_comp_segments(clip, [])
        with pytest.raises(TypeError, match="^set_clip_takes: takes must be a sequence"):
            project.set_clip_takes(clip, None)  # type: ignore[arg-type]
        with pytest.raises(TypeError, match="^set_clip_comp_segments: segments must be a sequence"):
            project.set_clip_comp_segments(clip, None)  # type: ignore[arg-type]
