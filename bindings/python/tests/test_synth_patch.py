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
    synth_preset_names,
    synth_preset_patch,
)


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
    with pytest.raises(SonareError):
        synth_preset_patch("no-such-preset")


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
