"""Realtime engine MIDI tests: live input, scheduled MIDI clips and external MIDI routing."""

from __future__ import annotations

import ctypes
import os
from typing import Any

import numpy as np
import pytest

from libsonare import (
    BuiltinSynthConfig,
    EngineMidiClipSchedule,
    EngineMidiEvent,
    ErrorCode,
    ExternalMidiEvent,
    MidiCcBinding,
    ProjectMidiEvent,
    RealtimeEngine,
    Sf2InstrumentConfig,
    SonareError,
    SonareValueError,
    _engine_midi,
    midi_cc_learn,
    midi_cc_to_breakpoint,
    midi_param_to_cc,
)
from libsonare._ffi_types_mastering_project import SonareMidiCcBinding
from libsonare._runtime import _get_lib

from ._helpers import _midi1_word, raises_coded

# ---------------------------------------------------------------------------
# Live-MIDI parity surface (built-in instrument bind, CC bindings, queued
# input source, immediate note/CC injection) added for Node/WASM parity.
# ---------------------------------------------------------------------------


def test_engine_builtin_instrument_bind_and_clear() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(waveform="saw", gain=0.5), 0)
        assert engine.midi_instrument_count == 1
        engine.set_builtin_instrument()  # default sine patch on destination 0
        assert engine.midi_instrument_count == 1
        engine.clear_midi_instrument(0)
        assert engine.midi_instrument_count == 0


@pytest.mark.parametrize(
    ("bind", "config", "init", "entry"),
    [
        (
            "set_builtin_instrument",
            BuiltinSynthConfig(waveform="saw", gain=0.5),
            "sonare_engine_builtin_synth_config_init",
            "sonare_engine_set_builtin_instrument",
        ),
        (
            "set_sf2_instrument",
            Sf2InstrumentConfig(gain=0.5),
            "sonare_engine_sf2_instrument_config_init",
            "sonare_engine_set_sf2_instrument",
        ),
    ],
)
def test_an_engine_instrument_config_is_seeded_by_the_engines_own_init(
    monkeypatch: pytest.MonkeyPatch, bind: str, config: Any, init: str, entry: str
) -> None:
    lib = _get_lib()
    seeded: list[int] = []
    passed: list[tuple[int, float]] = []
    real_init, real_entry = getattr(lib, init), getattr(lib, entry)

    def record_init(ptr: Any) -> None:
        real_init(ptr)
        seeded.append(ptr._obj.struct_size)

    def record_entry(handle: Any, destination: Any, ptr: Any) -> int:
        passed.append((ptr._obj.struct_size, ptr._obj.gain))
        return int(real_entry(handle, destination, ptr))

    monkeypatch.setattr(lib, init, record_init)
    monkeypatch.setattr(lib, entry, record_entry)
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        getattr(engine, bind)(config, 0)
        assert engine.midi_instrument_count == 1
    assert len(seeded) == 1 and seeded[0] > 0
    assert passed == [(seeded[0], 0.5)]


def test_engine_bind_midi_cc_checks_every_shape_before_domains() -> None:
    binding = MidiCcBinding(
        cc_number=128,
        param_id=42,
        max_value=object(),  # type: ignore[arg-type]
    )
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(TypeError) as refused:
            engine.bind_midi_cc(binding)
        assert str(refused.value) == (
            "RealtimeEngine.bind_midi_cc: binding.max_value must be a number"
        )


def test_engine_midi_cc_bindings() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.bind_midi_cc(MidiCcBinding(cc_number=1, channel=0, param_id=42))
        engine.bind_midi_cc(
            MidiCcBinding(
                cc_number=2,
                cc_lsb_number=34,
                channel=0,
                kind="controlChange14",
                selector_msb=0,
                selector_lsb=0,
                param_id=43,
                min_value=0.0,
                max_value=1.0,
                consume=True,
            )
        )
        bindings = sorted(engine.midi_cc_bindings(), key=lambda binding: binding.cc_number)
        assert len(bindings) == 2
        assert [binding.consume for binding in bindings] == [False, True]
        engine.clear_midi_cc_bindings()
        assert engine.midi_cc_bindings() == []
        for binding in reversed(bindings):
            engine.bind_midi_cc(binding)
        assert sorted(engine.midi_cc_bindings(), key=lambda binding: binding.cc_number) == bindings
        bindings.clear()
        assert len(engine.midi_cc_bindings()) == 2


@pytest.mark.skipif(
    os.environ.get("SONARE_TEST_CC_CLEAR_OOM") != "1",
    reason="requires the explicit CC-map allocation-failure injection",
)
def test_engine_midi_cc_clear_out_of_memory_preserves_bindings() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.bind_midi_cc(MidiCcBinding(cc_number=7, channel=0, param_id=42))
        before = engine.midi_cc_bindings()
        assert len(before) == 1
        with raises_coded(ErrorCode.OUT_OF_MEMORY):
            engine.clear_midi_cc_bindings()
        assert engine.midi_cc_bindings() == before


class _FakeMidiCcBindingLibrary:
    def __init__(self) -> None:
        self.rows: dict[int, list[SonareMidiCcBinding]] = {}
        self.arrays: list[Any] = []
        self.bound_consumes: list[int] = []
        self.free_calls = 0
        self.malformed_list = False

    def sonare_engine_bind_midi_cc(self, handle: ctypes.c_void_p, binding: Any) -> int:
        raw = binding._obj
        copied = SonareMidiCcBinding()
        for name, *_ in copied._fields_:
            setattr(copied, name, getattr(raw, name))
        self.bound_consumes.append(int(copied.consume))
        rows = self.rows.setdefault(int(handle.value), [])
        identity = (int(copied.cc_number), int(copied.channel))
        rows[:] = [row for row in rows if (int(row.cc_number), int(row.channel)) != identity]
        rows.append(copied)
        return 0

    def sonare_engine_midi_cc_bindings(self, handle: ctypes.c_void_p, out: Any) -> int:
        rows = list(reversed(self.rows.get(int(handle.value), [])))
        raw = out._obj
        if self.malformed_list:
            raw.count = 1
            raw.items = ctypes.POINTER(SonareMidiCcBinding)()
            return 0
        raw.count = len(rows)
        if rows:
            array = (SonareMidiCcBinding * len(rows))(*rows)
            self.arrays.append(array)
            raw.items = ctypes.cast(array, ctypes.POINTER(SonareMidiCcBinding))
        else:
            raw.items = ctypes.POINTER(SonareMidiCcBinding)()
        return 0

    def sonare_free_midi_cc_binding_list(self, result: Any) -> None:
        self.free_calls += 1
        raw = result._obj
        if self.arrays:
            array = self.arrays.pop()
            for row in array:
                for name, *_ in row._fields_:
                    setattr(row, name, 0)
        raw.items = ctypes.POINTER(SonareMidiCcBinding)()
        raw.count = 0


def _bare_engine(handle: int) -> RealtimeEngine:
    engine = object.__new__(RealtimeEngine)
    engine._handle = ctypes.c_void_p(handle)
    return engine


def test_engine_midi_cc_bindings_round_trip_copies_and_preserves_consume(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fake = _FakeMidiCcBindingLibrary()
    monkeypatch.setattr(_engine_midi, "_get_lib", lambda: fake)
    first = _bare_engine(1)
    second = _bare_engine(2)
    default = MidiCcBinding(cc_number=1, param_id=42)
    consuming = MidiCcBinding(cc_number=2, channel=3, param_id=43, consume=True)

    assert first.midi_cc_bindings() == []
    first.bind_midi_cc(default)
    first.bind_midi_cc(consuming)
    with pytest.raises(TypeError, match=r"binding\.consume must be a bool"):
        first.bind_midi_cc(MidiCcBinding(cc_number=3, param_id=44, consume=1))  # type: ignore[arg-type]

    snapshot = first.midi_cc_bindings()
    assert snapshot[0].kind == "controlChange7"
    assert fake.bound_consumes == [0, 1]
    for binding in snapshot:
        second.bind_midi_cc(binding)

    def by_cc_number(binding: MidiCcBinding) -> int:
        return binding.cc_number

    expected = [default, consuming]
    assert sorted(snapshot, key=by_cc_number) == expected
    assert sorted(second.midi_cc_bindings(), key=by_cc_number) == expected
    snapshot.clear()
    assert sorted(first.midi_cc_bindings(), key=by_cc_number) == expected
    assert sorted(second.midi_cc_bindings(), key=by_cc_number) == expected
    assert fake.free_calls == 5
    first._handle = None
    second._handle = None


class _FakeMidiCcUtilityLibrary:
    def __init__(self) -> None:
        self.conversion_consumes: list[tuple[int, ...]] = []

    def _consume_values(self, bindings: Any, count: int) -> tuple[int, ...]:
        values = tuple(int(bindings[index].consume) for index in range(count.value))
        self.conversion_consumes.append(values)
        return values

    def sonare_midi_cc_to_breakpoint(self, bindings: Any, count: int, event: Any, out: Any) -> int:
        self._consume_values(bindings, count)
        result = out._obj
        result.ppq = event._obj.ppq
        result.value = 0.375
        result.curve_to_next = 0
        return 0

    def sonare_midi_param_to_cc(
        self,
        bindings: Any,
        count: int,
        param_id: int,
        unit_value: float,
        group: int,
        ppq: float,
        out: Any,
    ) -> int:
        self._consume_values(bindings, count)
        result = out._obj
        result.ppq = ppq
        result.word0 = 0x20000000
        result.word1 = 0
        result.sysex = None
        result.sysex_size = 0
        return 0

    def sonare_midi_cc_learn(
        self,
        events: Any,
        count: int,
        param_id: int,
        min_value: float,
        max_value: float,
        min_movement: int,
        out: Any,
    ) -> int:
        result = out._obj
        result.cc_number = 7
        result.channel = 2
        result.kind = 0
        result.cc_lsb_number = 0
        result.selector_msb = 0
        result.selector_lsb = 0
        result.consume = 0
        result.reserved = 0
        result.param_id = param_id
        result.min_value = min_value
        result.max_value = max_value
        return 0


def test_midi_cc_conversion_validates_but_ignores_consume(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import libsonare._project_midi as project_midi_module

    fake = _FakeMidiCcUtilityLibrary()
    monkeypatch.setattr(project_midi_module, "_get_lib", lambda: fake)
    plain = MidiCcBinding(cc_number=7, param_id=5)
    consuming = MidiCcBinding(cc_number=7, param_id=5, consume=True)
    event = ProjectMidiEvent(ppq=2.0, word0=0x20000000)

    assert midi_cc_to_breakpoint([consuming], event) == midi_cc_to_breakpoint([plain], event)
    assert midi_param_to_cc([consuming], 5, 0.5, 0, ppq=3.0) == midi_param_to_cc(
        [plain], 5, 0.5, 0, ppq=3.0
    )
    assert fake.conversion_consumes == [(1,), (0,), (1,), (0,)]

    invalid = MidiCcBinding(cc_number=7, param_id=5, consume=1)  # type: ignore[arg-type]
    with pytest.raises(TypeError, match=r"bindings\[0\]\.consume must be a bool"):
        midi_cc_to_breakpoint([invalid], event)
    with pytest.raises(TypeError, match=r"bindings\[0\]\.consume must be a bool"):
        midi_param_to_cc([invalid], 5, 0.5, 0, ppq=3.0)
    assert fake.conversion_consumes == [(1,), (0,), (1,), (0,)]


def test_midi_cc_learned_binding_defaults_to_not_consuming(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import libsonare._project_midi as project_midi_module

    monkeypatch.setattr(project_midi_module, "_get_lib", lambda: _FakeMidiCcUtilityLibrary())
    learned = midi_cc_learn([], 77, min_value=-1.0, max_value=1.0)
    assert learned == MidiCcBinding(
        cc_number=7,
        channel=2,
        param_id=77,
        min_value=-1.0,
        max_value=1.0,
        consume=False,
    )


def test_engine_midi_cc_bindings_refuse_inconsistent_owned_list(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    fake = _FakeMidiCcBindingLibrary()
    fake.malformed_list = True
    monkeypatch.setattr(_engine_midi, "_get_lib", lambda: fake)
    engine = _bare_engine(1)
    with pytest.raises(SonareError) as error:
        engine.midi_cc_bindings()
    assert error.value.code == ErrorCode.INVALID_FORMAT
    assert fake.free_calls == 1
    engine._handle = None


_INACTIVE_CC_FIELDS = [
    ("cc_lsb_number", "control-change 14"),
    ("selector_msb", "RPN or NRPN"),
    ("selector_lsb", "RPN or NRPN"),
]


@pytest.mark.parametrize(("field", "kind"), _INACTIVE_CC_FIELDS)
def test_engine_bind_midi_cc_refuses_a_nonzero_inactive_field(field: str, kind: str) -> None:
    binding = MidiCcBinding(cc_number=7, param_id=11, **{field: 1})
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(SonareValueError) as error:
            engine.bind_midi_cc(binding)
        assert str(error.value).endswith(f"binding.{field} must be 0 unless binding.kind is {kind}")
        assert engine.midi_cc_bindings() == []


@pytest.mark.parametrize(("field", "kind"), _INACTIVE_CC_FIELDS)
def test_midi_cc_conversions_refuse_a_nonzero_inactive_field(field: str, kind: str) -> None:
    good = MidiCcBinding(cc_number=7, param_id=5)
    bad = MidiCcBinding(cc_number=7, param_id=5, **{field: 1})
    expected = f"bindings[1].{field} must be 0 unless bindings[1].kind is {kind}"
    event = ProjectMidiEvent(ppq=0.0, word0=0x20B00740)
    with pytest.raises(SonareValueError) as error:
        midi_cc_to_breakpoint([good, bad], event)
    assert str(error.value).endswith(expected)
    with pytest.raises(SonareValueError) as error:
        midi_param_to_cc([good, bad], 5, 0.5, 0)
    assert str(error.value).endswith(expected)


def test_a_cc_binding_mapping_is_a_type_error() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(TypeError, match="MidiCcBinding"):
            engine.bind_midi_cc({"cc_number": 2, "param_id": 43})  # type: ignore[arg-type]
        assert engine.midi_cc_bindings() == []


@pytest.mark.parametrize("data", [10, [1, 2]])
def test_engine_sound_font_refuses_non_bytes_like_data_before_native_call(data: object) -> None:
    engine = RealtimeEngine.__new__(RealtimeEngine)
    with pytest.raises(TypeError, match="data must be bytes-like"):
        engine.load_sound_font(data)  # type: ignore[arg-type]


def test_engine_set_and_clear_midi_fx_round_trips() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_midi_fx(0, {"transposeSemitones": 12})
        engine.clear_midi_fx(0)
        with pytest.raises(TypeError, match="json.loads"):
            engine.set_midi_fx(0, '{"transposeSemitones":12}')  # type: ignore[arg-type]


def test_engine_live_midi_input_source_queue() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        # The input family is refused until the input source is enabled.
        with pytest.raises(SonareError) as disabled:
            engine.push_midi_input_pitch_bend(0, 0, 16383, 0)
        assert disabled.value.code == int(ErrorCode.INVALID_STATE)
        engine.set_midi_input_source(0)
        engine.push_midi_input_note_on(0, 0, 60, 100, 0)
        engine.push_midi_input_cc(0, 0, 1, 64, 0)
        engine.push_midi_input_pitch_bend(0, 0, 16383, 0)
        engine.push_midi_input_channel_pressure(0, 0, 127, 0)
        engine.push_midi_input_poly_pressure(0, 0, 60, 96, 0)
        engine.push_midi_input_note_off(0, 0, 60, 0, 0)
        assert engine.midi_input_pending_count == 6
        engine.clear_midi_input_source()


def test_engine_push_immediate_notes_do_not_raise() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        engine.push_midi_note_on(0, 0, 0, 60, 100)
        engine.push_midi_note_off(0, 0, 0, 60, 0)


def test_engine_push_midi_bend_and_pressure_change_rendered_audio() -> None:
    sample_rate = 48000
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
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        engine.push_midi_pitch_bend(0, 0, 0, 16383)
        with pytest.raises(SonareValueError, match=r"bend14 must be in \[0, 16383\]"):
            engine.push_midi_pitch_bend(0, 0, 0, 16384)
        engine.set_midi_input_source(0)
        with pytest.raises(SonareValueError, match=r"bend14 must be in \[0, 16383\]"):
            engine.push_midi_input_pitch_bend(0, 0, 16384, 0)
        for pressure in (128, 255):
            with pytest.raises(SonareValueError, match="pressure must be in"):
                engine.push_midi_channel_pressure(0, 0, 0, pressure)


def test_engine_push_midi_sysex_accepts_frame_and_rejects_oversized() -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_builtin_instrument(BuiltinSynthConfig(), 0)
        # GM system-on universal SysEx (0xF0 ... 0xF7).
        engine.push_midi_sysex(0, b"\xf0\x7e\x7f\x09\x01\xf7")
        engine.process([[0.0] * 128, [0.0] * 128])
        with pytest.raises(SonareError):
            engine.push_midi_sysex(0, b"\xf0" + b"\x00" * 1022 + b"\xf7")
        with pytest.raises(ValueError):
            engine.push_midi_sysex(0, b"")


def test_engine_scheduled_midi_clips_render_builtin_instrument() -> None:
    def midi1_word(status: int, channel: int, word0: int, word1: int) -> int:
        return (0x2 << 28) | ((status & 0xF) << 20) | ((channel & 0xF) << 16) | (word0 << 8) | word1

    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
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
        with pytest.raises(SonareValueError, match=r"channel must be in \[0, 15\]"):
            engine.push_midi_note_on(6, 0, 16, 60, 100)
        with pytest.raises(SonareError) as bad_soundfont_error:
            engine.load_sound_font(b"not sf2")
        assert bad_soundfont_error.value.code == 2
        engine.set_midi_clips([])


@pytest.mark.parametrize("word_count", [-1, 5, 256])
def test_engine_midi_clip_word_count_only_zero_infers(word_count: int) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        engine.set_midi_destination_external(5, True)
        note_on = _midi1_word(0x9, 0, 60, 100)
        valid = EngineMidiClipSchedule(
            id=8,
            track_id=5,
            destination_id=5,
            length_samples=256,
            events=[EngineMidiEvent(0, word0=note_on, word_count=0)],
        )
        engine.set_midi_clips([valid])

        invalid = EngineMidiClipSchedule(
            id=9,
            track_id=5,
            destination_id=5,
            length_samples=256,
            events=[
                EngineMidiEvent(
                    0,
                    word0=_midi1_word(0x9, 0, 61, 100),
                    word_count=word_count,
                )
            ],
        )
        with pytest.raises(SonareValueError) as refused:
            engine.set_midi_clips([invalid])
        assert refused.value.code == ErrorCode.INVALID_PARAMETER
        assert str(refused.value) == (
            "RealtimeEngine.set_midi_clips: clips[0].events[0].word_count "
            "must be an integer in [0, 4]"
        )

        # A rejected replacement leaves the previously published, inferred event in force.
        engine.play()
        engine.process([[0.0] * 128, [0.0] * 128])
        drained = engine.drain_external_midi()
        assert len(drained) == 1
        assert drained[0].bytes[1] == 60


def test_engine_drains_external_midi_routing_to_host() -> None:
    with RealtimeEngine(
        sample_rate=48000, max_block_size=128, command_capacity=16, telemetry_capacity=16
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
        assert engine.external_midi_dropped_count == 0


def test_engine_clip_event_group_comes_from_word0() -> None:
    with RealtimeEngine(
        sample_rate=48000, max_block_size=128, command_capacity=16, telemetry_capacity=16
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

        # Stopping resets the played channel and releases no note.
        engine.stop()
        engine.process([[0.0] * 128, [0.0] * 128])
        assert [event.bytes for event in engine.drain_external_midi()] == [
            bytes([0xB0, 64, 0]),
            bytes([0xB0, 121, 0]),
            bytes([0xB0, 123, 0]),
            bytes([0xE0, 0, 64]),
        ]


def test_engine_external_destination_table_overflow() -> None:
    with RealtimeEngine(
        sample_rate=48000, max_block_size=128, command_capacity=16, telemetry_capacity=16
    ) as engine:
        for dest_id in range(16):
            engine.set_midi_destination_external(dest_id, True)
        # Re-marking an existing destination is idempotent.
        engine.set_midi_destination_external(3, True)
        # The 17th distinct destination overflows the slot table.
        with raises_coded(ErrorCode.INVALID_STATE):
            engine.set_midi_destination_external(99, True)
        # Freeing a slot lets the next mark succeed.
        engine.set_midi_destination_external(3, False)
        engine.set_midi_destination_external(99, True)


def test_engine_drain_external_midi_honors_max_records_cap_losslessly() -> None:
    # Regression: drain must treat max_records as an output-event cap (not drain
    # the whole queue), and must not lose events past the cap — they stay queued.
    with RealtimeEngine(
        sample_rate=48000, max_block_size=128, command_capacity=16, telemetry_capacity=16
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
        assert engine.external_midi_dropped_count == 0
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
        sample_rate=48000, max_block_size=128, command_capacity=16, telemetry_capacity=16
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
        assert engine.external_midi_dropped_count == 0


def test_engine_forwards_midi_clock_transport_to_external_queue() -> None:
    with RealtimeEngine(
        sample_rate=48000, max_block_size=24000, command_capacity=16, telemetry_capacity=16
    ) as engine:
        engine.set_tempo(120.0)
        engine.set_external_midi_clock_enabled(True)
        engine.play(0)
        engine.process([[0.0] * 24000, [0.0] * 24000])

        # One Start plus 24 clock ticks (one every 1000 samples at 120 BPM).
        drained = engine.drain_external_midi()
        assert len(drained) == 25
        for event in drained:
            assert event.destination_id is None
            assert len(event.bytes) == 1
        assert drained[0].bytes[0] == 0xFA  # Start
        assert drained[1].bytes[0] == 0xF8  # Clock


def test_engine_set_midi_clips_forwards_explicit_destination_id_zero() -> None:
    # Regression: an explicit destination_id=0 must be forwarded unchanged, not
    # silently rewritten to track_id. Only the instrument at destination 0 is
    # bound, so a rewrite to track_id would leave the note unheard (silence).
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
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
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
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
        sample_rate=48000, max_block_size=128, command_capacity=16, telemetry_capacity=16
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


# Each call holds a value that fits its C type and lies outside the MIDI domain, so only the
# live push argument rule can refuse it; the input source stays disabled because an argument
# is judged before the source state.
_LIVE_PUSH_REFUSALS = [
    ("push_midi_note_on", (0, 0, 16, 60, 100), "channel must be in [0, 15]"),
    ("push_midi_note_off", (0, 0, 0, 60, 128), "velocity must be in [0, 127]"),
    ("push_midi_cc", (0, 0, 0, 7, 128), "value must be in [0, 127]"),
    ("push_midi_pitch_bend", (0, 0, 0, 16384), "bend14 must be in [0, 16383]"),
    ("push_midi_channel_pressure", (0, 0, 0, 128), "pressure must be in [0, 127]"),
    ("push_midi_poly_pressure", (0, 0, 0, 128, 64), "note must be in [0, 127]"),
    ("push_midi_program", (0, 0, 0, 0, True, 128, 0), "bank_msb must be in [0, 127]"),
    (
        "push_midi_program",
        (0, 0, 0, 0, False, 0, 1),
        "bank MSB and LSB must be 0 when bank-valid is false",
    ),
    ("push_midi_ump", (0, [0x30000000, 0]), "words do not form a pushable UMP message"),
    ("push_midi_input_note_on", (16, 0, 60, 100), "group must be in [0, 15]"),
    ("push_midi_input_note_off", (0, 0, 128, 0), "note must be in [0, 127]"),
    ("push_midi_input_cc", (0, 0, 128, 0), "controller must be in [0, 127]"),
    ("push_midi_input_pitch_bend", (0, 0, 16384), "bend14 must be in [0, 16383]"),
    ("push_midi_input_channel_pressure", (0, 16, 64), "channel must be in [0, 15]"),
    ("push_midi_input_poly_pressure", (0, 0, 60, 128), "pressure must be in [0, 127]"),
    ("push_midi_input_program", (0, 0, 128), "program must be in [0, 127]"),
    ("push_midi_input_program", (0, 0, 0, True, 0, 128), "bank_lsb must be in [0, 127]"),
    ("push_midi_input_ump", ([0x40903C00],), "words do not form a pushable UMP message"),
]


@pytest.mark.parametrize("method,args,message", _LIVE_PUSH_REFUSALS)
def test_live_push_names_the_refused_argument(method: str, args: tuple, message: str) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(SonareValueError) as refused:
            getattr(engine, method)(*args)
        assert str(refused.value) == f"RealtimeEngine.{method}: {message}"
        assert refused.value.code == int(ErrorCode.INVALID_PARAMETER)


_LIVE_INPUT_PUSHES = [
    ("push_midi_input_note_on", (0, 0, 60, 100)),
    ("push_midi_input_note_off", (0, 0, 60, 0)),
    ("push_midi_input_cc", (0, 0, 7, 100)),
    ("push_midi_input_pitch_bend", (0, 0, 8192)),
    ("push_midi_input_channel_pressure", (0, 0, 64)),
    ("push_midi_input_poly_pressure", (0, 0, 60, 64)),
    ("push_midi_input_ump", ([0x40903C00, 0x80000000],)),
    ("push_midi_input_program", (0, 0, 5, True, 8, 0)),
]


_LIVE_PUSH_SHAPE_BEFORE_DOMAIN = [
    ("push_midi_cc", (0, 256, object(), 7, 64), "channel"),
    ("push_midi_program", (0, 256, object(), 0, True, 0, 0), "channel"),
    ("push_midi_input_note_on", (256, object(), 60, 100), "channel"),
    ("push_midi_input_note_off", (256, object(), 60, 0), "channel"),
    ("push_midi_input_cc", (256, object(), 7, 64), "channel"),
    ("push_midi_input_pitch_bend", (256, object(), 8192), "channel"),
    ("push_midi_input_channel_pressure", (256, object(), 64), "channel"),
    ("push_midi_input_poly_pressure", (256, object(), 60, 64), "channel"),
    ("push_midi_input_program", (256, object(), 0), "channel"),
    ("push_midi_note_on", (0, 256, object(), 60, 100), "channel"),
    ("push_midi_note_off", (0, 256, object(), 60, 0), "channel"),
    ("push_midi_pitch_bend", (0, 256, object(), 8192), "channel"),
    ("push_midi_channel_pressure", (0, 256, object(), 64), "channel"),
    ("push_midi_poly_pressure", (0, 256, object(), 60, 64), "channel"),
    ("push_midi_program", (0, 16, 0, 0, 0, 0, 0), "bank_valid"),
    ("push_midi_ump", (2**32, [object()]), "words[0]"),
    ("push_midi_input_ump", ([2**32, object()],), "words[1]"),
]


@pytest.mark.parametrize("method,args,field", _LIVE_PUSH_SHAPE_BEFORE_DOMAIN)
def test_live_push_checks_shape_before_domain(method: str, args: tuple, field: str) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(TypeError) as refused:
            getattr(engine, method)(*args)
        expected = (
            f"RealtimeEngine.{method}: {field} must be a bool"
            if field == "bank_valid"
            else f"RealtimeEngine.{method}: {field} must be an integer"
        )
        assert str(refused.value) == expected


def test_push_midi_sysex_rejects_non_buffer_inputs_before_earlier_domain() -> None:
    engine = RealtimeEngine(sample_rate=48000, max_block_size=128)
    try:
        for data in ([0xF0, 0xF7], 2):
            with pytest.raises(TypeError) as refused:
                engine.push_midi_sysex(2**32, data)  # type: ignore[arg-type]
            assert str(refused.value) == (
                "RealtimeEngine.push_midi_sysex: data must be bytes, bytearray or memoryview"
            )
        for data in (b"\xf0\xf7", bytearray(b"\xf0\xf7"), memoryview(b"\xf0\xf7")):
            with pytest.raises(SonareValueError) as refused:
                engine.push_midi_sysex(2**32, data)
            assert str(refused.value) == (
                "RealtimeEngine.push_midi_sysex: destination_id must be an integer in "
                "[0, 4294967295]"
            )
    finally:
        engine.close()


@pytest.mark.parametrize("method,args", _LIVE_INPUT_PUSHES)
def test_live_input_push_refuses_a_disabled_source_as_invalid_state(
    method: str, args: tuple
) -> None:
    with RealtimeEngine(sample_rate=48000, max_block_size=128) as engine:
        with pytest.raises(SonareError) as refused:
            getattr(engine, method)(*args)
        assert refused.value.code == int(ErrorCode.INVALID_STATE)
        assert str(refused.value) == (
            f"[7] RealtimeEngine.{method}: the MIDI input source is not enabled"
        )
