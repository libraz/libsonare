"""Realtime engine MIDI tests: live input, scheduled MIDI clips and external MIDI routing."""

from __future__ import annotations

import numpy as np
import pytest

from libsonare import (
    BuiltinSynthConfig,
    EngineMidiClipSchedule,
    EngineMidiEvent,
    ExternalMidiEvent,
    MidiCcBinding,
    RealtimeEngine,
    SonareError,
    SonareValueError,
)

from ._helpers import _midi1_word

# ---------------------------------------------------------------------------
# Live-MIDI parity surface (built-in instrument bind, CC bindings, queued
# input source, immediate note/CC injection) added for Node/WASM parity.
# ---------------------------------------------------------------------------


def test_engine_builtin_instrument_bind_and_clear() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(waveform="saw", gain=0.5), 0)
        assert engine.midi_instrument_count() == 1
        engine.set_builtin_instrument()  # default sine patch on destination 0
        assert engine.midi_instrument_count() == 1
        engine.clear_midi_instrument(0)
        assert engine.midi_instrument_count() == 0


def test_engine_midi_cc_bindings() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.bind_midi_cc(0, 1, 42, min_value=0.0, max_value=1.0)
        engine.bind_midi_cc_binding(
            MidiCcBinding(
                cc_number=2,
                cc_lsb_number=34,
                channel=0,
                kind=1,
                selector_msb=0,
                selector_lsb=0,
                param_id=43,
                min_value=0.0,
                max_value=1.0,
            )
        )
        assert engine.midi_cc_binding_count() == 2
        engine.clear_midi_cc_bindings()
        assert engine.midi_cc_binding_count() == 0


def test_engine_set_and_clear_midi_fx_round_trips() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_midi_fx(0, '{"transpose_semitones":12}')
        engine.clear_midi_fx(0)
        with pytest.raises(SonareError):
            engine.set_midi_fx(0, "{bad json")


def test_engine_live_midi_input_source_queue() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        # The input family is refused until the input source is enabled.
        with pytest.raises(SonareError):
            engine.push_midi_input_pitch_bend(0, 0, 16383, 0)
        engine.set_midi_input_source(0)
        engine.push_midi_input_note_on(0, 0, 60, 100, 0)
        engine.push_midi_input_cc(0, 0, 1, 64, 0)
        engine.push_midi_input_pitch_bend(0, 0, 16383, 0)
        engine.push_midi_input_channel_pressure(0, 0, 127, 0)
        engine.push_midi_input_poly_pressure(0, 0, 60, 96, 0)
        engine.push_midi_input_note_off(0, 0, 60, 0, 0)
        assert engine.midi_input_pending_count() == 6
        engine.clear_midi_input_source()


def test_engine_push_immediate_notes_do_not_raise() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        engine.push_midi_note_on(0, 0, 0, 60, 100)
        engine.push_midi_note_off(0, 0, 0, 60, 0)


def test_engine_push_midi_bend_and_pressure_change_rendered_audio() -> None:
    sample_rate = 48000.0
    block = 512
    window = 8192

    def render(engine: RealtimeEngine, frames: int) -> np.ndarray:
        blocks = [
            np.asarray(engine.process([[0.0] * block, [0.0] * block]))[0]
            for _ in range(frames // block)
        ]
        return np.concatenate(blocks)

    def peak_hz(samples: np.ndarray) -> float:
        spectrum = np.abs(np.fft.rfft(samples * np.hanning(len(samples))))
        bin_index = int(np.argmax(spectrum))
        # One bin is 5.9 Hz here, which is 2% of the fundamental and would eat
        # the tolerance below whole, so interpolate the peak over its
        # neighbours rather than lengthening the render.
        if 0 < bin_index < len(spectrum) - 1:
            low, mid, high = spectrum[bin_index - 1 : bin_index + 2]
            curvature = low - 2.0 * mid + high
            if curvature != 0.0:
                return (bin_index + 0.5 * (low - high) / curvature) * sample_rate / len(samples)
        return bin_index * sample_rate / len(samples)

    def rms(samples: np.ndarray) -> float:
        return float(np.sqrt(np.mean(np.square(samples))))

    with RealtimeEngine(sample_rate=sample_rate, max_block_size=block) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        engine.push_midi_note_on(0, 0, 0, 60, 100)
        render(engine, window)  # Let the amplitude envelope reach sustain.
        plain = render(engine, window)
        engine.push_midi_pitch_bend(0, 0, 0, 16383)
        bent = render(engine, window)
        engine.push_midi_channel_pressure(0, 0, 0, 127)
        pressed = render(engine, window)
        # Channel pressure back to zero, so what the next block carries is the
        # key pressure alone rather than the pair saturating together.
        engine.push_midi_channel_pressure(0, 0, 0, 0)
        engine.push_midi_poly_pressure(0, 0, 0, 60, 127)
        poly_pressed = render(engine, window)

    assert peak_hz(plain) == pytest.approx(261.6, rel=0.01)
    # A full positive bend is the built-in synth's whole +2-semitone range.
    assert peak_hz(bent) / peak_hz(plain) == pytest.approx(2 ** (2 / 12), rel=0.01)
    # Full channel pressure doubles the amplitude; the bend left it alone.
    assert rms(bent) == pytest.approx(rms(plain), rel=0.01)
    assert rms(pressed) / rms(bent) == pytest.approx(2.0, rel=0.01)
    # Key pressure reaches the one sounding voice and doubles it the same way.
    assert rms(poly_pressed) / rms(bent) == pytest.approx(2.0, rel=0.01)


def test_engine_push_midi_bend_rejects_out_of_range_value() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        engine.push_midi_pitch_bend(0, 0, 0, 16383)
        with pytest.raises(SonareError) as bend_error:
            engine.push_midi_pitch_bend(0, 0, 0, 16384)
        assert bend_error.value.code == 4
        engine.set_midi_input_source(0)
        with pytest.raises(SonareError):
            engine.push_midi_input_pitch_bend(0, 0, 16384, 0)
        for pressure in (128, 255):
            with pytest.raises(SonareError):
                engine.push_midi_channel_pressure(0, 0, 0, pressure)


def test_engine_push_midi_sysex_accepts_frame_and_rejects_oversized() -> None:
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        # GM system-on universal SysEx (0xF0 ... 0xF7).
        engine.push_midi_sysex(0, b"\xf0\x7e\x7f\x09\x01\xf7")
        engine.process([[0.0] * 128, [0.0] * 128])
        with pytest.raises(SonareError):
            engine.push_midi_sysex(0, b"\xf0" + b"\x00" * 1022 + b"\xf7")
        with pytest.raises(ValueError):
            engine.push_midi_sysex(0, b"")


def test_engine_scheduled_midi_clips_render_builtin_instrument() -> None:
    def midi1_word(status: int, channel: int, data0: int, data1: int) -> int:
        return (0x2 << 28) | ((status & 0xF) << 20) | ((channel & 0xF) << 16) | (data0 << 8) | data1

    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(gain=0.5), 6)
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=1,
                    track_id=6,
                    destination_id=6,
                    length_samples=8192,
                    events=[
                        EngineMidiEvent(0, word0=midi1_word(0x9, 0, 60, 100), word_count=1),
                        EngineMidiEvent(4096, word0=midi1_word(0x8, 0, 60, 0), word_count=1),
                    ],
                )
            ]
        )
        engine.play()
        out = np.asarray(engine.process([[0.0] * 128, [0.0] * 128]))
        assert float(np.max(np.abs(out))) > 0.0
        with pytest.raises(SonareError) as bad_group_error:
            engine.set_midi_clips(
                [
                    EngineMidiClipSchedule(
                        id=2,
                        track_id=6,
                        destination_id=6,
                        events=[
                            EngineMidiEvent(
                                0,
                                word0=midi1_word(0x9, 0, 60, 100),
                                word_count=1,
                                group=16,
                            )
                        ],
                    )
                ]
            )
        assert bad_group_error.value.code == 4
        with pytest.raises(SonareError) as bad_channel_error:
            engine.push_midi_note_on(6, 0, 16, 60, 100)
        assert bad_channel_error.value.code == 4
        with pytest.raises(SonareError) as bad_soundfont_error:
            engine.load_soundfont(b"not sf2")
        assert bad_soundfont_error.value.code == 2
        engine.set_midi_clips([])


def test_engine_drains_external_midi_routing_to_host() -> None:
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        # Route destination 5 to the external queue instead of an instrument.
        engine.set_midi_destination_external(5, True)
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=7,
                    track_id=5,
                    destination_id=5,
                    length_samples=256,
                    events=[
                        EngineMidiEvent(
                            0, word0=_midi1_word(0x9, 1, 64, 110), word_count=1, group=1
                        ),
                        EngineMidiEvent(
                            48, word0=_midi1_word(0x8, 1, 64, 0), word_count=1, group=1
                        ),
                    ],
                )
            ]
        )
        engine.play()
        engine.process([[0.0] * 128, [0.0] * 128])

        drained = engine.drain_external_midi()
        assert len(drained) == 2
        assert all(isinstance(event, ExternalMidiEvent) for event in drained)
        assert drained[0].destination_id == 5
        assert len(drained[0].bytes) == 3
        assert drained[0].bytes[0] & 0xF0 == 0x90  # note on
        assert drained[0].render_frame == 0
        assert drained[1].destination_id == 5
        assert drained[1].bytes[0] & 0xF0 == 0x80  # note off
        assert drained[1].render_frame == 48

        # A drained queue stays empty and overflow telemetry is observable.
        assert engine.drain_external_midi() == []
        assert engine.external_midi_dropped_count() == 0


def test_engine_clip_event_group_comes_from_word0() -> None:
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_midi_destination_external(7, True)
        group = 3
        note_on = _midi1_word(0x9, 0, 60, 100) | (group << 24)
        note_off = _midi1_word(0x8, 0, 60, 0) | (group << 24)

        # A group of 256 narrows to 0 in the c_uint8 field, which is a valid
        # group, so the C ABI's own range check would accept it. Reject the wrap
        # before ctypes can hide it.
        with pytest.raises(ValueError):
            engine.set_midi_clips(
                [
                    EngineMidiClipSchedule(
                        id=1,
                        track_id=7,
                        destination_id=7,
                        events=[EngineMidiEvent(0, word0=note_on, word_count=1, group=256)],
                    )
                ]
            )

        # ``group`` is ignored on input. The pair below is authored on one word0
        # group and disagrees only in the redundant field; read under two groups
        # the note-off would not match its note-on, leaving the note sounding so
        # that stopping the transport emits a hang-note release.
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=2,
                    track_id=7,
                    destination_id=7,
                    length_samples=256,
                    events=[
                        EngineMidiEvent(0, word0=note_on, word_count=1, group=5),
                        EngineMidiEvent(48, word0=note_off, word_count=1, group=0),
                    ],
                )
            ]
        )
        engine.play()
        engine.process([[0.0] * 128, [0.0] * 128])
        assert len(engine.drain_external_midi()) == 2

        engine.stop()
        engine.process([[0.0] * 128, [0.0] * 128])
        assert engine.drain_external_midi() == []


def test_engine_external_destination_table_overflow() -> None:
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        for dest_id in range(16):
            engine.set_midi_destination_external(dest_id, True)
        # Re-marking an existing destination is idempotent.
        engine.set_midi_destination_external(3, True)
        # The 17th distinct destination overflows the slot table.
        with pytest.raises((ValueError, RuntimeError)):
            engine.set_midi_destination_external(99, True)
        # Freeing a slot lets the next mark succeed.
        engine.set_midi_destination_external(3, False)
        engine.set_midi_destination_external(99, True)


def test_engine_drain_external_midi_honors_max_records_cap_losslessly() -> None:
    # Regression: drain must treat max_records as an output-event cap (not drain
    # the whole queue), and must not lose events past the cap — they stay queued.
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_midi_destination_external(5, True)
        n_notes = 24
        events = []
        for i in range(n_notes):
            events.append(
                EngineMidiEvent(i, word0=_midi1_word(0x9, 1, 40 + i, 100), word_count=1, group=1)
            )
            events.append(
                EngineMidiEvent(i, word0=_midi1_word(0x8, 1, 40 + i, 0), word_count=1, group=1)
            )
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=7, track_id=5, destination_id=5, length_samples=256, events=events
                )
            ]
        )
        engine.play()
        engine.process([[0.0] * 128, [0.0] * 128])

        # A single capped drain returns at most the cap and leaves the rest queued.
        first = engine.drain_external_midi(5)
        assert len(first) <= 5

        collected = list(first)
        for _ in range(1000):
            batch = engine.drain_external_midi(5)
            if not batch:
                break
            assert len(batch) <= 5
            collected.extend(batch)

        assert len(collected) == 2 * n_notes
        assert engine.external_midi_dropped_count() == 0
        # Losslessness: every queued note-on and note-off survives the capped drain.
        note_ons = {ev.bytes[1] for ev in collected if ev.bytes[0] & 0xF0 == 0x90}
        note_offs = {ev.bytes[1] for ev in collected if ev.bytes[0] & 0xF0 == 0x80}
        for i in range(n_notes):
            assert (40 + i) in note_ons
            assert (40 + i) in note_offs


@pytest.mark.parametrize("max_records", [1, 2, 3])
def test_engine_drain_external_midi_refuses_a_budget_below_the_lowering_bound(
    max_records,
) -> None:
    """A budget too small to consume a record is refused, not silently ignored.

    One queued record lowers to at most four MIDI 1.0 messages (a registered
    controller's RPN sequence), so a budget of 1 to 3 can never consume one.
    It used to take neither the empty-budget early return nor a drain: the
    loop broke on its first iteration and returned an empty list with the
    events still queued, on every repeated call. The C ABI
    and the other bindings refuse the same budget.
    """
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_midi_destination_external(5, True)
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=7,
                    track_id=5,
                    destination_id=5,
                    length_samples=256,
                    events=[
                        EngineMidiEvent(
                            0, word0=_midi1_word(0x9, 1, 60, 100), word_count=1, group=1
                        ),
                        EngineMidiEvent(1, word0=_midi1_word(0x8, 1, 60, 0), word_count=1, group=1),
                    ],
                )
            ]
        )
        engine.play()
        engine.process([[0.0] * 128, [0.0] * 128])

        with pytest.raises(SonareValueError, match="at least 4"):
            engine.drain_external_midi(max_records)
        # A refusal is also caught by the plain argument-validation style.
        with pytest.raises(ValueError):
            engine.drain_external_midi(max_records)

        # An empty or negative budget keeps drawing nothing without raising, so
        # the boundary between "drain nothing" and "refuse" is explicit.
        assert engine.drain_external_midi(0) == []
        assert engine.drain_external_midi(-1) == []

        # Positive control: no refused call consumed an event.
        assert len(engine.drain_external_midi()) == 2
        assert engine.external_midi_dropped_count() == 0


def test_engine_forwards_midi_clock_transport_to_external_queue() -> None:
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=24000, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_tempo(120.0)
        engine.set_external_midi_clock_enabled(True)
        engine.play(0)
        engine.process([[0.0] * 24000, [0.0] * 24000])

        # One Start plus 24 clock ticks (one every 1000 samples at 120 BPM).
        drained = engine.drain_external_midi()
        assert len(drained) == 25
        for event in drained:
            assert event.destination_id == 4294967295
            assert len(event.bytes) == 1
        assert drained[0].bytes[0] == 0xFA  # Start
        assert drained[1].bytes[0] == 0xF8  # Clock


def test_engine_set_midi_clips_forwards_explicit_destination_id_zero() -> None:
    # Regression: an explicit destination_id=0 must be forwarded unchanged, not
    # silently rewritten to track_id. Only the instrument at destination 0 is
    # bound, so a rewrite to track_id would leave the note unheard (silence).
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(gain=0.5), 0)
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=1,
                    track_id=6,
                    destination_id=0,
                    length_samples=8192,
                    events=[
                        EngineMidiEvent(0, word0=_midi1_word(0x9, 0, 60, 100), word_count=1),
                        EngineMidiEvent(4096, word0=_midi1_word(0x8, 0, 60, 0), word_count=1),
                    ],
                )
            ]
        )
        engine.play()
        out = np.asarray(engine.process([[0.0] * 128, [0.0] * 128]))
        assert float(np.max(np.abs(out))) > 0.0


def test_engine_set_midi_clips_omitted_destination_id_falls_back_to_track_id() -> None:
    # Omitting destination_id (the sentinel default None) still falls back to
    # track_id, so an instrument bound only at the track's own destination is heard.
    with RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(gain=0.5), 6)
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=1,
                    track_id=6,
                    length_samples=8192,
                    events=[
                        EngineMidiEvent(0, word0=_midi1_word(0x9, 0, 60, 100), word_count=1),
                        EngineMidiEvent(4096, word0=_midi1_word(0x8, 0, 60, 0), word_count=1),
                    ],
                )
            ]
        )
        engine.play()
        out = np.asarray(engine.process([[0.0] * 128, [0.0] * 128]))
        assert float(np.max(np.abs(out))) > 0.0


def test_engine_drain_external_midi_delivers_a_midi2_rpn_as_four_cc_messages() -> None:
    """A MIDI 2.0 Registered Controller reaches a MIDI 1.0 port as CC 101/100/6/38."""
    rpn_word0 = (0x4 << 28) | (0x2 << 20) | (1 << 16)  # group 0, channel 1, RPN 0/0
    with RealtimeEngine(
        sample_rate=48000.0, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_midi_destination_external(5, True)
        engine.set_midi_clips(
            [
                EngineMidiClipSchedule(
                    id=7,
                    track_id=5,
                    destination_id=5,
                    length_samples=256,
                    events=[EngineMidiEvent(0, word0=rpn_word0, word1=12 << 25, word_count=2)],
                )
            ]
        )
        engine.play()
        engine.process([[0.0] * 128, [0.0] * 128])
        drained = engine.drain_external_midi(4)
        assert [list(e.bytes) for e in drained] == [
            [0xB1, 101, 0],
            [0xB1, 100, 0],
            [0xB1, 6, 12],
            [0xB1, 38, 0],
        ]
