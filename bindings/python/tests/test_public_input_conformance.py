"""Cross-binding public-input transaction corpus."""

from __future__ import annotations

import json
import math
import re
from pathlib import Path
from typing import Any

import pytest

from libsonare import (
    EngineMarker,
    MidiCcBinding,
    Project,
    ProjectMidiCcBindingKind,
    RealtimeEngine,
    SonareValueError,
    mix_stereo,
)

_CORPUS = json.loads(
    (Path(__file__).resolve().parents[3] / "tests/conformance/public_input_corpus.json").read_text()
)


def _marker_value(raw: dict[str, Any]) -> EngineMarker:
    marker_id = 0xFFFFFFFF if raw["id"] == "uint32_max" else raw["id"]
    ppq = {"nan": math.nan, "inf": math.inf}.get(raw["ppq"], raw["ppq"])
    return EngineMarker(marker_id, ppq, raw["name"])


def _snapshot(engine: RealtimeEngine) -> bytes:
    values = []
    for marker in engine.markers():
        values.append(
            [
                marker.id,
                marker.ppq,
                marker.name,
                marker.kind,
                marker.key_fifths,
                marker.key_minor,
            ]
        )
    return json.dumps(values, ensure_ascii=False, separators=(",", ":")).encode()


def _mix_option_value(raw: object) -> object:
    specials = {"nan": math.nan, "inf": math.inf, "neg_inf": -math.inf}
    return specials[raw] if isinstance(raw, str) and raw in specials else raw


@pytest.mark.parametrize(
    "case", _CORPUS["marker_transaction"]["cases"], ids=lambda case: case["id"]
)
def test_shared_marker_transaction_corpus(case: dict[str, Any]) -> None:
    initial = [_marker_value(raw) for raw in _CORPUS["marker_transaction"]["initial"]]
    candidate = [_marker_value(raw) for raw in case["markers"]]
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_markers(initial)
        before = _snapshot(engine)
        if case["accepted"]:
            engine.set_markers(candidate)
            stored = engine.markers()
            assert len(stored) == len(candidate)
            for actual, expected in zip(stored, candidate, strict=True):
                assert (actual.id, actual.ppq, actual.name) == (
                    expected.id,
                    expected.ppq,
                    expected.name,
                )
        else:
            with pytest.raises((ValueError, OverflowError, TypeError, RuntimeError)):
                engine.set_markers(candidate)
            assert _snapshot(engine) == before


@pytest.mark.parametrize("case", _CORPUS["mix_options"]["cases"], ids=lambda case: case["id"])
def test_shared_mix_option_corpus_rejects_and_recovers(case: dict[str, Any]) -> None:
    value = _mix_option_value(case["value"])
    options = (
        {"pan": [0.0], "pan_mode": value}
        if case["field"] == "pan_mode"
        else {case["field"]: [value]}
    )
    strips = [([0.25, -0.25], [0.25, -0.25])]

    with pytest.raises((ValueError, OverflowError, TypeError, RuntimeError)):
        mix_stereo(strips, sample_rate=48000, **options)

    recovered = mix_stereo(strips, sample_rate=48000)
    assert len(recovered.left) == 2
    assert len(recovered.right) == 2


def _python_field(case: dict[str, Any]) -> str:
    """The corpus field is the Node spelling; the Python keyword is its snake case."""
    return re.sub(r"(?<!^)(?=[A-Z])", "_", case["field"]).lower()


def _midi_cc_binding(case: dict[str, Any]) -> MidiCcBinding:
    return MidiCcBinding(**{"cc_number": 7, "param_id": 1, _python_field(case): case["value"]})


_MIDI_CC_CASES = _CORPUS["midi_cc_binding"]["cases"]


def test_midi_cc_binding_corpus_has_both_outcomes() -> None:
    assert len(_MIDI_CC_CASES) >= 11
    assert {case["accepted"] for case in _MIDI_CC_CASES} == {True, False}


# Each byte field with the binding kind that reads it and an in-range value the
# library accepts there (a 14-bit LSB controller is the MSB's pair, 32..63).
_MIDI_CC_BYTE_FIELDS: dict[str, tuple[ProjectMidiCcBindingKind, int]] = {
    "cc_number": ("controlChange7", 127),
    "cc_lsb_number": ("controlChange14", 33),
    "selector_msb": ("rpn", 127),
    "selector_lsb": ("nrpn", 127),
}


@pytest.mark.parametrize("field", sorted(_MIDI_CC_BYTE_FIELDS))
@pytest.mark.parametrize("value", [128, 255])
def test_bind_midi_cc_refuses_a_byte_above_the_midi_data_range(field: str, value: int) -> None:
    # The C fields are uint8, so 128..255 fit the struct and only the MIDI domain refuses them.
    kind, accepted = _MIDI_CC_BYTE_FIELDS[field]
    binding = MidiCcBinding(param_id=1, kind=kind, **{"cc_number": 1, field: value})
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(
            SonareValueError,
            match=rf"^RealtimeEngine\.bind_midi_cc: {field} must be an integer in \[0, 127\]$",
        ):
            engine.bind_midi_cc(binding)
        assert engine.midi_cc_binding_count == 0
        engine.bind_midi_cc(
            MidiCcBinding(param_id=1, kind=kind, **{"cc_number": 1, field: accepted})
        )
        assert engine.midi_cc_binding_count == 1


@pytest.mark.parametrize("case", _MIDI_CC_CASES, ids=lambda case: case["id"])
def test_bind_midi_cc_byte_fields_follow_the_corpus(case: dict[str, Any]) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        if case["accepted"]:
            engine.bind_midi_cc(_midi_cc_binding(case))
            assert engine.midi_cc_binding_count == 1
        else:
            with pytest.raises(
                SonareValueError, match=rf"^\S+: {_python_field(case)} must be an integer in \[0, "
            ):
                engine.bind_midi_cc(_midi_cc_binding(case))
            assert engine.midi_cc_binding_count == 0


@pytest.mark.parametrize("case", _MIDI_CC_CASES, ids=lambda case: case["id"])
def test_midi_cc_to_breakpoint_byte_fields_follow_the_corpus(case: dict[str, Any]) -> None:
    event = Project.midi_cc(0.0, 0, 0, 7, 64)
    if case["accepted"]:
        Project.midi_cc_to_breakpoint([_midi_cc_binding(case)], event)
    else:
        with pytest.raises(
            SonareValueError, match=rf"^\S+: {_python_field(case)} must be an integer in \[0, "
        ):
            Project.midi_cc_to_breakpoint([_midi_cc_binding(case)], event)


_STREAMING = _CORPUS["streaming_contract"]
# The facade folds the band counts into the matrix shapes and the flags into None.
_STREAM_FOLDED = {"n_mels", "n_chroma", "feature_flags"}
_STREAM_CHORD_FIELDS = {"chord_root", "chord_quality", "chord_confidence"}
_STREAM_FEATURE_FIELDS = {
    "mel": {"mel"},
    "chroma": {"chroma"} | _STREAM_CHORD_FIELDS,
    "onset": {"onset_strength"},
    "spectral": {"spectral_centroid", "spectral_flatness"},
}


def _small_analyzer(**options: Any) -> Any:
    from libsonare import StreamAnalyzer

    return StreamAnalyzer(sample_rate=8000, n_fft=256, hop_length=64, n_mels=8, **options)


def _stream_tone(n_samples: int) -> list[float]:
    return [0.5 * math.sin(2.0 * math.pi * 440.0 * i / 8000) for i in range(n_samples)]


def test_stream_feature_flags_follow_the_corpus() -> None:
    from libsonare import streaming

    bits = {
        "mel": streaming._FEATURE_MEL,
        "chroma": streaming._FEATURE_CHROMA,
        "onset": streaming._FEATURE_ONSET,
        "spectral": streaming._FEATURE_SPECTRAL,
    }
    assert bits == _STREAMING["feature_flags"]


def test_stream_frames_fields_follow_the_corpus() -> None:
    import dataclasses

    from libsonare import StreamFrames

    fields = {field.name for field in dataclasses.fields(StreamFrames)}
    expected = set(_STREAMING["frame_fields"]) - _STREAM_FOLDED
    assert fields - _STREAM_CHORD_FIELDS == expected


def test_stream_output_format_is_the_float_default_and_unreachable() -> None:
    import ctypes
    import inspect

    from libsonare import StreamAnalyzer
    from libsonare._ffi_types_streaming import SonareStreamConfig
    from libsonare._runtime import _get_lib

    lib = _get_lib()
    raw = SonareStreamConfig()
    assert lib.sonare_stream_analyzer_config_default(ctypes.byref(raw)) == 0
    assert raw.output_format == _STREAMING["output_format_default"]
    # The analyzer never offers the generic output format, so a legacy one cannot be asked for.
    assert "output_format" not in inspect.signature(StreamAnalyzer).parameters
    for legacy in _STREAMING["legacy_output_formats_rejected"]:
        raw.sample_rate = 8000
        raw.output_format = legacy
        handle = ctypes.c_void_p()
        assert lib.sonare_stream_analyzer_create(ctypes.byref(raw), ctypes.byref(handle)) != 0
        assert not handle.value


@pytest.mark.parametrize("feature", sorted(_STREAMING["feature_flags"]))
def test_stream_disabled_feature_reads_as_none(feature: str) -> None:
    assert _STREAMING["disabled_feature_shape"] == "empty"
    switches = {f"compute_{name}": name != feature for name in _STREAM_FEATURE_FIELDS}
    with _small_analyzer(**switches) as analyzer:
        analyzer.process(_stream_tone(4096))
        frames = analyzer.read_frames(1024)
    assert frames.n_frames > 0
    for name, fields in _STREAM_FEATURE_FIELDS.items():
        for field in fields:
            value = getattr(frames, field)
            if name == feature:
                assert value is None, field
            else:
                assert value is not None and value.shape[-1] == frames.n_frames, field


def test_stream_overflow_drops_the_newest_frames() -> None:
    import numpy as np

    assert _STREAMING["overflow_policy"] == "drop_newest"
    tone = _stream_tone(4096)
    with _small_analyzer() as unbounded:
        unbounded.process(tone)
        reference = unbounded.read_frames(1024)
    cap = 3
    with _small_analyzer(max_pending_frames=cap) as bounded:
        bounded.process(tone)
        stats = bounded.stats()
        kept = bounded.read_frames(1024)
    assert reference.n_frames > cap
    assert stats.dropped_output_frames == reference.n_frames - cap
    assert kept.n_frames == cap
    # The retained frames are the oldest ones, sample for sample.
    np.testing.assert_array_equal(kept.timestamps_sec, reference.timestamps_sec[:cap])
    np.testing.assert_array_equal(kept.rms_energy, reference.rms_energy[:cap])
