from __future__ import annotations

import ctypes
import gc
import os
import subprocess
import sys
import weakref
from pathlib import Path

import pytest

from libsonare import (
    LiveAudioDevice,
    LiveMidiDevice,
    LiveSession,
    RealtimeEngine,
    _engine_io,
    _live,
    _runtime,
)
from libsonare import engine as engine_module
from libsonare._errors import ErrorCode, SonareError, SonareValueError
from libsonare._ffi_types_core import (
    SonareLiveAudioDevice,
    SonareLiveAudioDeviceList,
    SonareLiveMidiDevice,
    SonareLiveMidiDeviceList,
    SonareLiveSessionConfig,
    SonareLiveSessionStatus,
)
from libsonare._ffi_types_mastering_project import SonareMidiEventList, SonareMidiEventPod


def _contents(pointer: object, record_type: type[ctypes.Structure]) -> ctypes.Structure:
    return ctypes.cast(pointer, ctypes.POINTER(record_type)).contents


class _FakeLib:
    def __init__(self) -> None:
        self.create_calls = 0
        self.destroyed_sessions: list[int] = []
        self.freed_audio_lists = 0
        self.freed_midi_lists = 0
        self.freed_event_lists = 0
        self.last_error = b"live device support is not compiled in"
        self.last_reason = b"notCompiledIn"
        self.realtime_reason_reads = 0
        self.create_rc = int(ErrorCode.OK)
        self.enumeration_rc = int(ErrorCode.OK)
        self._audio_rows: ctypes.Array[SonareLiveAudioDevice] | None = None
        self._midi_rows: ctypes.Array[SonareLiveMidiDevice] | None = None
        self._midi_events: ctypes.Array[SonareMidiEventPod] | None = None
        self._sysex = b"\xf0\x7d\x01\xf7"

    def sonare_live_session_config_init(self, pointer: object) -> None:
        config = _contents(pointer, SonareLiveSessionConfig)
        config.struct_size = ctypes.sizeof(SonareLiveSessionConfig)
        config.block_size = 128

    def sonare_live_audio_devices(self, pointer: object) -> int:
        if self.enumeration_rc != int(ErrorCode.OK):
            return self.enumeration_rc
        self._audio_rows = (SonareLiveAudioDevice * 1)()
        row = self._audio_rows[0]
        row.uid = b"device-1"
        row.name = "入力 device 🎵".encode()
        row.input_channels = 2
        row.output_channels = 2
        row.is_default_input = 1
        row.nominal_sample_rate = 48000.0
        out = _contents(pointer, SonareLiveAudioDeviceList)
        out.items = ctypes.cast(self._audio_rows, ctypes.POINTER(SonareLiveAudioDevice))
        out.count = 1
        return int(ErrorCode.OK)

    def sonare_free_live_audio_device_list(self, pointer: object) -> None:
        out = _contents(pointer, SonareLiveAudioDeviceList)
        self.freed_audio_lists += 1
        if self._audio_rows is not None:
            self._audio_rows[0].name = b"freed"
        out.items = None
        out.count = 0

    def sonare_live_midi_sources(self, pointer: object) -> int:
        self._midi_rows = (SonareLiveMidiDevice * 1)()
        self._midi_rows[0].uid = b"123456"
        self._midi_rows[0].name = b"Keyboard"
        out = _contents(pointer, SonareLiveMidiDeviceList)
        out.items = ctypes.cast(self._midi_rows, ctypes.POINTER(SonareLiveMidiDevice))
        out.count = 1
        return int(ErrorCode.OK)

    def sonare_free_live_midi_device_list(self, pointer: object) -> None:
        out = _contents(pointer, SonareLiveMidiDeviceList)
        self.freed_midi_lists += 1
        if self._midi_rows is not None:
            self._midi_rows[0].name = b"freed"
        out.items = None
        out.count = 0

    def sonare_live_session_create(
        self, _engine: object, config_pointer: object, out_pointer: object
    ) -> int:
        self.create_calls += 1
        config = _contents(config_pointer, SonareLiveSessionConfig)
        self.last_config = {
            "audio_output": int(config.audio_output),
            "audio_input": int(config.audio_input),
            "midi_input": int(config.midi_input),
            "sample_rate": int(config.sample_rate),
            "block_size": int(config.block_size),
            "destination_id": int(config.destination_id),
        }
        if self.create_rc != int(ErrorCode.OK):
            return self.create_rc
        ctypes.cast(out_pointer, ctypes.POINTER(ctypes.c_void_p))[0] = ctypes.c_void_p(456)
        return int(ErrorCode.OK)

    def sonare_live_session_destroy(self, handle: ctypes.c_void_p) -> None:
        self.destroyed_sessions.append(int(handle.value or 0))

    def sonare_live_session_status(self, _handle: object, out_pointer: object) -> int:
        out = _contents(out_pointer, SonareLiveSessionStatus)
        out.running = 1
        out.device_lost = 0
        out.rate_changed = 1
        out.sample_rate = 48000
        out.output_block_size = 128
        out.input_block_size = 64
        out.output_latency_ms = 2.5
        out.input_latency_ms = 1.25
        out.xrun_count = 3
        out.input_overrun_frames = 4
        out.input_underrun_frames = 5
        out.dropped_midi_events = 6
        return int(ErrorCode.OK)

    def sonare_live_session_captured_midi(self, _handle: object, out_pointer: object) -> int:
        self._midi_events = (SonareMidiEventPod * 2)()
        sysex_event = self._midi_events[0]
        sysex_event.ppq = 12.5
        sysex_event.word0 = 0x3004F07D
        sysex_event.word1 = 0x01F70000
        sysex_event.sysex = ctypes.cast(
            ctypes.c_char_p(self._sysex), ctypes.POINTER(ctypes.c_uint8)
        )
        sysex_event.sysex_size = len(self._sysex)
        voice_event = self._midi_events[1]
        voice_event.ppq = 13.0
        voice_event.word0 = 0x20903C64
        voice_event.word1 = 0
        out = _contents(out_pointer, SonareMidiEventList)
        out.items = ctypes.cast(self._midi_events, ctypes.POINTER(SonareMidiEventPod))
        out.count = 2
        return int(ErrorCode.OK)

    def sonare_free_midi_event_list(self, pointer: object) -> None:
        out = _contents(pointer, SonareMidiEventList)
        self.freed_event_lists += 1
        out.items = None
        out.count = 0

    def sonare_last_error_message(self) -> bytes:
        return self.last_error

    def sonare_last_error_reason(self) -> bytes:
        self.realtime_reason_reads += 1
        return self.last_reason

    def sonare_last_refusal_subject_count(self) -> int:
        return 0

    def sonare_error_message(self, _code: object) -> bytes:
        return b"Invalid state"

    def sonare_engine_destroy(self, _handle: object) -> None:
        return None

    def sonare_engine_process(self, *_args: object) -> int:
        return int(ErrorCode.INVALID_STATE)


@pytest.fixture
def fake_lib(monkeypatch: pytest.MonkeyPatch) -> _FakeLib:
    lib = _FakeLib()
    monkeypatch.setattr(_live, "_get_lib", lambda: lib)
    monkeypatch.setattr(_runtime, "_get_lib", lambda: lib)
    monkeypatch.setattr(engine_module, "_get_lib", lambda: lib)
    return lib


def _engine() -> RealtimeEngine:
    engine = object.__new__(RealtimeEngine)
    engine._handle = ctypes.c_void_p(123)
    return engine


def test_live_device_enumeration_copies_records_before_free(fake_lib: _FakeLib) -> None:
    audio = LiveSession.audio_devices()
    midi = LiveSession.midi_sources()

    assert audio == [LiveAudioDevice("device-1", "入力 device 🎵", 2, 2, True, False, 48000.0)]
    assert midi == [LiveMidiDevice("123456", "Keyboard")]
    assert fake_lib.freed_audio_lists == 1
    assert fake_lib.freed_midi_lists == 1
    assert audio[0].name == "入力 device 🎵"
    assert midi[0].name == "Keyboard"


def test_live_device_enumeration_stub_refusal_has_library_reason(fake_lib: _FakeLib) -> None:
    fake_lib.enumeration_rc = int(ErrorCode.NOT_SUPPORTED)

    with pytest.raises(SonareError) as error:
        LiveSession.audio_devices()

    assert error.value.code is ErrorCode.NOT_SUPPORTED
    assert error.value.reason == "notCompiledIn"
    assert "live device support is not compiled in" in str(error.value)
    assert fake_lib.freed_audio_lists == 1


def test_live_session_refuses_bad_selector_before_native_create(fake_lib: _FakeLib) -> None:
    engine = _engine()
    with pytest.raises(SonareValueError, match="audio_output"):
        LiveSession(engine, audio_output="missing")  # type: ignore[arg-type]

    with pytest.raises(SonareValueError, match="midi_input"):
        LiveSession(engine, midi_input="default")  # type: ignore[arg-type]
    assert fake_lib.create_calls == 0
    engine.close()


def test_live_session_stub_refusal_has_library_reason(fake_lib: _FakeLib) -> None:
    fake_lib.create_rc = int(ErrorCode.NOT_SUPPORTED)
    engine = _engine()

    with pytest.raises(SonareError) as error:
        LiveSession(engine, audio_output="default")

    assert error.value.code is ErrorCode.NOT_SUPPORTED
    assert error.value.reason == "notCompiledIn"
    assert "live device support is not compiled in" in str(error.value)
    assert fake_lib.create_calls == 1
    engine.close()


def test_live_session_status_midi_context_and_idempotent_close(fake_lib: _FakeLib) -> None:
    engine = _engine()
    engine_ref = weakref.ref(engine)
    session = LiveSession(
        engine,
        audio_output="default",
        audio_input=LiveAudioDevice("input-1", "Input", 2, 0, True, False, 48000.0),
        midi_input=LiveMidiDevice("123", "MIDI"),
        sample_rate=44100,
        destination_id=7,
    )
    del engine
    gc.collect()

    assert engine_ref() is not None
    assert fake_lib.last_config == {
        "audio_output": 1,
        "audio_input": 2,
        "midi_input": 2,
        "sample_rate": 44100,
        "block_size": 128,
        "destination_id": 7,
    }
    assert session.status().running is True
    assert session.status().output_latency_ms == 2.5
    events = session.captured_midi()
    assert len(events) == 2
    assert events[0].sysex == b"\xf0\x7d\x01\xf7"
    assert events[1].sysex is None
    assert fake_lib.freed_event_lists == 1

    with session as opened:
        assert opened is session
    session.close()
    assert fake_lib.destroyed_sessions == [456]
    assert fake_lib.freed_event_lists == 1
    with pytest.raises(SonareError) as error:
        session.status()
    assert error.value.code is ErrorCode.INVALID_STATE
    assert error.value.reason == "releasedHandle"
    engine_ref().close()


def test_realtime_process_attaches_device_driven_reason_without_tls(
    fake_lib: _FakeLib, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(_engine_io, "_get_lib", lambda: fake_lib)
    engine = _engine()

    with pytest.raises(SonareError) as error:
        engine.process([[0.0, 0.0]])

    assert error.value.code is ErrorCode.INVALID_STATE
    assert error.value.reason == "engineDeviceDriven"
    assert fake_lib.realtime_reason_reads == 0
    engine.close()


def test_import_does_not_load_library_or_enumerate_devices(tmp_path: Path) -> None:
    repo_root = Path(__file__).resolve().parents[3]
    environment = os.environ.copy()
    environment["PYTHONPATH"] = str(repo_root / "bindings" / "python" / "src")
    environment["SONARE_LIB_PATH"] = str(tmp_path / "missing-libsonare")
    result = subprocess.run(
        [sys.executable, "-c", "import libsonare"],
        cwd=repo_root,
        env=environment,
        capture_output=True,
        text=True,
        timeout=20,
        check=False,
    )

    assert result.returncode == 0, result.stderr


@pytest.mark.skipif(sys.platform == "darwin", reason="macOS can provide native live devices")
def test_native_compiled_out_library_refuses_live_sessions() -> None:
    from ._helpers import LIB_AVAILABLE

    if not LIB_AVAILABLE:
        pytest.skip("libsonare shared library is not available")

    with RealtimeEngine(48000) as engine:
        for entry, call in (
            ("LiveSession.audio_devices", LiveSession.audio_devices),
            ("LiveSession.midi_sources", LiveSession.midi_sources),
            ("LiveSession", lambda: LiveSession(engine, audio_output="default")),
        ):
            with pytest.raises(SonareError) as error:
                call()
            assert error.value.code is ErrorCode.NOT_SUPPORTED
            assert error.value.reason == "notCompiledIn"
            assert error.value.entry == entry
