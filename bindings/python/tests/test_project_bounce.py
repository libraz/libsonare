"""Project bounce tests: bit-exact renders, built-in and external instruments, derived length."""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    BuiltinSynthConfig,
    ExternalInstrument,
    Project,
    SonareValueError,
)

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


def _build_midi_only_project() -> Project:
    """A MIDI-only arrangement: one MIDI track + clip with a sustained note."""
    project = Project()
    project.set_sample_rate(48000.0)
    _track, clip = project.add_midi_clip(0.0, 4.0)
    project.set_midi_events(
        clip,
        [
            Project.midi_note_on(0.0, 0, 0, 60, 100),
            Project.midi_note_off(2.0, 0, 0, 60, 0),
        ],
    )
    return project


def test_bounce_with_builtin_instrument_produces_non_silent_audio() -> None:
    """Flagship: a MIDI-only project bounced through the built-in synth is audible."""
    project = _build_midi_only_project()
    try:
        # Silent baseline: plain bounce has no instrument bound -> MIDI is silence.
        silent = project.bounce(
            total_frames=48000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert float(np.max(np.abs(silent))) == 0.0

        audio = project.bounce_with_builtin_instrument(
            total_frames=48000, block_size=128, num_channels=2, sample_rate=48000
        )
        assert audio.shape == (48000, 2)
        assert audio.dtype == np.float32
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_with_builtin_instrument_auto_derives_length() -> None:
    """Omitting total_frames lets the native layer derive the render length."""
    project = _build_midi_only_project()
    try:
        audio = project.bounce_with_builtin_instrument(num_channels=2, sample_rate=48000)
        assert audio.ndim == 2
        assert audio.shape[0] > 0
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_with_builtin_instrument_accepts_waveform_patch() -> None:
    """A non-default patch (named waveform + overrides) still renders audibly."""
    project = _build_midi_only_project()
    try:
        patch = BuiltinSynthConfig(waveform="saw", gain=0.3, polyphony=8)
        audio = project.bounce_with_builtin_instrument(
            patch, total_frames=24000, num_channels=2, sample_rate=48000
        )
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_builtin_synth_waveform_outside_the_enum_is_rejected() -> None:
    """An ordinal the enum does not name raises instead of resolving to sine.

    The values matter more than the count: 4 is the first one past the enum,
    which is what an off-by-one or a 1-based mirror emits, and -1 is the
    sentinel a generated caller reaches for. A guard that starts checking
    further out would pass on 42 alone. ``True`` is included because it is an
    ``int`` in Python and would otherwise resolve to saw.
    """
    project = _build_midi_only_project()
    try:
        for waveform in (4, -1, 5, 2**31, True):
            with pytest.raises(SonareValueError):
                project.bounce_with_builtin_instrument(
                    BuiltinSynthConfig(waveform=waveform), total_frames=1200
                )
        with pytest.raises(SonareValueError):
            project.bounce_with_builtin_instrument(
                BuiltinSynthConfig(waveform="noise"), total_frames=1200
            )
        # Rejection is by domain, not by the field having stopped working.
        for waveform in (0, 1, 2, 3, "sine", "saw", "sawtooth", "square", "triangle"):
            project.bounce_with_builtin_instrument(
                BuiltinSynthConfig(waveform=waveform), total_frames=1200
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


def test_bounce_with_instruments_hosts_external_callback() -> None:
    """Flagship: a MIDI-only project routes through a host-supplied instrument."""
    project = _build_midi_only_project()
    instrument = _ConstantInstrument(level=0.25)
    try:
        audio = project.bounce_with_instruments(
            instrument, total_frames=48000, block_size=128, num_channels=2, sample_rate=48000
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


def test_bounce_with_instruments_render_only_instrument() -> None:
    """Only render() is required; prepare/on_event are optional (duck-typed)."""

    class RenderOnly:
        def render(self, channels: np.ndarray, num_frames: int) -> None:
            channels += 0.1

    project = _build_midi_only_project()
    try:
        audio = project.bounce_with_instruments(
            RenderOnly(), total_frames=4800, num_channels=2, sample_rate=48000
        )
        assert audio.shape[0] > 0
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_with_instruments_auto_length_includes_tail_samples() -> None:
    """External instruments can report release/effect tail for auto-length bounce."""

    project = _build_midi_only_project()
    try:
        dry = _ConstantInstrument(level=0.0)
        no_tail = project.bounce_with_instruments(
            dry, total_frames=0, block_size=128, num_channels=2, sample_rate=48000
        )

        wet = _ConstantInstrument(level=0.0)
        wet.tail_samples = 4096
        with_tail = project.bounce_with_instruments(
            wet, total_frames=0, block_size=128, num_channels=2, sample_rate=48000
        )
        assert with_tail.shape[0] == no_tail.shape[0] + wet.tail_samples
    finally:
        project.close()


def test_bounce_with_instruments_propagates_callback_error() -> None:
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
            project.bounce_with_instruments(
                instrument, total_frames=48000, block_size=32, num_channels=2, sample_rate=48000
            )
        assert instrument.calls == 1
    finally:
        project.close()


def test_external_instrument_is_exported() -> None:
    import libsonare

    assert libsonare.ExternalInstrument is ExternalInstrument


def test_bounce_with_instruments_requires_an_instrument() -> None:
    project = _build_midi_only_project()
    try:
        with pytest.raises(ValueError, match="requires"):
            project.bounce_with_instruments(num_channels=2, sample_rate=48000)
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


def test_last_bounce_compile_result_is_empty_before_any_bounce() -> None:
    # No bounce has run, so the result is empty in full. A failed bounce is told
    # apart by its diagnostics, never by has_timeline alone: losing the timeline
    # always leaves an error diagnostic behind.
    project = Project()
    try:
        result = project.last_bounce_compile_result()
        assert result.has_timeline is False
        assert result.diagnostics == ()
        assert result.diagnostic_count == 0
    finally:
        project.close()
