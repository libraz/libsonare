"""SoundFont (SF2) binding tests: Project.load_soundfont / soundfont_manifest /
bounce_with_sf2_instrument and the realtime engine SF2 instrument entry."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from libsonare import (
    Project,
    RealtimeEngine,
    Sf2InstrumentConfig,
    Sf2ProgramStatus,
    SonareError,
)
from libsonare._project import SOURCE_BACKEND_SF2, SOURCE_BACKEND_SYNTH

# Canonical minimal GS test SoundFont (presets: "Piano 1" at (0,0),
# "Piano 2" at (0,1), "Standard Kit" at (128,0); program 2 uncovered).
_FIXTURE = Path(__file__).resolve().parents[3] / "tests" / "fixtures" / "sf2" / "minimal_gs.sf2"
_SF2_BYTES = _FIXTURE.read_bytes()


def _build_midi_only_project() -> Project:
    project = Project()
    project.set_sample_rate(48000.0)
    track, clip = project.add_midi_clip(0.0, 4.0)
    project.set_track_midi_destination(track, 0)
    project.set_midi_events(
        clip,
        [
            Project.midi_note_on(0.0, 0, 0, 60, 100),
            Project.midi_note_off(2.0, 0, 0, 60, 0),
        ],
    )
    return project


def test_load_soundfont_counts_presets_and_clears() -> None:
    project = Project()
    try:
        assert project.soundfont_preset_count() == 0
        project.load_soundfont(_SF2_BYTES)
        assert project.soundfont_preset_count() == 3
        project.clear_soundfont()
        assert project.soundfont_preset_count() == 0
    finally:
        project.close()


def test_load_soundfont_rejects_malformed_bytes() -> None:
    project = Project()
    try:
        project.load_soundfont(_SF2_BYTES)
        with pytest.raises(SonareError):
            project.load_soundfont(b"not an sf2 file")
        # The previous SoundFont is kept after a failed load.
        assert project.soundfont_preset_count() == 3
        with pytest.raises(ValueError):
            project.load_soundfont(b"")
    finally:
        project.close()


def test_soundfont_manifest_reports_backends() -> None:
    project = _build_midi_only_project()
    try:
        # Without a SoundFont the played program is a synth fallback.
        manifest = project.soundfont_manifest()
        assert manifest == [
            Sf2ProgramStatus(
                channel=0, bank=0, program=0, backend=SOURCE_BACKEND_SYNTH, preset_name=""
            )
        ]
        project.load_soundfont(_SF2_BYTES)
        manifest = project.soundfont_manifest()
        assert manifest == [
            Sf2ProgramStatus(
                channel=0, bank=0, program=0, backend=SOURCE_BACKEND_SF2, preset_name="Piano 1"
            )
        ]
    finally:
        project.close()


def test_bounce_with_sf2_instrument_produces_deterministic_audio() -> None:
    project = _build_midi_only_project()
    try:
        # Without a loaded SoundFont the bounce still sounds: the built-in
        # synthesizer GM fallback is the data-free floor.
        fallback = project.bounce_with_sf2_instrument(
            total_frames=4096, block_size=128, num_channels=2, sample_rate=48000
        )
        assert float(np.max(np.abs(fallback))) > 0.01

        project.load_soundfont(_SF2_BYTES)
        audio = project.bounce_with_sf2_instrument(
            Sf2InstrumentConfig(gain=1.0),
            total_frames=4096,
            block_size=128,
            num_channels=2,
            sample_rate=48000,
        )
        assert audio.shape == (4096, 2)
        assert audio.dtype == np.float32
        assert float(np.max(np.abs(audio))) > 0.01

        again = project.bounce_with_sf2_instrument(
            Sf2InstrumentConfig(gain=1.0),
            total_frames=4096,
            block_size=128,
            num_channels=2,
            sample_rate=48000,
        )
        assert np.array_equal(audio, again)

        # An explicitly empty bindings list renders silence.
        silent = project.bounce_with_sf2_instrument(
            instruments=[],
            total_frames=2048,
            block_size=128,
            num_channels=2,
            sample_rate=48000,
        )
        assert float(np.max(np.abs(silent))) == 0.0
    finally:
        project.close()


def test_engine_sf2_instrument_renders_live_midi() -> None:
    engine = RealtimeEngine(48000.0, 128)
    try:
        # Binding before a SoundFont is loaded is allowed: live MIDI plays
        # through the built-in synthesizer GM fallback (the data-free floor).
        engine.set_sf2_instrument(destination_id=7)
        engine.push_midi_note_on(7, 0, 0, 60, 100)
        out = engine.process([[0.0] * 128, [0.0] * 128])
        assert max(max(abs(s) for s in ch) for ch in out) > 0.0
        engine.clear_midi_instrument(7)

        with pytest.raises(SonareError):
            engine.load_soundfont(b"bad!")

        engine.load_soundfont(_SF2_BYTES)
        engine.set_sf2_instrument(Sf2InstrumentConfig(gain=1.0), destination_id=7)
        assert engine.midi_instrument_count() == 1

        engine.push_midi_note_on(7, 0, 0, 60, 100)
        out = engine.process([[0.0] * 128, [0.0] * 128])
        peak = max(max(abs(s) for s in ch) for ch in out)
        assert peak > 0.0

        engine.clear_midi_instrument(7)
        assert engine.midi_instrument_count() == 0
    finally:
        engine.close()
