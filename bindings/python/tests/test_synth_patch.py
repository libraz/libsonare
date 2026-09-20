"""NativeSynth binding tests: synth_preset_names / synth_preset_patch,
Project.bounce_with_synth_instrument and RealtimeEngine.set_synth_instrument."""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    Project,
    RealtimeEngine,
    SonareError,
    SynthModRouting,
    SynthPatch,
    synth_enum_tables,
    synth_preset_names,
    synth_preset_patch,
)
from libsonare._project import SYNTH_ENUM_TABLES

EXPECTED_SYNTH_ENUM_TABLES = {
    "engine_modes": (
        "default",
        "subtractive",
        "fm",
        "karplus-strong",
        "modal",
        "additive",
        "percussion",
        "piano",
        "pipe-organ",
        "bowed-string",
        "reed",
        "brass",
        "flute",
        "plucked-string",
        "vocal",
        "free-reed",
        "harpsichord",
        "sample",
    ),
    "waveforms": ("default", "sine", "saw", "square", "triangle", "noise"),
    "builtin_waveforms": ("sine", "saw", "sawtooth", "square", "triangle"),
    "filter_models": ("default", "svf", "moog-ladder", "diode-ladder", "sallen-key"),
    "filter_outputs": ("default", "lowpass", "bandpass", "highpass"),
    "body_types": ("default", "none", "guitar", "violin", "wood-tube", "brass-bell", "vocal"),
    "mod_sources": (
        "none",
        "amp-env",
        "filter-env",
        "lfo1",
        "lfo2",
        "velocity",
        "key-track",
        "mod-wheel",
        "random",
        "breath",
        "aftertouch",
        "expression-cc",
        "pitch-bend",
    ),
    "mod_destinations": (
        "none",
        "pitch-cents",
        "cutoff-cents",
        "amp-gain",
        "pan-units",
        "resonance-q",
        "vibrato-depth-cents",
        "filter-env-depth",
        "lfo1-rate-scale",
        "excitation-force",
        "excitation-position",
        "excitation-brightness",
        "spectrum-morph",
    ),
    "controller_inputs": (
        "control-change",
        "channel-pressure",
        "poly-pressure",
        "pitch-bend",
        "velocity",
    ),
    "controller_axes": (
        "none",
        "excitation",
        "position",
        "brightness",
        "morph",
        "loudness",
        "pitch-cents",
        "vibrato-depth",
    ),
    "articulations": (
        "poly",
        "mono-retrigger",
        "mono-legato",
    ),
    "mpe_dimensions": (
        "bend",
        "pressure",
        "timbre",
    ),
    "note_trackings": (
        "last",
        "lowest",
        "highest",
        "all",
    ),
}


def _build_midi_only_project(note: int = 60) -> Project:
    project = Project()
    project.set_sample_rate(48000.0)
    track, clip = project.add_midi_clip(0.0, 4.0)
    project.set_track_midi_destination(track, 0)
    project.set_midi_events(
        clip,
        [
            Project.midi_note_on(0.0, 0, 0, note, 100),
            Project.midi_note_off(2.0, 0, 0, note, 0),
        ],
    )
    return project


def _build_gm_program_project(program: int) -> Project:
    project = Project()
    project.set_sample_rate(48000.0)
    track, clip = project.add_midi_clip(0.0, 1.0)
    project.set_track_midi_destination(track, 0)
    project.set_midi_events(
        clip,
        [
            Project.midi_program(0.0, 0, 0, program),
            Project.midi_note_on(0.0, 0, 0, 60, 100),
            Project.midi_note_off(0.5, 0, 0, 60, 0),
        ],
    )
    return project


def test_synth_preset_names_lists_the_catalog() -> None:
    names = synth_preset_names()
    for expected in (
        "sine",
        "saw-lead",
        "warm-pad",
        "e-piano",
        "electric-guitar",
        "harp",
        "marimba",
        "organ",
        "drum-kit",
        "acoustic-piano",
    ):
        assert expected in names


def test_synth_preset_patch_round_trips() -> None:
    pad = synth_preset_patch("warm-pad")
    assert pad.preset == "warm-pad"
    assert pad.engine_mode == "subtractive"
    assert pad.waveform == "saw"
    assert pad.unison == 7
    assert pad.stereo_spread > 0.0
    # The "va:" routing prefix is accepted.
    assert synth_preset_patch("va:e-piano").engine_mode == "fm"
    assert synth_preset_patch("acoustic-piano").engine_mode == "piano"
    assert synth_preset_patch("clarinet").engine_mode == "reed"
    with pytest.raises(SonareError):
        synth_preset_patch("no-such-preset")


def test_synth_patch_enum_tables_round_trip_all_names_and_ordinals() -> None:
    assert SYNTH_ENUM_TABLES == EXPECTED_SYNTH_ENUM_TABLES
    assert synth_enum_tables() == EXPECTED_SYNTH_ENUM_TABLES

    for ordinal, name in enumerate(EXPECTED_SYNTH_ENUM_TABLES["engine_modes"]):
        by_name = SynthPatch._from_c(SynthPatch(engine_mode=name)._to_c())
        by_ordinal = SynthPatch._from_c(SynthPatch(engine_mode=ordinal)._to_c())
        assert by_name.engine_mode == name
        assert by_ordinal.engine_mode == name
    for ordinal, name in enumerate(EXPECTED_SYNTH_ENUM_TABLES["waveforms"]):
        by_name = SynthPatch._from_c(SynthPatch(waveform=name)._to_c())
        by_ordinal = SynthPatch._from_c(SynthPatch(waveform=ordinal)._to_c())
        assert by_name.waveform == name
        assert by_ordinal.waveform == name
    for ordinal, name in enumerate(EXPECTED_SYNTH_ENUM_TABLES["filter_models"]):
        by_name = SynthPatch._from_c(SynthPatch(filter_model=name)._to_c())
        by_ordinal = SynthPatch._from_c(SynthPatch(filter_model=ordinal)._to_c())
        assert by_name.filter_model == name
        assert by_ordinal.filter_model == name
    for ordinal, name in enumerate(EXPECTED_SYNTH_ENUM_TABLES["filter_outputs"]):
        by_name = SynthPatch._from_c(SynthPatch(filter_output=name)._to_c())
        by_ordinal = SynthPatch._from_c(SynthPatch(filter_output=ordinal)._to_c())
        assert by_name.filter_output == name
        assert by_ordinal.filter_output == name
    for ordinal, name in enumerate(EXPECTED_SYNTH_ENUM_TABLES["body_types"]):
        by_name = SynthPatch._from_c(SynthPatch(body=name)._to_c())
        by_ordinal = SynthPatch._from_c(SynthPatch(body=ordinal)._to_c())
        assert by_name.body == name
        assert by_ordinal.body == name
    for ordinal, name in enumerate(EXPECTED_SYNTH_ENUM_TABLES["mod_sources"]):
        by_name = SynthPatch._from_c(
            SynthPatch(mod_routings=(SynthModRouting(name, "pitch-cents", 1.0),))._to_c()
        )
        by_ordinal = SynthPatch._from_c(
            SynthPatch(mod_routings=(SynthModRouting(ordinal, "pitch-cents", 1.0),))._to_c()
        )
        assert by_name.mod_routings[0].source == name
        assert by_ordinal.mod_routings[0].source == name
    for ordinal, name in enumerate(EXPECTED_SYNTH_ENUM_TABLES["mod_destinations"]):
        by_name = SynthPatch._from_c(
            SynthPatch(mod_routings=(SynthModRouting("lfo1", name, 1.0),))._to_c()
        )
        by_ordinal = SynthPatch._from_c(
            SynthPatch(mod_routings=(SynthModRouting("lfo1", ordinal, 1.0),))._to_c()
        )
        assert by_name.mod_routings[0].destination == name
        assert by_ordinal.mod_routings[0].destination == name


def test_synth_patch_rejects_non_string_preset() -> None:
    with pytest.raises(TypeError, match="synth patch preset must be a string"):
        SynthPatch(preset=123)._to_c()  # type: ignore[arg-type]


def test_bounce_with_synth_instrument_renders_presets() -> None:
    project = _build_midi_only_project()
    try:
        for preset in ("va:saw-lead", "e-piano", "harp"):
            audio = project.bounce_with_synth_instrument(preset, total_frames=24000)
            assert audio.shape == (24000, 2)
            assert float(np.max(np.abs(audio))) > 0.0
        # Deterministic: bit-identical renders for a fixed patch.
        first = project.bounce_with_synth_instrument("saw-lead", total_frames=24000)
        second = project.bounce_with_synth_instrument("saw-lead", total_frames=24000)
        assert np.array_equal(first, second)
    finally:
        project.close()


def test_synth_patch_overrides_and_mod_matrix() -> None:
    project = _build_midi_only_project()
    try:
        plain = project.bounce_with_synth_instrument(total_frames=24000)
        assert float(np.max(np.abs(plain))) > 0.0
        dark = project.bounce_with_synth_instrument(
            SynthPatch(cutoff_hz=300.0, resonance_q=4.0), total_frames=24000
        )
        assert not np.array_equal(dark, plain)
        wobble = project.bounce_with_synth_instrument(
            SynthPatch(
                lfo_rate_hz=6.0,
                mod_routings=(SynthModRouting("lfo1", "pitch-cents", 80.0),),
            ),
            total_frames=24000,
        )
        assert not np.array_equal(wobble, plain)
        with pytest.raises(SonareError):
            project.bounce_with_synth_instrument("no-such-preset", total_frames=128)
        with pytest.raises(ValueError):
            project.bounce_with_synth_instrument(
                SynthPatch(waveform="sawtooth-ish"), total_frames=128
            )
    finally:
        project.close()


def test_synth_patch_zero_is_an_override_not_keep_the_base() -> None:
    """A field left at None keeps the base; a supplied zero overrides with it."""
    project = _build_midi_only_project()
    try:
        preset = project.bounce_with_synth_instrument("warm-pad", total_frames=24000)

        # warm-pad carries a non-zero stereo spread and bus drive, so turning
        # either off is a real edit that the patch can now express.
        no_spread = project.bounce_with_synth_instrument(
            SynthPatch(preset="warm-pad", stereo_spread=0.0), total_frames=24000
        )
        assert not np.array_equal(no_spread, preset)
        no_drive = project.bounce_with_synth_instrument(
            SynthPatch(preset="warm-pad", bus_drive=0.0), total_frames=24000
        )
        assert not np.array_equal(no_drive, preset)

        # Leaving the fields out keeps the preset's own values.
        untouched = project.bounce_with_synth_instrument(
            SynthPatch(preset="warm-pad"), total_frames=24000
        )
        assert np.array_equal(untouched, preset)

        # An empty routing tuple clears the base matrix; None keeps it.
        wobble = project.bounce_with_synth_instrument(
            SynthPatch(
                preset="warm-pad",
                lfo_rate_hz=6.0,
                mod_routings=(SynthModRouting("lfo1", "pitch-cents", 80.0),),
            ),
            total_frames=24000,
        )
        cleared = project.bounce_with_synth_instrument(
            SynthPatch(preset="warm-pad", lfo_rate_hz=6.0, mod_routings=()),
            total_frames=24000,
        )
        assert not np.array_equal(cleared, wobble)
    finally:
        project.close()


def test_synth_bounce_gm_programs_4_and_40_are_finite_audible_and_distinct() -> None:
    def render(program: int, auto_select_gm: bool) -> np.ndarray:
        project = _build_gm_program_project(program)
        try:
            audio = project.bounce_with_synth_instrument(
                "sine",
                auto_select_gm=auto_select_gm,
                total_frames=12000,
                block_size=128,
                num_channels=1,
                sample_rate=48000,
            )
            assert audio.shape == (12000, 1)
            assert np.isfinite(audio).all()
            assert float(np.max(np.abs(audio))) > 0.0
            return audio.copy()
        finally:
            project.close()

    disabled = render(4, False)
    gm4 = render(4, True)
    gm40 = render(40, True)
    assert float(np.max(np.abs(disabled - gm4))) > 1.0e-6
    assert float(np.max(np.abs(gm4 - gm40))) > 1.0e-6


def _build_two_destination_gm_project(program: int = 40) -> Project:
    """Two MIDI tracks on two destinations, each sending its own GM program."""
    project = Project()
    project.set_sample_rate(48000.0)
    for destination, note in ((0, 60), (1, 72)):
        track, clip = project.add_midi_clip(0.0, 1.0)
        project.set_track_midi_destination(track, destination)
        project.set_midi_events(
            clip,
            [
                Project.midi_program(0.0, 0, 0, program),
                Project.midi_note_on(0.0, 0, 0, note, 100),
                Project.midi_note_off(0.5, 0, 0, note, 0),
            ],
        )
    return project


def _render_two_destinations(
    project: Project,
    *,
    auto_select_gm: bool = False,
    gm: tuple[bool | None, bool | None] = (None, None),
) -> np.ndarray:
    audio = project.bounce_with_synth_instrument(
        instruments=[
            (0, SynthPatch(preset="sine", use_gm_programs=gm[0])),
            (1, SynthPatch(preset="sine", use_gm_programs=gm[1])),
        ],
        auto_select_gm=auto_select_gm,
        total_frames=12000,
        block_size=128,
        num_channels=1,
        sample_rate=48000,
    )
    assert np.isfinite(audio).all()
    assert float(np.max(np.abs(audio))) > 0.0
    return audio.copy()


def test_each_binding_states_its_own_gm_follow() -> None:
    """Two destinations, and only one of them follows its GM program changes.

    ``use_gm_programs`` is a field of the C binding struct, so this is the
    capability a single per-call argument could not express: a mixed render must
    be neither of the two uniform ones.
    """
    project = _build_two_destination_gm_project()
    try:
        both = _render_two_destinations(project, gm=(True, True))
        neither = _render_two_destinations(project, gm=(False, False))
        first_only = _render_two_destinations(project, gm=(True, False))
        second_only = _render_two_destinations(project, gm=(False, True))
        for mixed in (first_only, second_only):
            assert not np.array_equal(mixed, both)
            assert not np.array_equal(mixed, neither)
        # Swapping which destination follows changes the render, so the two
        # values cannot be collapsing into one shared flag.
        assert not np.array_equal(first_only, second_only)
    finally:
        project.close()


def test_auto_select_gm_reaches_every_binding_that_states_nothing() -> None:
    """The released per-call argument still decides for an unstated patch."""
    project = _build_two_destination_gm_project()
    try:
        assert np.array_equal(
            _render_two_destinations(project, auto_select_gm=True),
            _render_two_destinations(project, gm=(True, True)),
        )
        assert np.array_equal(
            _render_two_destinations(project, auto_select_gm=False),
            _render_two_destinations(project, gm=(False, False)),
        )
    finally:
        project.close()


def test_a_stated_patch_field_wins_over_the_per_call_argument() -> None:
    project = _build_two_destination_gm_project()
    try:
        assert np.array_equal(
            _render_two_destinations(project, auto_select_gm=True, gm=(False, False)),
            _render_two_destinations(project, gm=(False, False)),
        )
        assert np.array_equal(
            _render_two_destinations(project, auto_select_gm=False, gm=(True, True)),
            _render_two_destinations(project, gm=(True, True)),
        )
        # The argument is not simply inert: overriding it changes the render it
        # would otherwise have produced.
        assert not np.array_equal(
            _render_two_destinations(project, auto_select_gm=True, gm=(False, False)),
            _render_two_destinations(project, auto_select_gm=True),
        )
    finally:
        project.close()


def test_drum_kit_preset_plays_the_gm_map() -> None:
    # Note 38 = acoustic snare in the GM drum map.
    project = _build_midi_only_project(note=38)
    try:
        audio = project.bounce_with_synth_instrument("drum-kit", total_frames=24000)
        assert float(np.max(np.abs(audio))) > 0.0
    finally:
        project.close()


def test_engine_set_synth_instrument_renders_live_midi() -> None:
    engine = RealtimeEngine()
    try:
        engine.prepare(48000.0, 128, 16, 16)
        engine.set_synth_instrument("saw-lead", destination_id=7)
        engine.push_midi_note_on(7, 0, 0, 60, 100)
        out = np.asarray(engine.process([[0.0] * 128, [0.0] * 128]))
        assert float(np.max(np.abs(out))) > 0.0
        # Unknown presets are rejected without disturbing the binding.
        with pytest.raises(SonareError):
            engine.set_synth_instrument("no-such-preset", destination_id=7)
    finally:
        engine.close()
