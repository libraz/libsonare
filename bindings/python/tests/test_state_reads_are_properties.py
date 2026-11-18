"""Cheap state reads are properties, and planar stereo returns a StereoBlock."""

from __future__ import annotations

import inspect

import numpy as np
import pytest

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

_STATE_READS = [
    ("RealtimeVoiceChanger", "latency_samples"),
    ("RealtimeVoiceChanger", "non_finite_discard_count"),
    ("StreamingRetune", "grain_size_samples"),
    ("StreamingRetune", "latency_samples"),
    ("PolyphonicAnalysis", "note_count"),
    ("PolyphonicAnalysis", "frame_count"),
    ("Mixer", "strip_count"),
    ("Mixer", "bus_count"),
    ("Mixer", "vca_group_count"),
    ("Mixer", "tail_samples"),
    ("Mixer", "latency_samples"),
    ("PlaybackRenderer", "num_input_channels"),
    ("PlaybackRenderer", "num_output_channels"),
    ("PlaybackRenderer", "latency_samples"),
    ("PlaybackRenderer", "non_finite_discard_count"),
    ("PlaybackLoudnessMeter", "integrated_lufs"),
    ("RealtimeEngine", "midi_instrument_count"),
    ("RealtimeEngine", "midi_cc_binding_count"),
    ("RealtimeEngine", "midi_input_pending_count"),
    ("RealtimeEngine", "graph_node_count"),
    ("RealtimeEngine", "graph_connection_count"),
    ("RealtimeEngine", "clip_page_prefetch_frames"),
    ("RealtimeEngine", "tail_samples"),
    ("RealtimeEngine", "clip_count"),
    ("RealtimeEngine", "automation_lane_count"),
    ("RealtimeEngine", "external_midi_dropped_count"),
    ("RealtimeEngine", "clip_page_request_overflow_count"),
    ("RealtimeEngine", "warp_stretch_overflow_count"),
    ("Project", "soundfont_preset_count"),
]


@pytest.mark.parametrize(("class_name", "name"), _STATE_READS)
def test_state_read_is_a_property(class_name: str, name: str) -> None:
    cls = getattr(libsonare, class_name)
    assert isinstance(inspect.getattr_static(cls, name), property)


@pytest.mark.parametrize("with_carrier", [False, True])
def test_process_planar_stereo_returns_a_stereo_block(with_carrier: bool) -> None:
    tone = (0.2 * np.sin(np.arange(512) * 0.05)).astype(np.float32)
    with libsonare.RealtimeVoiceChanger(48_000, max_block_size=128, num_channels=2) as changer:
        if with_carrier:
            result = changer.process_planar_stereo_with_carrier(tone, tone, tone)
        else:
            result = changer.process_planar_stereo(tone, tone)
    assert isinstance(result, libsonare.StereoBlock)
    left, right = result
    assert left.shape == right.shape == tone.shape
