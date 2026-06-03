"""Tests for the editing DSP wrappers (pitch correct, note stretch, voice change)."""

import math

import pytest

import libsonare
from libsonare import Audio


def _tone(sample_rate: int = 22050, duration: float = 0.5, freq: float = 220.0) -> list[float]:
    """Synthesize a short sine tone."""
    count = int(sample_rate * duration)
    return [0.3 * math.sin(2 * math.pi * freq * i / sample_rate) for i in range(count)]


def test_pitch_correct_to_midi_function() -> None:
    sr = 22050
    samples = _tone(sr)

    result = libsonare.pitch_correct_to_midi(
        samples, sample_rate=sr, current_midi=57.0, target_midi=60.0
    )
    # numpy fast path: results are returned as numpy arrays (Sequence-compatible).
    assert hasattr(result, "__len__")
    assert hasattr(result, "__iter__")
    assert len(result) > 0
    assert all(math.isfinite(x) for x in result)


def test_pitch_correct_to_midi_timevarying_function() -> None:
    sr = 22050
    samples = _tone(sr, freq=220.0)
    hop = 512
    n_frames = len(samples) // hop + 1
    # A constant 220 Hz contour corrected toward MIDI 60; every frame voiced.
    f0 = [220.0] * n_frames

    result = libsonare.pitch_correct_to_midi_timevarying(
        samples, f0, target_midi=60.0, sample_rate=sr, hop_length=hop
    )
    assert len(result) == len(samples)
    assert all(math.isfinite(x) for x in result)

    # Explicit voiced flags / probabilities are accepted.
    voiced = [1] * n_frames
    voiced_prob = [1.0] * n_frames
    result2 = libsonare.pitch_correct_to_midi_timevarying(
        samples, f0, 60.0, sample_rate=sr, hop_length=hop, voiced=voiced, voiced_prob=voiced_prob
    )
    assert len(result2) == len(samples)

    # Mismatched companion-array lengths are rejected before the native call.
    with pytest.raises(ValueError):
        libsonare.pitch_correct_to_midi_timevarying(
            samples, f0, 60.0, sample_rate=sr, hop_length=hop, voiced=[1, 0]
        )


def test_note_stretch_function() -> None:
    sr = 22050
    samples = _tone(sr)

    result = libsonare.note_stretch(
        samples, sample_rate=sr, onset_sample=2000, offset_sample=6000, stretch_ratio=1.25
    )
    # numpy fast path: results are returned as numpy arrays (Sequence-compatible).
    assert hasattr(result, "__len__")
    assert hasattr(result, "__iter__")
    assert len(result) > 0
    assert all(math.isfinite(x) for x in result)


def test_voice_change_function() -> None:
    sr = 22050
    samples = _tone(sr)

    result = libsonare.voice_change(
        samples, sample_rate=sr, pitch_semitones=5.0, formant_factor=1.1
    )
    # numpy fast path: results are returned as numpy arrays (Sequence-compatible).
    assert hasattr(result, "__len__")
    assert hasattr(result, "__iter__")
    assert len(result) > 0
    assert all(math.isfinite(x) for x in result)


def test_realtime_voice_changer_function_and_class() -> None:
    sr = 22050
    samples = _tone(sr, duration=0.1)

    result = libsonare.voice_change_realtime(samples, sample_rate=sr, preset="bright-idol")
    # numpy fast path: results are returned as numpy arrays (Sequence-compatible).
    assert hasattr(result, "__len__")
    assert hasattr(result, "__iter__")
    assert len(result) == len(samples)
    assert all(math.isfinite(x) for x in result)

    with libsonare.RealtimeVoiceChanger(sr, "soft-whisper", max_block_size=128) as changer:
        block = samples[:128]
        out = changer.process_mono(block)
        assert len(out) == len(block)
        assert changer.latency_samples() > 0
        # config_json must reflect the active preset (B-1 regression).
        cfg = changer.config_json()
        assert isinstance(cfg, str) and cfg.startswith("{")
        assert "schemaVersion" in cfg
        changer.set_config("deep-narrator")
        cfg2 = changer.config_json()
        assert cfg2 != cfg  # config text must change after set_config
        changer.reset()

    names = libsonare.realtime_voice_changer_preset_names()
    assert "robot-mascot" in names
    preset_json = libsonare.realtime_voice_changer_preset_json("bright-idol")
    assert "bright-idol" in preset_json
    assert libsonare.validate_realtime_voice_changer_preset_json(preset_json)["ok"] is True
    # Empty object lacks required schemaVersion/id/name/dsp — must be rejected.
    assert libsonare.validate_realtime_voice_changer_preset_json("{}")["ok"] is False


def test_voice_character_preset_id_round_trips() -> None:
    from libsonare._effects import _VC_PRESET_NAME_TO_ORDINAL

    # Every canonical name maps to an ordinal that resolves back to the same id.
    for name, ordinal in _VC_PRESET_NAME_TO_ORDINAL.items():
        assert libsonare.voice_character_preset_id(ordinal) == name

    # Spot-check the documented example explicitly.
    bright_ordinal = _VC_PRESET_NAME_TO_ORDINAL["bright-idol"]
    assert libsonare.voice_character_preset_id(bright_ordinal) == "bright-idol"

    # Out-of-range ordinals resolve to None per the wrapper contract.
    assert libsonare.voice_character_preset_id(-1) is None
    assert libsonare.voice_character_preset_id(len(_VC_PRESET_NAME_TO_ORDINAL)) is None


def test_realtime_voice_changer_stereo_interleaved() -> None:
    sr = 22050
    # Two channels interleaved (LRLR…); same sine on both so output is symmetric.
    n = 1024
    mono = [0.2 * math.sin(2 * math.pi * 440.0 * i / sr) for i in range(n)]
    interleaved: list[float] = []
    for s in mono:
        interleaved.append(s)
        interleaved.append(s)

    result = libsonare.voice_change_realtime(
        interleaved, sample_rate=sr, preset="bright-idol", channels=2
    )
    assert len(result) == len(interleaved)
    assert all(math.isfinite(x) for x in result)
    # Identical L/R input must produce identical L/R output (per-channel state
    # is initialized identically and the reverb seed is shared per instance).
    for i in range(n):
        assert result[2 * i] == result[2 * i + 1]


def test_realtime_voice_changer_rejects_odd_interleaved_stereo_length() -> None:
    with pytest.raises(ValueError, match="interleaved stereo input length must be even"):
        libsonare.voice_change_realtime([0.0, 0.1, 0.2], channels=2)


def test_editing_audio_methods() -> None:
    sr = 22050
    samples = _tone(sr)

    audio = Audio.from_buffer(samples, sample_rate=sr)
    try:
        corrected = audio.pitch_correct_to_midi(current_midi=57.0, target_midi=60.0)
        assert len(corrected) > 0

        stretched = audio.note_stretch(onset_sample=2000, offset_sample=6000, stretch_ratio=1.25)
        assert len(stretched) > 0

        changed = audio.voice_change(pitch_semitones=5.0, formant_factor=1.1)
        assert len(changed) > 0

        realtime_changed = audio.voice_change_realtime("bright-idol")
        assert len(realtime_changed) == len(samples)
    finally:
        audio.close()


# ---------------------------------------------------------------------------
# Error-path tests for RealtimeVoiceChanger
# ---------------------------------------------------------------------------


def test_realtime_voice_changer_invalid_sample_rate() -> None:
    """Negative or zero sample_rate must be rejected at construction."""
    import pytest

    with pytest.raises((ValueError, RuntimeError)):
        libsonare.RealtimeVoiceChanger(-1, "neutral-monitor")

    with pytest.raises((ValueError, RuntimeError)):
        libsonare.RealtimeVoiceChanger(0, "neutral-monitor")


def test_realtime_voice_changer_invalid_channels() -> None:
    """channels > 2 must be rejected at construction."""
    import pytest

    with pytest.raises((ValueError, RuntimeError)):
        libsonare.RealtimeVoiceChanger(48000, "neutral-monitor", channels=3)


def test_realtime_voice_changer_invalid_max_block_size() -> None:
    """max_block_size <= 0 must be rejected at construction."""
    import pytest

    with pytest.raises((ValueError, RuntimeError)):
        libsonare.RealtimeVoiceChanger(48000, "neutral-monitor", max_block_size=-1)

    with pytest.raises((ValueError, RuntimeError)):
        libsonare.RealtimeVoiceChanger(48000, "neutral-monitor", max_block_size=0)


def test_realtime_voice_changer_invalid_preset_id() -> None:
    """An unrecognised preset string must be rejected at construction."""
    import pytest

    with pytest.raises((ValueError, RuntimeError)):
        libsonare.RealtimeVoiceChanger(48000, "nonexistent-preset-id-xyz")


def test_realtime_voice_changer_invalid_json_config() -> None:
    """The strict preset-pack validator must reject malformed JSON.

    The C++ constructor path (`realtime_voice_changer_config_from_json`) is
    intentionally lenient — unknown fields fall back to defaults rather than
    throwing. The strict gate is `validate_realtime_voice_changer_preset_json`
    which is what callers use when they need the schema enforced (e.g. when
    loading a third-party preset pack). Exercise THAT path here.
    """
    # Missing schemaVersion / id / dsp → strict validator must reject.
    result = libsonare.validate_realtime_voice_changer_preset_json('{"malformed":"json structure"}')
    assert result["ok"] is False
    assert "error" in result
    # Non-JSON input must also be rejected.
    result_garbage = libsonare.validate_realtime_voice_changer_preset_json("not even json")
    assert result_garbage["ok"] is False


def test_realtime_voice_changer_close_then_process() -> None:
    """process_mono after close() must raise (NULL handle returns INVALID_PARAMETER)."""
    import pytest

    changer = libsonare.RealtimeVoiceChanger(48000, "neutral-monitor", max_block_size=128)
    changer.close()
    with pytest.raises(RuntimeError):
        changer.process_mono([0.0] * 128)
    # Idempotent close should be a no-op even after handle is already null.
    changer.close()
