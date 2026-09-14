"""A fractional count is refused, not truncated onto a field's zero.

``int()`` in front of a ctypes field assignment truncates, and for the fields
here zero is not an ordinary value: on the sentinel fields the C side reads 0 as
"keep the default", and on the identity fields 0 is the no-op. Either way
``int(0.5)`` turns a sub-unit request into the outcome the caller would have got
by asking for nothing, and the call returns success -- so the measured signature
of the defect is an accepted call, not a wrong one.

Each case is therefore four probes, not one: the field omitted (A), set to the
value zero selects (B), set to a *different* value (C), and set to a fractional
value (D). (C) is the positive control -- without a result that differs from (A),
"(D) matched (A)" is equally consistent with the field never being read. (D) must
raise, and (A), (B) and (C) must still agree with each other, so the refusal is
shown not to have moved the accepted path.

Zero is an ordinary value on plenty of neighbouring fields -- span bounds, enum
ordinals whose 0 is a real member, MIDI data words -- and those keep truncating.
The separator is the C side: a field is only in scope here when the core reads 0
as the default or as the identity rather than rejecting it.
"""

from __future__ import annotations

import hashlib

import numpy as np
import pytest

import libsonare as ls
from libsonare import SonareValueError

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SAMPLE_RATE = 22050

# The eight count fields behind one helper, each documented in
# sonare_c_polyphony.h as "0 => <default>".
POLYPHONIC_COUNT_FIELDS = (
    "n_fft",
    "hop_length",
    "win_length",
    "salience_harmonics",
    "max_polyphony",
    "mask_harmonics",
    "inharmonicity_min_partials",
    "window_frames",
)


def _digest(samples) -> str:
    return hashlib.sha1(np.asarray(samples, dtype=np.float32).tobytes()).hexdigest()


def _midi1_note_on(note: int, channel: int = 0, velocity: int = 100) -> int:
    return (0x2 << 28) | (0x9 << 20) | ((channel & 0xF) << 16) | (note << 8) | velocity


@pytest.fixture(scope="module")
def chord() -> np.ndarray:
    n = SAMPLE_RATE
    t = np.arange(n) / SAMPLE_RATE
    voices = (220.0, 277.2, 330.0, 415.3)
    return np.sum([0.25 * np.sin(2 * np.pi * f * t) for f in voices], axis=0).astype(np.float32)


@pytest.fixture(scope="module")
def tone() -> np.ndarray:
    n = SAMPLE_RATE
    t = np.arange(n) / SAMPLE_RATE
    return (0.4 * np.sin(2 * np.pi * 220 * t)).astype(np.float32)


@pytest.fixture(scope="module")
def mixture() -> np.ndarray:
    n = SAMPLE_RATE // 2
    t = np.arange(n) / SAMPLE_RATE
    rng = np.random.default_rng(1)
    return (
        0.3 * np.sin(2 * np.pi * 220 * t)
        + 0.2 * np.sin(2 * np.pi * 660 * t)
        + 0.05 * rng.standard_normal(n)
    ).astype(np.float32)


@pytest.fixture(scope="module")
def hits() -> np.ndarray:
    rng = np.random.default_rng(3)
    audio = np.zeros(SAMPLE_RATE, dtype=np.float32)
    envelope = np.exp(-np.arange(1500) / 200.0).astype(np.float32)
    for k in range(6):
        start = k * 3000
        audio[start : start + 1500] += 0.9 * envelope * rng.standard_normal(1500).astype(np.float32)
    return audio


# --- sentinel: zero means "keep the default" -------------------------------


def test_a_fractional_polyphonic_count_is_refused_rather_than_read_as_the_default(chord) -> None:
    """One helper narrows all eight count fields; max_polyphony is the observable."""

    def pitches(**kwargs) -> tuple[float, ...]:
        with ls.PolyphonicAnalysis.analyze(chord, SAMPLE_RATE, **kwargs) as analysis:
            return tuple(round(float(note.median_hz), 2) for note in analysis.notes())

    omitted = pitches()
    assert pitches(max_polyphony=4) == omitted  # 4 is the field's documented default
    assert pitches(max_polyphony=1) != omitted  # positive control
    for field in POLYPHONIC_COUNT_FIELDS:
        with pytest.raises(SonareValueError, match=field):
            pitches(**{field: 0.5})
    assert pitches() == omitted


def test_a_fractional_spectral_edit_count_is_refused_rather_than_read_as_the_default(
    tone,
) -> None:
    """All three of this config's counts document 0 as "keep the default"."""
    op = ls.SpectralRegionOp(
        start_sample=2000,
        end_sample=8000,
        low_hz=300.0,
        high_hz=600.0,
        gain_db=-24.0,
        mode="heal",
    )

    def edited(**kwargs) -> str:
        return _digest(ls.spectral_edit(tone, SAMPLE_RATE, [op], **kwargs))

    omitted = edited()
    assert edited(n_fft=2048, hop_length=512, heal_radius_frames=2) == omitted  # the defaults
    assert edited(heal_radius_frames=7) != omitted  # positive control
    assert edited(hop_length=256) != omitted
    assert edited(n_fft=1024) != omitted
    for field in ("n_fft", "hop_length", "heal_radius_frames"):
        with pytest.raises(SonareValueError, match=field):
            edited(**{field: 0.5})
    assert edited() == omitted


def test_a_fractional_decompose_count_is_refused_rather_than_read_as_the_default(mixture) -> None:
    """Both counts document 0 as "keep the default", and the eager `<= 0` check lets 0.5 past."""

    def factored(**kwargs) -> tuple[tuple[int, ...], str]:
        # nndsvd rather than the default random init, so the content is a
        # deterministic function of the two counts under test.
        result = ls.decompose_stems(mixture, SAMPLE_RATE, init="nndsvd", **kwargs)
        components = np.asarray(result["components"], dtype=np.float32)
        return components.shape, _digest(components)

    omitted = factored()
    assert factored(n_components=4, n_iter=100) == omitted  # the documented defaults
    assert factored(n_components=2) != omitted  # positive control
    assert factored(n_iter=2) != omitted  # positive control
    for field in ("n_components", "n_iter"):
        # Matched on the narrowing's own wording, not just the field name: this
        # entry point also refuses a non-positive count, and that message names
        # the same two fields, so a looser match would be satisfied by the guard
        # catching the truncated 0 rather than by the value being refused.
        with pytest.raises(SonareValueError, match=f"{field} must be an integer"):
            factored(**{field: 0.5})
    assert factored() == omitted


def test_a_fractional_take_source_id_is_refused_rather_than_read_as_the_clips_own_source() -> None:
    """0 on ClipTake.source_id means "use the clip's current source"."""

    def takes_json(source_id) -> bytes:
        project = ls.Project()
        try:
            track = project.add_track("audio", "gtr")
            clip = project.add_clip(
                track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_sample_rate=48000
            )
            project.add_clip(
                track, 480.0, 480.0, audio=[0.3, 0.4, 0.3, 0.0], audio_sample_rate=48000
            )
            take = {"id": 1, "source_offset_ppq": 0.0, "name": "A"}
            if source_id is not None:
                take["source_id"] = source_id
            project.set_clip_takes(clip, [take], active_take_id=1)
            return project.to_json_bytes()
        finally:
            project.close()

    omitted = takes_json(None)
    assert takes_json(0) == omitted
    assert takes_json(2) != omitted  # positive control: source 2 is the second clip's
    with pytest.raises(SonareValueError, match="source_id"):
        takes_json(0.5)
    with pytest.raises(SonareValueError, match="source_id"):
        takes_json(2.5)
    assert takes_json(None) == omitted


def test_a_fractional_active_take_id_is_refused_rather_than_read_as_the_base_source() -> None:
    """The same sentinel one argument over, where set_clip_takes takes it by value."""

    def takes_json(active_take_id) -> bytes:
        project = ls.Project()
        try:
            track = project.add_track("audio", "gtr")
            clip = project.add_clip(
                track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_sample_rate=48000
            )
            takes = [
                {"id": 1, "source_offset_ppq": 0.0, "name": "A"},
                {"id": 2, "source_offset_ppq": 0.5, "name": "B"},
            ]
            if active_take_id is None:
                project.set_clip_takes(clip, takes)
            else:
                project.set_clip_takes(clip, takes, active_take_id=active_take_id)
            return project.to_json_bytes()
        finally:
            project.close()

    omitted = takes_json(None)
    assert takes_json(0) == omitted  # 0 is also this argument's own default
    assert takes_json(2) != omitted  # positive control
    for value in (0.5, 2.5):
        with pytest.raises(SonareValueError, match="active_take_id"):
            takes_json(value)
    assert takes_json(None) == omitted


def test_a_fractional_comp_take_id_is_refused_rather_than_read_as_the_base_take() -> None:
    """0 on ClipCompSegment.take_id selects the base/active fallback."""

    def comp_json(take_id) -> bytes:
        project = ls.Project()
        try:
            track = project.add_track("audio", "gtr")
            clip = project.add_clip(
                track, 0.0, 480.0, audio=[0.1, 0.2, 0.1, 0.0], audio_sample_rate=48000
            )
            project.set_clip_takes(
                clip,
                [
                    {"id": 1, "source_offset_ppq": 0.0, "name": "A"},
                    {"id": 2, "source_offset_ppq": 0.5, "name": "B"},
                ],
                active_take_id=1,
            )
            segment = {"start_ppq": 0.0, "end_ppq": 1.0}
            if take_id is not None:
                segment["take_id"] = take_id
            project.set_clip_comp_segments(clip, [segment])
            return project.to_json_bytes()
        finally:
            project.close()

    omitted = comp_json(None)
    assert comp_json(0) == omitted
    assert comp_json(2) != omitted  # positive control
    with pytest.raises(SonareValueError, match="take_id"):
        comp_json(0.5)
    with pytest.raises(SonareValueError, match="take_id"):
        comp_json(2.5)
    assert comp_json(None) == omitted


def test_a_fractional_click_length_is_refused_rather_than_read_as_derived_from_seconds() -> None:
    """0 on click_samples means "derive the click length from click_seconds"."""

    def stored(**kwargs) -> int:
        with ls.RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
            engine.set_metronome(
                ls.EngineMetronomeConfig(enabled=True, beat_gain=0.5, accent_gain=0.8, **kwargs)
            )
            return engine.metronome().click_samples

    omitted = stored()
    assert stored(click_samples=0) == omitted
    assert stored(click_samples=64) != omitted  # positive control
    for value in (0.5, 64.5):
        with pytest.raises(SonareValueError, match="click_samples"):
            stored(click_samples=value)
    assert stored() == omitted


def test_a_fractional_output_bus_is_refused_rather_than_read_as_the_master_mix() -> None:
    """0 on a lane's output_bus_id keeps the lane on the master mix."""
    frames = 256

    def bus_peak_db(output_bus_id) -> float:
        with ls.RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
            engine.set_clips(
                [
                    ls.EngineClip(
                        id=1,
                        track_id=10,
                        channels=[[0.5] * frames],
                        start_ppq=0.0,
                        length_samples=frames,
                    )
                ]
            )
            engine.set_track_buses(
                [{"bus_id": 1, "channel_layout": ls.ChannelLayout.FIVE_POINT_ONE}]
            )
            lane: dict[str, object] = {"track_id": 10}
            if output_bus_id is not None:
                lane["output_bus_id"] = output_bus_id
            engine.set_track_lanes([lane])
            engine.play()
            engine.process([[0.0] * frames for _ in range(6)])
            wide = engine.drain_meter_telemetry_wide()
            bus = next(record for record in wide if record.target_id == 33)
            return round(max(bus.peak_db), 3)

    omitted = bus_peak_db(None)
    assert bus_peak_db(0) == omitted
    assert bus_peak_db(1) != omitted  # positive control: the lane reaches the group bus
    for value in (0.5, 1.5):
        with pytest.raises(SonareValueError, match="output_bus_id"):
            bus_peak_db(value)
    assert bus_peak_db(None) == omitted


def test_a_fractional_port_count_is_refused_rather_than_read_as_the_specs_channels() -> None:
    """0 on a graph node's num_ports takes the spec's channel count."""

    def build(num_ports) -> tuple[float, float]:
        node_kwargs = {} if num_ports is None else {"num_ports": num_ports}
        with ls.RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
            engine.set_graph(
                ls.EngineGraphSpec(
                    nodes=[
                        ls.EngineGraphNode("in"),
                        ls.EngineGraphNode(
                            "gain", ls.EngineGraphNodeType.GAIN, gain_db=0.0, **node_kwargs
                        ),
                        ls.EngineGraphNode("out"),
                    ],
                    connections=[
                        ls.EngineGraphConnection("in", 0, "gain", 0),
                        ls.EngineGraphConnection("in", 1, "gain", 1),
                        ls.EngineGraphConnection("gain", 0, "out", 0),
                        ls.EngineGraphConnection("gain", 1, "out", 1),
                    ],
                    input_node="in",
                    output_node="out",
                    num_channels=2,
                )
            )
            engine.play()
            out = engine.process([[0.25] * 128, [-0.25] * 128])
            return round(out[0][0], 4), round(out[1][0], 4)

    omitted = build(None)
    assert build(0) == omitted
    # Positive control: one port cannot carry the second connection, so a value
    # that is not the sentinel changes the outcome rather than being ignored.
    with pytest.raises(ls.SonareError):
        build(1)
    for value in (0.5, 1.5):
        with pytest.raises(SonareValueError, match="num_ports"):
            build(value)
    assert build(None) == omitted


@pytest.mark.parametrize(
    ("binder", "config", "extra", "default_voices"),
    [
        ("set_builtin_instrument", "BuiltinSynthConfig", {"waveform": "saw", "gain": 0.5}, 16),
        ("set_sf2_instrument", "Sf2InstrumentConfig", {"gain": 1.0}, 48),
    ],
)
def test_a_fractional_voice_count_is_refused_rather_than_read_as_the_default(
    binder, config, extra, default_voices
) -> None:
    """Both instrument configs read 0 on polyphony as "keep the default voice cap"."""
    frames = 256
    chord = (60, 64, 67, 71, 74, 77, 81)  # seven at once, so a lower cap is audible

    def rendered(polyphony) -> str:
        kwargs = dict(extra)
        if polyphony is not None:
            kwargs["polyphony"] = polyphony
        with ls.RealtimeEngine(sample_rate=48000.0, max_block_size=frames) as engine:
            getattr(engine, binder)(getattr(ls, config)(**kwargs), 6)
            engine.set_midi_clips(
                [
                    ls.EngineMidiClipSchedule(
                        id=1,
                        track_id=6,
                        destination_id=6,
                        length_samples=1 << 20,
                        events=[
                            ls.EngineMidiEvent(0, word0=_midi1_note_on(note), word_count=1)
                            for note in chord
                        ],
                    )
                ]
            )
            engine.play()
            for _ in range(5):
                block = engine.process([[0.0] * frames, [0.0] * frames])
            return _digest(block)

    omitted = rendered(None)
    assert rendered(0) == omitted
    assert rendered(default_voices) == omitted  # the field's documented default
    assert rendered(1) != omitted  # positive control: the cap steals voices
    for value in (0.5, 1.5):
        with pytest.raises(SonareValueError, match=f"{config}: polyphony must be an integer"):
            rendered(value)
    assert rendered(None) == omitted


def test_a_fractional_dither_word_length_is_refused_rather_than_read_as_the_default() -> None:
    """0 on EngineBounceOptions.dither_bits keeps the default 16-bit word length."""
    frames = 512

    def bounced(dither_bits) -> str:
        kwargs = {} if dither_bits is None else {"dither_bits": dither_bits}
        with ls.RealtimeEngine(sample_rate=48000.0, max_block_size=256) as engine:
            rng = np.random.default_rng(5)
            signal = (0.4 * rng.standard_normal(frames)).astype(np.float32).tolist()
            engine.set_clips(
                [
                    ls.EngineClip(
                        id=1,
                        track_id=10,
                        channels=[signal, signal],
                        start_ppq=0.0,
                        length_samples=frames,
                    )
                ]
            )
            engine.play()
            result = engine.bounce_offline(
                ls.EngineBounceOptions(total_frames=frames, dither=1, **kwargs)
            )
            return _digest(result.interleaved)

    omitted = bounced(None)  # the dataclass default is already 16
    assert bounced(0) == omitted
    assert bounced(16) == omitted
    assert bounced(8) != omitted  # positive control
    for value in (0.5, 8.5):
        with pytest.raises(SonareValueError, match="dither_bits"):
            bounced(value)
    assert bounced(None) == omitted


# --- identity: zero is the no-op, reached the same way ----------------------


def test_a_fractional_note_offset_is_refused_rather_than_rendered_unmoved(tone) -> None:
    """0 on NoteEdit.time_offset_samples is the identity, so a sub-sample shift is a no-op."""
    frame_rate = SAMPLE_RATE / 256.0
    f0 = np.full(int(len(tone) / 256) + 1, 220.0, dtype=np.float32)
    voiced_prob = np.ones_like(f0)

    def rendered(offset) -> str:
        notes = ls.extract_notes(tone, SAMPLE_RATE, f0, frame_rate, voiced_prob=voiced_prob)
        if offset is not None:
            notes[0].edit.time_offset_samples = offset
        return _digest(ls.render_notes(tone, SAMPLE_RATE, notes, f0_hz=f0, frame_rate=frame_rate))

    untouched = rendered(None)
    assert rendered(0) == untouched
    assert rendered(500) != untouched  # positive control
    for value in (0.5, 500.5):
        with pytest.raises(SonareValueError, match="time_offset_samples"):
            rendered(value)
    assert rendered(None) == untouched


def test_a_fractional_event_offset_is_refused_rather_than_rendered_unmoved(hits) -> None:
    """The same identity on a percussive event's edit."""

    def rendered(offset) -> str:
        events = ls.extract_percussive_events(hits, SAMPLE_RATE)
        if offset is not None:
            events[0].edit.time_offset_samples = offset
        return _digest(ls.render_percussive_events(hits, SAMPLE_RATE, events))

    untouched = rendered(None)
    assert rendered(0) == untouched
    assert rendered(500) != untouched  # positive control
    for value in (0.5, 500.5):
        with pytest.raises(SonareValueError, match="time_offset_samples"):
            rendered(value)
    assert rendered(None) == untouched


def test_a_fractional_polyphonic_note_offset_is_refused_rather_than_rendered_unmoved(
    chord,
) -> None:
    """The handle door reaches the same field through its own marshaller."""

    def rendered(offset) -> str:
        with ls.PolyphonicAnalysis.analyze(chord, SAMPLE_RATE) as analysis:
            if offset is not None:
                edit = analysis.notes()[0].edit
                edit.time_offset_samples = offset
                analysis.set_note_edit(0, edit)
            return _digest(analysis.render())

    untouched = rendered(None)
    assert rendered(0) == untouched
    assert rendered(500) != untouched  # positive control
    for value in (0.5, 500.5):
        with pytest.raises(SonareValueError, match="time_offset_samples"):
            rendered(value)
    assert rendered(None) == untouched
