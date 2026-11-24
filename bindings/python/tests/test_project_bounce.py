"""Project bounce tests: bit-exact renders, built-in and external instruments, derived length."""

from __future__ import annotations

import ctypes
import dataclasses

import numpy as np
import pytest

import libsonare as ls
import libsonare._project_render as _project_render
from libsonare import (
    BuiltinSynthConfig,
    EngineMetronomeConfig,
    ErrorCode,
    ExternalInstrument,
    Project,
    ProjectArticulationBinding,
    ProjectBounceResult,
    ProjectCompileResult,
    ProjectControllerProfileBinding,
    ProjectInstrumentBinding,
    ProjectSf2InstrumentBinding,
    Sf2InstrumentConfig,
    SonareError,
    SonareValueError,
    SynthPatch,
)
from libsonare._project_render import _ProjectRenderMixin

from ._helpers import raises_coded
from ._project_helpers import _build_project

# --- bounce ----------------------------------------------------------------


def test_bounce_is_bit_exact_across_two_renders() -> None:
    project, *_ = _build_project()
    try:
        first_result = project.bounce(total_frames=24000, block_size=128, num_channels=2)
        assert isinstance(first_result, ProjectBounceResult)
        assert dataclasses.is_dataclass(first_result)
        assert first_result.sample_rate == 48000
        first = first_result.channels
        assert first.shape == (2, 24000)
        assert first.dtype == np.float32

        second_result = project.bounce(total_frames=24000, block_size=128, num_channels=2)
        second = second_result.channels
        assert second.shape == first.shape
        # Deterministic: same project + options => bit-identical output.
        assert np.array_equal(first, second)
        with pytest.raises(dataclasses.FrozenInstanceError):
            first_result.sample_rate = 44100  # type: ignore[misc]
    finally:
        project.close()


def test_project_render_uses_project_sample_rate_without_a_rate_override() -> None:
    project = _build_midi_only_project()
    try:
        project.set_sample_rate(44100)
        assert project.count_in_frames(1) == 88200
        result = project.bounce(total_frames=128, num_channels=2)
        assert result.sample_rate == 44100
        with pytest.raises(TypeError, match="sample_rate"):
            project.count_in_frames(1, sample_rate=48000)  # type: ignore[call-arg]
        with pytest.raises(TypeError, match="sample_rate"):
            project.bounce(sample_rate=48000)  # type: ignore[call-arg]
    finally:
        project.close()


@pytest.mark.parametrize("width", [1, 2])
def test_bounce_returns_the_requested_frames_at_every_width(width: int) -> None:
    project, *_ = _build_project()
    try:
        out = project.bounce(total_frames=4800, block_size=128, num_channels=width).channels
        assert out.shape == (width, 4800)
    finally:
        project.close()


@pytest.mark.parametrize(
    ("layout", "width"), [(None, 2), ("mono", 1), ("stereo", 2), ("5.1", 6), ("7.1", 8)]
)
def test_bounce_takes_the_scene_master_width_when_num_channels_is_omitted(
    layout: str | None, width: int
) -> None:
    project = Project()
    try:
        project.set_sample_rate(48000)
        if layout is not None:
            project.set_mixer_scene(
                {"version": 2, "buses": [{"id": "master", "role": "master", "layout": layout}]}
            )
        project.add_midi_clip(0, 1)
        assert project.bounce(total_frames=128).channels.shape == (width, 128)
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
            ls.midi_note_on(0.0, 0, 0, 60, 100),
            ls.midi_note_off(2.0, 0, 0, 60, 0),
        ],
    )
    return project


def test_bounce_builtin_instruments_produces_non_silent_audio() -> None:
    """Flagship: a MIDI-only project bounced through the built-in synth is audible."""
    project = _build_midi_only_project()
    try:
        # Silent baseline: plain bounce has no instrument bound -> MIDI is silence.
        silent = project.bounce(total_frames=48000, block_size=128, num_channels=2).channels
        assert float(np.max(np.abs(silent))) == 0.0

        audio = project.bounce(
            builtin_instruments=[ProjectInstrumentBinding(instrument=BuiltinSynthConfig())],
            total_frames=48000,
            block_size=128,
            num_channels=2,
        ).channels
        assert audio.shape == (2, 48000)
        assert audio.dtype == np.float32
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_bounce_builtin_instruments_auto_derives_length() -> None:
    """Omitting total_frames lets the native layer derive the render length."""
    project = _build_midi_only_project()
    try:
        audio = project.bounce(
            builtin_instruments=[ProjectInstrumentBinding(instrument=BuiltinSynthConfig())],
            num_channels=2,
        ).channels
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
            builtin_instruments=[ProjectInstrumentBinding(instrument=patch)],
            total_frames=24000,
            num_channels=2,
        ).channels
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
                    builtin_instruments=[
                        ProjectInstrumentBinding(instrument=BuiltinSynthConfig(waveform=waveform))
                    ],
                    total_frames=1200,
                )
        with pytest.raises(SonareValueError):
            project.bounce(
                builtin_instruments=[
                    ProjectInstrumentBinding(instrument=BuiltinSynthConfig(waveform="noise"))
                ],
                total_frames=1200,
            )
        # Rejection is by domain, not by the field having stopped working.
        for waveform in ("sine", "saw", "sawtooth", "square", "triangle"):
            project.bounce(
                builtin_instruments=[
                    ProjectInstrumentBinding(instrument=BuiltinSynthConfig(waveform=waveform))
                ],
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
            instruments=[ProjectInstrumentBinding(instrument=instrument)],
            total_frames=48000,
            block_size=128,
            num_channels=2,
        ).channels
        assert audio.shape == (2, 48000)
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
            instruments=[ProjectInstrumentBinding(instrument=RenderOnly())],
            total_frames=4800,
            num_channels=2,
        ).channels
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
            instruments=[ProjectInstrumentBinding(instrument=dry)],
            block_size=128,
            num_channels=2,
        ).channels

        wet = _ConstantInstrument(level=0.0)
        wet.tail_samples = 4096
        with_tail = project.bounce(
            instruments=[ProjectInstrumentBinding(instrument=wet)],
            block_size=128,
            num_channels=2,
        ).channels
        assert with_tail.shape[1] == no_tail.shape[1] + wet.tail_samples
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
                instruments=[ProjectInstrumentBinding(instrument=instrument)],
                total_frames=48000,
                block_size=32,
                num_channels=2,
            )
        assert instrument.calls == 1
    finally:
        project.close()


def test_bounce_preserves_soundfont_reason_with_callback_instrument() -> None:
    project = _build_midi_only_project()
    try:
        with pytest.raises(SonareError, match="urn:missing") as refused:
            project.bounce(
                instruments=[
                    ProjectInstrumentBinding(instrument=_ConstantInstrument(), destination_id=1)
                ],
                sf2_instruments=[
                    ProjectSf2InstrumentBinding(
                        instrument=Sf2InstrumentConfig(), sound_font_uri="urn:missing"
                    )
                ],
                total_frames=128,
            )
        assert refused.value.code == ErrorCode.INVALID_STATE
        assert refused.value.reason == "unresolvedSoundFont"
    finally:
        project.close()


def test_count_in_frames_preserves_int64_precision_above_javascript_safe_range(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    expected = 2**53 + 1

    class FakeLib:
        @staticmethod
        def sonare_project_count_in_frames(
            _handle: object, count_in_bars: int, out_frames: object
        ) -> int:
            assert count_in_bars == 16
            ctypes.cast(out_frames, ctypes.POINTER(ctypes.c_int64)).contents.value = expected
            return int(ErrorCode.OK)

    class FakeProject(_ProjectRenderMixin):
        @staticmethod
        def _require_handle() -> ctypes.c_void_p:
            return ctypes.c_void_p(1)

    monkeypatch.setattr(_project_render, "_get_lib", lambda: FakeLib())
    result = FakeProject().count_in_frames(16)
    assert type(result) is int
    assert result == expected


def test_count_in_frames_marks_negative_native_output_malformed(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class FakeLib:
        @staticmethod
        def sonare_project_count_in_frames(
            _handle: object, _count_in_bars: int, out_frames: object
        ) -> int:
            ctypes.cast(out_frames, ctypes.POINTER(ctypes.c_int64)).contents.value = -1
            return int(ErrorCode.OK)

    class FakeProject(_ProjectRenderMixin):
        @staticmethod
        def _require_handle() -> ctypes.c_void_p:
            return ctypes.c_void_p(1)

    monkeypatch.setattr(_project_render, "_get_lib", lambda: FakeLib())
    with pytest.raises(SonareError, match=r"frames must be non-negative") as malformed:
        FakeProject().count_in_frames(1)
    assert malformed.value.code == ErrorCode.INVALID_FORMAT


def test_bounce_refuses_binding_shape_before_scalar_domains_without_loading_library(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        _project_render,
        "_get_lib",
        lambda: pytest.fail("shape refusal must happen before loading the library"),
    )
    with pytest.raises(
        TypeError,
        match=r"sf2_instruments\[0\] must be a ProjectSf2InstrumentBinding",
    ):
        _ProjectRenderMixin.bounce(
            object(),
            count_in_bars=17,
            total_frames=0,
            sf2_instruments=[(0, Sf2InstrumentConfig())],  # type: ignore[arg-type]
        )


def test_external_instrument_is_exported() -> None:
    import libsonare

    assert libsonare.ExternalInstrument is ExternalInstrument


def test_bounce_without_instrument_keywords_is_the_plain_bounce() -> None:
    """Every instrument keyword absent, None or empty renders the plain call's silence."""
    project = _build_midi_only_project()
    try:
        kwargs = {"total_frames": 4800, "block_size": 128, "num_channels": 2}
        plain = project.bounce(**kwargs).channels
        assert float(np.max(np.abs(plain))) == 0.0
        assert np.array_equal(
            project.bounce(instrument_latency_samples=None, **kwargs).channels, plain
        )
        for empty in (
            {},
            {"instruments": None},
            {"builtin_instruments": None},
            {"synth_instruments": None},
            {"sf2_instruments": None},
            {"instruments": [], "builtin_instruments": [], "synth_instruments": []},
            {"sf2_instruments": []},
        ):
            assert np.array_equal(project.bounce(**empty, **kwargs).channels, plain)
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
                    ls.midi_note_on(0.0, 0, 0, note, 100),
                    ls.midi_note_off(0.5, 0, 0, note, 0),
                ],
            )
        kwargs = {"total_frames": 12000, "block_size": 128, "num_channels": 2}
        builtin_only = project.bounce(
            builtin_instruments=[
                ProjectInstrumentBinding(instrument=BuiltinSynthConfig(), destination_id=1)
            ],
            **kwargs,
        ).channels
        external = _ConstantInstrument(level=0.25)
        both = project.bounce(
            instruments=[ProjectInstrumentBinding(instrument=external)],
            builtin_instruments=[
                ProjectInstrumentBinding(instrument=BuiltinSynthConfig(), destination_id=1)
            ],
            **kwargs,
        ).channels
        assert external.render_calls > 0
        assert float(np.max(np.abs(both - builtin_only))) > 0.1
    finally:
        project.close()


def test_bounce_refuses_a_destination_bound_twice() -> None:
    project = _build_midi_only_project()
    try:
        with pytest.raises(SonareError):
            project.bounce(
                builtin_instruments=[
                    ProjectInstrumentBinding(instrument=BuiltinSynthConfig()),
                    ProjectInstrumentBinding(instrument=BuiltinSynthConfig()),
                ],
                total_frames=4800,
            )
        with pytest.raises(SonareError):
            project.bounce(
                instruments=[ProjectInstrumentBinding(instrument=_ConstantInstrument())],
                builtin_instruments=[ProjectInstrumentBinding(instrument=BuiltinSynthConfig())],
                total_frames=4800,
            )
    finally:
        project.close()


def test_bounce_auto_derives_length_when_total_frames_omitted() -> None:
    """bounce() with total_frames omitted no longer renders empty (C auto-derives)."""
    project, *_ = _build_project()
    try:
        audio = project.bounce(num_channels=2).channels
        assert audio.ndim == 2
        assert audio.shape[0] > 0
    finally:
        project.close()


def test_bounce_count_in_queries_length_and_shifts_the_arrangement() -> None:
    project = _build_midi_only_project()
    try:
        assert project.count_in_frames(0) == 0
        assert project.count_in_frames(1) == 96000
        assert project.count_in_frames(16) == 16 * 96000
        with pytest.raises(TypeError, match="count_in_bars"):
            project.count_in_frames(True)  # type: ignore[arg-type]
        with pytest.raises(SonareValueError, match="count_in_bars"):
            project.count_in_frames(-1)
        for invalid in (1.5, 2**100, -(2**100)):
            with pytest.raises(
                SonareValueError,
                match=r"count_in_bars must be an integer in \[0, 16\]",
            ):
                project.count_in_frames(invalid)

        without = project.bounce(
            builtin_instruments=[ProjectInstrumentBinding(instrument=BuiltinSynthConfig())],
            total_frames=4096,
            num_channels=2,
        ).channels
        with_prefix = project.bounce(
            builtin_instruments=[ProjectInstrumentBinding(instrument=BuiltinSynthConfig())],
            count_in_bars=1,
            total_frames=96000 + 4096,
            num_channels=2,
        ).channels
        assert with_prefix.shape == (2, 96000 + 4096)
        assert np.count_nonzero(with_prefix[:, :96000]) == 0
        assert np.array_equal(with_prefix[:, 96000:], without)
        short = project.bounce(
            builtin_instruments=[ProjectInstrumentBinding(instrument=BuiltinSynthConfig())],
            count_in_bars=1,
            total_frames=128,
            num_channels=2,
        ).channels
        assert short.shape == (2, 128)
        assert np.count_nonzero(short) == 0

        omitted = project.bounce(
            count_in_bars=1, total_frames=96000 + 4096, num_channels=2
        ).channels
        disabled = project.bounce(
            metronome=EngineMetronomeConfig(enabled=False),
            count_in_bars=1,
            total_frames=96000 + 4096,
            num_channels=2,
        ).channels
        assert np.array_equal(disabled, omitted)
        omitted_without_count = project.bounce(total_frames=4096, num_channels=2).channels
        disabled_without_count = project.bounce(
            metronome=EngineMetronomeConfig(enabled=False),
            total_frames=4096,
            num_channels=2,
        ).channels
        assert np.array_equal(disabled_without_count, omitted_without_count)

        clicked = project.bounce(
            metronome=EngineMetronomeConfig(enabled=True),
            count_in_bars=1,
            total_frames=96000 + 128,
            num_channels=2,
        ).channels
        assert np.count_nonzero(clicked[:, :128]) > 0
        assert np.count_nonzero(clicked[:, 96000:]) > 0
    finally:
        project.close()


def test_bounce_control_binding_records_are_frozen_and_keyword_only() -> None:
    profile = ProjectControllerProfileBinding(destination_id=0, preset_name="gm")
    articulation = ProjectArticulationBinding(destination_id=0, channel=1, articulation="poly")
    assert ls.types.ProjectControllerProfileBinding is type(profile)
    assert ls.types.ProjectArticulationBinding is type(articulation)
    with pytest.raises(dataclasses.FrozenInstanceError):
        profile.preset_name = "other"  # type: ignore[misc]
    with pytest.raises(dataclasses.FrozenInstanceError):
        articulation.channel = 2  # type: ignore[misc]
    with pytest.raises(TypeError):
        ProjectControllerProfileBinding(0, "gm")  # type: ignore[misc]
    with pytest.raises(TypeError):
        ProjectArticulationBinding(destination_id=0, articulation="poly")  # type: ignore[call-arg]


@pytest.mark.parametrize(
    ("options", "subject"),
    [
        ({"controller_profiles": [(0, "gm")]}, r"controller_profiles\[0\]"),
        ({"articulations": [(0, 0, "poly")]}, r"articulations\[0\]"),
        (
            {
                "controller_profiles": [
                    ProjectControllerProfileBinding(destination_id=-1, preset_name=5)
                ]
            },
            r"controller_profiles\[0\]\.preset_name",
        ),
        (
            {
                "articulations": [
                    ProjectArticulationBinding(destination_id=0, channel=16, articulation=3)
                ]
            },
            r"articulations\[0\]\.articulation",
        ),
    ],
)
def test_bounce_control_binding_shapes_precede_domains(options, subject: str) -> None:
    project = Project.__new__(Project)
    with pytest.raises(TypeError, match=subject):
        project.bounce(count_in_bars=17, **options)


def test_bounce_control_binding_records_marshal_named_fields() -> None:
    profiles, names, articulations = _project_render._bounce_bindings(
        [ProjectControllerProfileBinding(destination_id=7, preset_name="gm")],
        [ProjectArticulationBinding(destination_id=11, channel=3, articulation="monoLegato")],
    )
    assert profiles is not None and articulations is not None
    assert (profiles[0].destination_id, profiles[0].preset_name) == (7, b"gm")
    assert names == [b"gm"]
    assert (articulations[0].destination_id, articulations[0].channel) == (11, 3)
    assert articulations[0].articulation == 2


def test_bounce_new_options_refuse_values_by_keyword() -> None:
    project = _build_midi_only_project()
    try:
        with pytest.raises(TypeError, match="metronome"):
            project.bounce(metronome=True)  # type: ignore[arg-type]
        with pytest.raises(SonareValueError, match="count_in_bars"):
            project.bounce(count_in_bars=17)
        with pytest.raises(
            TypeError,
            match=r"sf2_instruments\[0\] must be a ProjectSf2InstrumentBinding",
        ):
            project.bounce(
                count_in_bars=17,
                total_frames=0,
                sf2_instruments=[(0, Sf2InstrumentConfig())],  # type: ignore[list-item]
            )
        with pytest.raises(
            TypeError, match=r"controller_profiles\[0\] must be a ProjectControllerProfileBinding"
        ):
            project.bounce(controller_profiles=[(0,)])  # type: ignore[list-item]
        with pytest.raises(SonareValueError, match=r"controller_profiles\[0\]\.preset_name"):
            project.bounce(
                controller_profiles=[
                    ProjectControllerProfileBinding(destination_id=0, preset_name="")
                ]
            )
        with pytest.raises(SonareValueError, match=r"controller_profiles\[0\]\.preset_name"):
            project.bounce(
                synth_instruments=[
                    ProjectInstrumentBinding(instrument=SynthPatch(preset="sawLead"))
                ],
                controller_profiles=[
                    ProjectControllerProfileBinding(destination_id=0, preset_name="unknown-profile")
                ],
                total_frames=128,
            )
        with pytest.raises(
            TypeError, match=r"articulations\[0\] must be a ProjectArticulationBinding"
        ):
            project.bounce(articulations=[(0, 0)])  # type: ignore[list-item]
        with pytest.raises(SonareValueError, match=r"articulations\[0\]\.channel"):
            project.bounce(
                articulations=[
                    ProjectArticulationBinding(destination_id=0, channel=16, articulation="poly")
                ]
            )
        with pytest.raises(SonareValueError, match=r"articulations\[0\]\.articulation"):
            project.bounce(
                articulations=[
                    ProjectArticulationBinding(
                        destination_id=0,
                        channel=0,
                        articulation="unknown",  # type: ignore[arg-type]
                    )
                ]
            )
        with pytest.raises(SonareValueError, match=r"metronome\.click_samples"):
            project.bounce(metronome=EngineMetronomeConfig(click_samples=0))
        with pytest.raises(SonareValueError, match="mutually exclusive"):
            project.bounce(metronome=EngineMetronomeConfig(click_samples=10, click_sec=0.01))
        with pytest.raises(SonareValueError, match=r"controller_profiles\[1\]\.destination_id"):
            project.bounce(
                synth_instruments=[
                    ProjectInstrumentBinding(instrument=SynthPatch(preset="sawLead"))
                ],
                controller_profiles=[
                    ProjectControllerProfileBinding(destination_id=0, preset_name="gm")
                ]
                * 2,
                total_frames=128,
            )
        with pytest.raises(
            SonareValueError,
            match=r"articulations\[1\]\.destination_id.*articulations\[1\]\.channel",
        ):
            project.bounce(
                synth_instruments=[
                    ProjectInstrumentBinding(instrument=SynthPatch(preset="sawLead"))
                ],
                articulations=[
                    ProjectArticulationBinding(destination_id=0, channel=0, articulation="poly"),
                    ProjectArticulationBinding(
                        destination_id=0, channel=0, articulation="monoLegato"
                    ),
                ],
                total_frames=128,
            )

        # Both control lists reach the native synth when bound to its destination.
        rendered = project.bounce(
            synth_instruments=[ProjectInstrumentBinding(instrument=SynthPatch(preset="sawLead"))],
            controller_profiles=[
                ProjectControllerProfileBinding(destination_id=0, preset_name="gm")
            ],
            articulations=[
                ProjectArticulationBinding(destination_id=0, channel=0, articulation="poly")
            ],
            total_frames=128,
            num_channels=2,
        )
        assert rendered.channels.shape == (2, 128)
        empty_bindings = project.bounce(
            synth_instruments=[ProjectInstrumentBinding(instrument=SynthPatch(preset="sawLead"))],
            controller_profiles=[],
            articulations=[],
            total_frames=128,
            num_channels=2,
        )
        plain_bindings = project.bounce(
            synth_instruments=[ProjectInstrumentBinding(instrument=SynthPatch(preset="sawLead"))],
            total_frames=128,
            num_channels=2,
        )
        assert np.array_equal(empty_bindings.channels, plain_bindings.channels)
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
        project.bounce(total_frames=480, num_channels=2)
        result = project.last_bounce_compile_result()
        assert isinstance(result, ProjectCompileResult)
        assert result.diagnostics == ()
    finally:
        project.close()


def test_a_failed_bounce_refuses_and_leaves_the_compile_result_empty() -> None:
    project = Project()
    try:
        # One track more than the realtime track mixer holds is a compile error.
        for index in range(33):
            project.add_track("audio", f"t{index}")
        with raises_coded(ErrorCode.INVALID_STATE, match=r"the project does not compile: "):
            project.bounce(total_frames=480, num_channels=2)
        assert project.last_bounce_compile_result() is None
    finally:
        project.close()
