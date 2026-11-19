"""An optional count, rate or id is ``None`` when absent, and an explicit 0 is refused.

Each field below reaches a C slot whose 0 means "not given". The cases pair the
omitted spelling with a given value that changes the outcome, and refuse 0 by
name, so the test fails if 0 drifts back to meaning absence.
"""

from __future__ import annotations

import math
from typing import Any

import numpy as np
import pytest

import libsonare
from libsonare import (
    EngineBounceOptions,
    EngineClip,
    EngineMetronomeConfig,
    EngineMidiClipSchedule,
    Project,
    RealtimeEngine,
    SonareValueError,
)

SR = 48000


def test_move_clip_keeps_the_track_when_none_and_refuses_zero() -> None:
    with Project() as project:
        first = project.add_track("audio", "a")
        second = project.add_track("audio", "b")
        clip = project.add_clip(
            first, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_channels=1, audio_sample_rate=SR
        )

        def track_of() -> int:
            return {c.id: c.track_id for c in project.clips()}[clip]

        project.move_clip(clip, 240.0)
        assert track_of() == first
        project.move_clip(clip, 240.0, second)
        assert track_of() == second
        with pytest.raises(
            SonareValueError,
            match=r"^Project\.move_clip: new_track_id must be an integer (?:in|within) \[1, ",
        ):
            project.move_clip(clip, 240.0, 0)


def test_metronome_click_length_reads_back_none_when_omitted() -> None:
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.set_metronome(EngineMetronomeConfig(enabled=True))
        assert engine.metronome().click_samples is None
        engine.set_metronome(EngineMetronomeConfig(enabled=True, click_samples=64))
        assert engine.metronome().click_samples == 64


def test_engine_clip_track_is_none_when_unset_and_zero_is_refused() -> None:
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        clip = EngineClip(id=1, channels=[[0.5] * 128], start_ppq=0.0)
        assert clip.track_id is None
        engine.set_clips([clip])
        with pytest.raises(
            SonareValueError,
            match=r"^RealtimeEngine\.set_clips: clip\.track_id must be an integer in \[1, ",
        ):
            engine.set_clips([EngineClip(id=1, channels=[[0.5] * 128], start_ppq=0.0, track_id=0)])


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("id", 0),
        ("track_id", 0),
        ("length_samples", 0),
        ("length_samples", -1),
        ("loop_length_samples", 0),
        ("loop_length_samples", -1),
    ],
)
def test_midi_schedule_refuses_zero_where_none_is_absence(field: str, value: int) -> None:
    with (
        RealtimeEngine(sample_rate=SR, max_block_size=128) as engine,
        pytest.raises(
            SonareValueError,
            match=rf"^RealtimeEngine\.set_midi_clips: clip\.{field} must be an integer in \[1, ",
        ),
    ):
        given: dict[str, Any] = {field: value}
        engine.set_midi_clips([EngineMidiClipSchedule(events=[], **given)])


def test_midi_schedule_omitted_fields_are_accepted() -> None:
    schedule = EngineMidiClipSchedule(events=[])
    assert (schedule.id, schedule.track_id, schedule.length_samples) == (None, None, None)
    assert schedule.loop_length_samples is None
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.set_midi_clips([schedule])
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    events=[], id=3, length_samples=512, loop=True, loop_length_samples=256
                )
            ]
        )


def test_bounce_rates_are_none_when_omitted_and_zero_is_refused() -> None:
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.play()
        kept = engine.bounce_offline(EngineBounceOptions(total_frames=128, num_channels=2))
        assert kept.sample_rate == SR
        halved = engine.bounce_offline(
            EngineBounceOptions(total_frames=128, num_channels=2, target_sample_rate=SR // 2)
        )
        assert halved.sample_rate == SR // 2
        for field in ("target_sample_rate", "source_sample_rate"):
            zero: dict[str, Any] = {field: 0}
            with pytest.raises(
                SonareValueError,
                match=rf"^RealtimeEngine\.bounce_offline: options\.{field} must be an integer",
            ):
                engine.bounce_offline(EngineBounceOptions(total_frames=128, num_channels=2, **zero))


def _tone(n: int = SR // 4) -> np.ndarray:
    t = np.arange(n, dtype=np.float32) / SR
    return (0.3 * np.sin(2.0 * np.pi * 440.0 * t)).astype(np.float32)


def test_onset_threshold_is_none_for_the_adaptive_path_and_zero_is_refused() -> None:
    tone = _tone()
    assert libsonare.detect_onsets(tone, SR).dtype == np.float32
    with pytest.raises(
        SonareValueError,
        match="^detect_onsets: threshold must not be 0; omit it for the adaptive threshold$",
    ):
        libsonare.detect_onsets(tone, SR, threshold=0.0)


@pytest.mark.parametrize("high_hz", [0.0, -1.0, math.nan, math.inf])
def test_spectral_high_edge_is_none_for_nyquist_and_non_positive_is_refused(
    high_hz: float,
) -> None:
    tone = _tone()
    whole = libsonare.SpectralRegionOp(low_hz=300.0, gain_db=-12.0)
    assert len(libsonare.spectral_edit(tone, SR, [whole])) == len(tone)
    with pytest.raises(
        SonareValueError,
        match=(
            r"^spectral_edit: ops\[1\]\.high_hz must be finite and greater than 0; "
            r"omit it for Nyquist$"
        ),
    ):
        libsonare.spectral_edit(
            tone, SR, [whole, libsonare.SpectralRegionOp(low_hz=300.0, high_hz=high_hz)]
        )


@pytest.mark.parametrize("field", ["low_hz", "gain_db"])
def test_spectral_region_refusals_name_the_op_index(field: str) -> None:
    nan: dict[str, Any] = {field: math.nan}
    ops = [libsonare.SpectralRegionOp(), libsonare.SpectralRegionOp(**nan)]
    with pytest.raises(
        SonareValueError, match=rf"^spectral_edit: ops\[1\]\.{field} must be a finite number$"
    ):
        libsonare.spectral_edit(_tone(), SR, ops)


def test_metronome_click_seconds_omitted_is_the_library_length_and_zero_is_refused() -> None:
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.set_metronome(EngineMetronomeConfig(enabled=True))
        # The core stores the length it resolved, so an omitted one reads back positive.
        resolved = engine.metronome().click_sec
        assert resolved is not None and resolved > 0
        engine.set_metronome(EngineMetronomeConfig(enabled=True, click_sec=0.004))
        assert engine.metronome().click_sec == pytest.approx(0.004)
        with pytest.raises(
            SonareValueError, match="^RealtimeEngine.set_metronome: click_sec must not be 0"
        ):
            engine.set_metronome(EngineMetronomeConfig(enabled=True, click_sec=0.0))


def test_sample_source_rate_is_none_for_the_render_rate_and_zero_is_refused() -> None:
    with libsonare.SampleBank() as bank:
        assert bank.add_sample(_tone(4800)) == 0
        with pytest.raises(
            SonareValueError,
            match="^SampleBank.add_sample: source_rate must not be 0; "
            "omit it for the render's own rate$",
        ):
            bank.add_sample(_tone(4800), source_rate=0.0)


@pytest.mark.parametrize(
    ("config", "field"),
    [
        ("BuiltinSynthConfig", "gain"),
        ("BuiltinSynthConfig", "attack_ms"),
        ("BuiltinSynthConfig", "decay_ms"),
        ("BuiltinSynthConfig", "sustain"),
        ("BuiltinSynthConfig", "release_ms"),
        ("Sf2InstrumentConfig", "gain"),
    ],
)
def test_instrument_zero_level_or_time_is_refused_until_it_can_be_expressed(
    config: str, field: str
) -> None:
    cls = getattr(libsonare, config)
    binder = "set_builtin_instrument" if config == "BuiltinSynthConfig" else "set_sf2_instrument"
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        getattr(engine, binder)(cls(), 6)
        with pytest.raises(
            SonareValueError,
            match=rf"^RealtimeEngine\.{binder}: {field} must not be 0; "
            "omit it for the library value$",
        ):
            getattr(engine, binder)(cls(**{field: 0.0}), 6)


def test_bounce_dither_is_a_token_and_none_renders_undithered() -> None:
    with RealtimeEngine(sample_rate=SR, max_block_size=128) as engine:
        engine.play()
        for dither in (None, "rpdf", "tpdf", "noiseShaped"):
            engine.bounce_offline(
                EngineBounceOptions(total_frames=128, num_channels=2, dither=dither)
            )
        with pytest.raises(TypeError, match="dither"):
            engine.bounce_offline(
                EngineBounceOptions(total_frames=128, num_channels=2, dither=1)  # type: ignore[arg-type]
            )
        with pytest.raises(SonareValueError, match="dither"):
            engine.bounce_offline(
                EngineBounceOptions(total_frames=128, num_channels=2, dither="none")  # type: ignore[arg-type]
            )


def test_an_omitted_effect_hop_is_a_quarter_of_the_given_n_fft() -> None:
    tone = _tone()
    omitted = libsonare.hpss(tone, SR, n_fft=1024)
    quarter = libsonare.hpss(tone, SR, n_fft=1024, hop_length=256)
    np.testing.assert_array_equal(omitted.harmonic, quarter.harmonic)
    np.testing.assert_array_equal(omitted.percussive, quarter.percussive)
    with pytest.raises(SonareValueError, match=r"^hpss: hop_length must be an integer in \[1, "):
        libsonare.hpss(tone, SR, n_fft=1024, hop_length=0)


@pytest.mark.parametrize(
    "field", ["n_fft", "hop_length", "hpss_kernel_harmonic", "hpss_kernel_percussive"]
)
def test_percussive_separation_refuses_zero_where_none_is_the_default(field: str) -> None:
    tone = _tone()
    with pytest.raises(
        SonareValueError,
        match=rf"^extract_percussive_events: {field} must be an integer in \[1, ",
    ):
        libsonare.extract_percussive_events(tone, SR, **{field: 0})
    with pytest.raises(
        SonareValueError,
        match=rf"^render_percussive_events: {field} must be an integer in \[1, ",
    ):
        libsonare.render_percussive_events(tone, SR, [], **{field: 0})


@pytest.mark.parametrize(
    ("field", "zero"), [("onset_wait", 0), ("onset_delta", 0.0), ("max_event_ms", 0.0)]
)
def test_percussive_detection_refuses_zero_the_c_config_reads_as_the_default(
    field: str, zero: float
) -> None:
    tone = _tone()
    assert isinstance(libsonare.extract_percussive_events(tone, SR), list)
    with pytest.raises(
        SonareValueError,
        match=rf"^extract_percussive_events: {field} must not be 0; omit it for the library value$",
    ):
        given: dict[str, Any] = {field: zero}
        libsonare.extract_percussive_events(tone, SR, **given)
