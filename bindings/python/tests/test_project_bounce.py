"""Project bounce tests: bit-exact renders, built-in and external instruments, derived length."""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    BuiltinSynthConfig,
    ErrorCode,
    ExternalInstrument,
    Project,
    ProjectCompileResult,
    SonareError,
    SonareValueError,
)

from ._helpers import raises_coded
from ._project_helpers import _build_project

# --- bounce ----------------------------------------------------------------


def test_bounce_is_bit_exact_across_two_renders() -> None:
    project, *_ = _build_project()
    try:
        first = project.bounce(
            total_frames=24000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert first.shape == (24000, 2)
        assert first.dtype == np.float32

        second = project.bounce(
            total_frames=24000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert second.shape == first.shape
        # Deterministic: same project + options => bit-identical output.
        assert np.array_equal(first, second)
    finally:
        project.close()


@pytest.mark.parametrize("width", [1, 2])
def test_bounce_returns_the_requested_frames_at_every_width(width: int) -> None:
    project, *_ = _build_project()
    try:
        out = project.bounce(total_frames=4800, block_size=128, num_channels=width)
        assert out.shape == (4800, width)
    finally:
        project.close()


def _build_midi_only_project() -> Project:
    """A MIDI-only arrangement: one MIDI track + clip with a sustained note."""
    project = Project()
    project.set_sample_rate(48000)
    clip = project.add_midi_clip(0.0, 4.0).clip_id
    project.set_midi_events(
        clip,
        [
            Project.midi_note_on(0.0, 0, 0, 60, 100),
            Project.midi_note_off(2.0, 0, 0, 60, 0),
        ],
    )
    return project


def test_bounce_builtin_instruments_produces_non_silent_audio() -> None:
    """Flagship: a MIDI-only project bounced through the built-in synth is audible."""
    project = _build_midi_only_project()
    try:
        # Silent baseline: plain bounce has no instrument bound -> MIDI is silence.
        silent = project.bounce(
            total_frames=48000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert float(np.max(np.abs(silent))) == 0.0

        audio = project.bounce(
            builtin_instruments=[(0, BuiltinSynthConfig())],
            total_frames=48000,
            block_size=128,
            num_channels=2,
            sample_rate=48000,
        )
        assert audio.shape == (48000, 2)
        assert audio.dtype == np.float32
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_builtin_instruments_auto_derives_length() -> None:
    """Omitting total_frames lets the native layer derive the render length."""
    project = _build_midi_only_project()
    try:
        audio = project.bounce(
            builtin_instruments=[(0, BuiltinSynthConfig())],
            num_channels=2,
            sample_rate=48000,
        )
        assert audio.ndim == 2
        assert audio.shape[0] > 0
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_builtin_instruments_accepts_waveform_patch() -> None:
    """A non-default patch (named waveform + overrides) still renders audibly."""
    project = _build_midi_only_project()
    try:
        patch = BuiltinSynthConfig(waveform="saw", gain=0.3, polyphony=8)
        audio = project.bounce(
            builtin_instruments=[(0, patch)],
            total_frames=24000,
            num_channels=2,
            sample_rate=48000,
        )
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_builtin_synth_waveform_outside_the_enum_is_rejected() -> None:
    """A number never selects a waveform; an unknown name raises rather than resolving to sine."""
    project = _build_midi_only_project()
    try:
        for waveform in (0, 1, 4, -1, True):
            with pytest.raises(TypeError):
                project.bounce(
                    builtin_instruments=[(0, BuiltinSynthConfig(waveform=waveform))],
                    total_frames=1200,
                )
        with pytest.raises(SonareValueError):
            project.bounce(
                builtin_instruments=[(0, BuiltinSynthConfig(waveform="noise"))],
                total_frames=1200,
            )
        # Rejection is by domain, not by the field having stopped working.
        for waveform in ("sine", "saw", "sawtooth", "square", "triangle"):
            project.bounce(
                builtin_instruments=[(0, BuiltinSynthConfig(waveform=waveform))],
                total_frames=1200,
            )
    finally:
        project.close()


class _ConstantInstrument:
    """An :class:`ExternalInstrument` that emits a constant DC level and records
    every dispatched MIDI event, so a test can assert both audible output and
    sample-accurate event delivery."""

    def __init__(self, level: float = 0.25) -> None:
        self.level = float(level)
        self.prepared: tuple[float, int] | None = None
        self.events: list[tuple[int, tuple[int, ...], int]] = []
        self.render_calls = 0

    def prepare(self, sample_rate: float, max_block_size: int) -> None:
        self.prepared = (sample_rate, max_block_size)

    def on_event(self, destination_id: int, ump_words: tuple[int, ...], render_frame: int) -> None:
        self.events.append((destination_id, ump_words, render_frame))

    def render(self, channels: np.ndarray, num_frames: int) -> None:
        self.render_calls += 1
        channels += self.level


def test_bounce_instruments_hosts_external_callback() -> None:
    """Flagship: a MIDI-only project routes through a host-supplied instrument."""
    project = _build_midi_only_project()
    instrument = _ConstantInstrument(level=0.25)
    try:
        audio = project.bounce(
            instruments=[(0, instrument)],
            total_frames=48000,
            block_size=128,
            num_channels=2,
            sample_rate=48000,
        )
        assert audio.shape == (48000, 2)
        assert audio.dtype == np.float32
        # The instrument emits a constant 0.25 DC, so every rendered frame is audible.
        assert float(np.min(audio)) == pytest.approx(0.25, abs=1e-6)
        assert instrument.prepared is not None
        assert instrument.render_calls > 0
        # The note-on / note-off were delivered as dispatched UMP events.
        assert len(instrument.events) >= 2
        statuses = [(words[0] >> 20) & 0xF for _dst, words, _frame in instrument.events if words]
        assert 0x9 in statuses  # note-on
        assert 0x8 in statuses  # note-off
    finally:
        project.close()


def test_bounce_instruments_render_only_instrument() -> None:
    """Only render() is required; prepare/on_event are optional (duck-typed)."""

    class RenderOnly:
        def render(self, channels: np.ndarray, num_frames: int) -> None:
            channels += 0.1

    project = _build_midi_only_project()
    try:
        audio = project.bounce(
            instruments=[(0, RenderOnly())],
            total_frames=4800,
            num_channels=2,
            sample_rate=48000,
        )
        assert audio.shape[0] > 0
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_instruments_auto_length_includes_tail_samples() -> None:
    """External instruments can report release/effect tail for auto-length bounce."""

    project = _build_midi_only_project()
    try:
        dry = _ConstantInstrument(level=0.0)
        no_tail = project.bounce(
            instruments=[(0, dry)],
            block_size=128,
            num_channels=2,
            sample_rate=48000,
        )

        wet = _ConstantInstrument(level=0.0)
        wet.tail_samples = 4096
        with_tail = project.bounce(
            instruments=[(0, wet)],
            block_size=128,
            num_channels=2,
            sample_rate=48000,
        )
        assert with_tail.shape[0] == no_tail.shape[0] + wet.tail_samples
    finally:
        project.close()


def test_bounce_instruments_propagates_callback_error() -> None:
    """An exception raised inside a callback surfaces to the caller, not silenced."""

    class Boom:
        def __init__(self) -> None:
            self.calls = 0

        def render(self, channels: np.ndarray, num_frames: int) -> None:
            self.calls += 1
            raise ValueError("synthesis failed")

    project = _build_midi_only_project()
    instrument = Boom()
    try:
        with pytest.raises(ValueError, match="synthesis failed"):
            project.bounce(
                instruments=[(0, instrument)],
                total_frames=48000,
                block_size=32,
                num_channels=2,
                sample_rate=48000,
            )
        assert instrument.calls == 1
    finally:
        project.close()


def test_external_instrument_is_exported() -> None:
    import libsonare

    assert libsonare.ExternalInstrument is ExternalInstrument


def test_bounce_without_instrument_keywords_is_the_plain_bounce() -> None:
    """Every instrument keyword absent, None or empty renders the plain call's silence."""
    project = _build_midi_only_project()
    try:
        kwargs = {"total_frames": 4800, "block_size": 128, "num_channels": 2, "sample_rate": 48000}
        plain = project.bounce(**kwargs)
        assert float(np.max(np.abs(plain))) == 0.0
        for empty in (
            {},
            {"instruments": None},
            {"builtin_instruments": None},
            {"synth_instruments": None},
            {"sf2_instruments": None},
            {"instruments": [], "builtin_instruments": [], "synth_instruments": []},
            {"sf2_instruments": []},
        ):
            assert np.array_equal(project.bounce(**empty, **kwargs), plain)
    finally:
        project.close()


def test_bounce_mixes_instrument_kinds_on_separate_destinations() -> None:
    """One call hosts an external and a built-in instrument on two destinations."""
    project = Project()
    project.set_sample_rate(48000)
    try:
        for destination, note in ((0, 60), (1, 72)):
            midi = project.add_midi_clip(0.0, 1.0)
            project.set_track_midi_destination(midi.track_id, destination)
            project.set_midi_events(
                midi.clip_id,
                [
                    Project.midi_note_on(0.0, 0, 0, note, 100),
                    Project.midi_note_off(0.5, 0, 0, note, 0),
                ],
            )
        kwargs = {"total_frames": 12000, "block_size": 128, "num_channels": 2, "sample_rate": 48000}
        builtin_only = project.bounce(builtin_instruments=[(1, BuiltinSynthConfig())], **kwargs)
        external = _ConstantInstrument(level=0.25)
        both = project.bounce(
            instruments=[(0, external)],
            builtin_instruments=[(1, BuiltinSynthConfig())],
            **kwargs,
        )
        assert external.render_calls > 0
        assert float(np.max(np.abs(both - builtin_only))) > 0.1
    finally:
        project.close()


def test_bounce_refuses_a_destination_bound_twice() -> None:
    project = _build_midi_only_project()
    try:
        with pytest.raises(SonareError):
            project.bounce(
                builtin_instruments=[(0, BuiltinSynthConfig()), (0, BuiltinSynthConfig())],
                total_frames=4800,
            )
        with pytest.raises(SonareError):
            project.bounce(
                instruments=[(0, _ConstantInstrument())],
                builtin_instruments=[(0, BuiltinSynthConfig())],
                total_frames=4800,
            )
    finally:
        project.close()


def test_bounce_auto_derives_length_when_total_frames_omitted() -> None:
    """bounce() with total_frames omitted no longer renders empty (C auto-derives)."""
    project, *_ = _build_project()
    try:
        audio = project.bounce(num_channels=2, sample_rate=48000)
        assert audio.ndim == 2
        assert audio.shape[0] > 0
    finally:
        project.close()


def test_last_bounce_compile_result_is_none_before_any_bounce() -> None:
    project = Project()
    try:
        assert project.last_bounce_compile_result() is None
    finally:
        project.close()


def test_last_bounce_compile_result_after_a_clean_bounce_has_no_diagnostics() -> None:
    project = Project()
    try:
        track = project.add_track("audio", "a")
        project.add_clip(
            track, 0.0, 1.0, audio=[0.1] * 480, audio_channels=1, audio_sample_rate=48000
        )
        project.bounce(total_frames=480, num_channels=2, sample_rate=48000)
        result = project.last_bounce_compile_result()
        assert isinstance(result, ProjectCompileResult)
        assert result.diagnostics == ()
    finally:
        project.close()


def test_last_bounce_compile_result_after_a_failed_bounce_carries_the_error() -> None:
    project = Project()
    try:
        # One track more than the realtime track mixer holds is a compile error.
        for index in range(33):
            project.add_track("audio", f"t{index}")
        with raises_coded(ErrorCode.INVALID_STATE):
            project.bounce(total_frames=480, num_channels=2, sample_rate=48000)
        result = project.last_bounce_compile_result()
        assert isinstance(result, ProjectCompileResult)
        assert ("automationLaneCapacity", "error") in [
            (d.code, d.severity) for d in result.diagnostics
        ]
    finally:
        project.close()
