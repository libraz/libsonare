"""Tests for the editing DSP wrappers (pitch correct, note stretch, voice change)."""

import inspect
import json
import math
from pathlib import Path

import numpy as np
import pytest

import libsonare
from libsonare import Audio, ErrorCode

from ._helpers import raises_coded


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

    detected = libsonare.pitch_pyin(
        result, sample_rate=sr, frame_length=2048, hop_length=512, f0_min_hz=100.0, f0_max_hz=1000.0
    )
    expected_hz = 220.0 * 2 ** (3 / 12)
    cents_error = 1200.0 * math.log2(detected.median_f0 / expected_hz)
    assert abs(cents_error) < 5.0


def test_pitch_correct_to_midi_rejects_out_of_range() -> None:
    sr = 22050
    samples = _tone(sr)

    for current, target in ((57.0, 200.0), (57.0, -1.0), (-1.0, 60.0), (200.0, 60.0)):
        with raises_coded():
            libsonare.pitch_correct_to_midi(
                samples, sample_rate=sr, current_midi=current, target_midi=target
            )


def test_pitch_correct_timevarying_follows_a_midi_target() -> None:
    sr = 22050
    samples = _tone(sr, freq=220.0)
    hop = 512
    n_frames = len(samples) // hop + 1
    # A constant 220 Hz contour corrected toward MIDI 60; every frame voiced.
    f0 = [220.0] * n_frames

    result = libsonare.pitch_correct_timevarying(
        samples, f0_hz=f0, target_midi=60.0, sample_rate=sr, hop_length=hop
    )
    assert len(result) == len(samples)
    assert all(math.isfinite(x) for x in result)

    # Explicit voiced flags / probabilities are accepted.
    voiced = [1] * n_frames
    voiced_prob = [1.0] * n_frames
    result2 = libsonare.pitch_correct_timevarying(
        samples,
        f0_hz=f0,
        target_midi=60.0,
        sample_rate=sr,
        hop_length=hop,
        voiced=voiced,
        voiced_prob=voiced_prob,
    )
    assert len(result2) == len(samples)

    # pYIN emits NaN F0 for unvoiced frames by default; this canonical track
    # representation must pass directly into correction.
    f0[0] = math.nan
    voiced[0] = 0
    pyin_result = libsonare.pitch_correct_timevarying(
        samples,
        f0_hz=f0,
        target_midi=60.0,
        sample_rate=sr,
        hop_length=hop,
        voiced=voiced,
        voiced_prob=voiced_prob,
    )
    assert all(math.isfinite(x) for x in pyin_result)

    # Mismatched companion-array lengths are rejected before the native call.
    with pytest.raises(ValueError):
        libsonare.pitch_correct_timevarying(
            samples, f0_hz=f0, target_midi=60.0, sample_rate=sr, hop_length=hop, voiced=[1, 0]
        )


def test_pitch_correct_timevarying_derives_voicing_from_voiced_prob() -> None:
    """When ``voiced`` is omitted, ``voiced_prob`` decides (>= 0.5 is voiced)
    instead of every frame defaulting to voiced regardless of what
    voiced_prob says.

    Driven through pYIN's own NaN-for-unvoiced convention: the core accepts a
    NaN F0 frame only when that frame is NOT treated as voiced, so acceptance
    or rejection of an all-NaN track is a direct, deterministic readout of the
    voicing decision the implementation actually made.
    """
    sr = 22050
    samples = _tone(sr)
    hop = 512
    n_frames = len(samples) // hop + 1
    f0 = [math.nan] * n_frames

    # Below 0.5: every frame reads as unvoiced, so the canonical unvoiced NaN
    # representation is accepted -- this used to fail with voiced forced true.
    libsonare.pitch_correct_timevarying(
        samples,
        f0_hz=f0,
        target_midi=60.0,
        sample_rate=sr,
        hop_length=hop,
        voiced_prob=[0.1] * n_frames,
    )
    # At/above 0.5: every frame reads as voiced, so a voiced frame carrying
    # NaN (no measured pitch) is rejected, the same as an explicit voiced=1.
    for threshold in (0.9, 0.5):
        with raises_coded():
            libsonare.pitch_correct_timevarying(
                samples,
                f0_hz=f0,
                target_midi=60.0,
                sample_rate=sr,
                hop_length=hop,
                voiced_prob=[threshold] * n_frames,
            )
    # voiced_prob absent too: every frame defaults to voiced, so NaN is
    # rejected exactly like the all-voiced case above.
    with raises_coded():
        libsonare.pitch_correct_timevarying(
            samples, f0_hz=f0, target_midi=60.0, sample_rate=sr, hop_length=hop
        )


def test_pitch_correction_explicit_voiced_ignores_invalid_probability_shape_and_values() -> None:
    """Explicit voiced flags take precedence over a companion probability track."""
    sr = 22050
    samples = _tone(sr)
    hop = 512
    n_frames = len(samples) // hop + 1
    f0 = [220.0] * n_frames
    voiced = [1] * n_frames

    general_without_prob = libsonare.pitch_correct_timevarying(
        samples, f0_hz=f0, sample_rate=sr, hop_length=hop, voiced=voiced
    )

    # A supplied probability track is ignored completely once voiced is present,
    # including its length and element-domain checks.
    for invalid_prob in ([math.nan] * (n_frames + 1), [-0.1] * n_frames, [1.1] * n_frames):
        general_with_invalid_prob = libsonare.pitch_correct_timevarying(
            samples,
            f0_hz=f0,
            sample_rate=sr,
            hop_length=hop,
            voiced=voiced,
            voiced_prob=invalid_prob,
        )
        assert list(general_with_invalid_prob) == list(general_without_prob)

    # Without explicit flags, the probability track still owns the [0,1] and
    # finite-value contract.
    for invalid in (math.nan, -0.1, 1.1):
        with raises_coded():
            libsonare.pitch_correct_timevarying(
                samples,
                f0_hz=f0,
                sample_rate=sr,
                hop_length=hop,
                voiced_prob=[invalid] * n_frames,
            )


def test_pitch_correct_timevarying_voiced_accepts_any_int_sequence() -> None:
    """The voiced flags marshal in bulk, so every int-like sequence agrees.

    They used to go through ``(c_int32 * n)(*seq)``, which unpacks each frame
    as a Python vararg -- the one per-element hop left on an otherwise
    vectorised call. Bulk marshalling has to accept exactly what the
    element-wise loop did, so a list, a NumPy int array and a NumPy bool array
    describing the same frames must produce the same audio.
    """
    np = pytest.importorskip("numpy")
    sr = 22050
    samples = _tone(sr, freq=220.0)
    hop = 512
    n_frames = len(samples) // hop + 1
    f0 = [220.0] * n_frames
    pattern = [1, 0] * (n_frames // 2) + [1] * (n_frames % 2)

    forms = [
        pattern,
        np.array(pattern, dtype=np.int32),
        np.array(pattern, dtype=bool),
        [bool(flag) for flag in pattern],
    ]
    results = [
        libsonare.pitch_correct_timevarying(
            samples, f0_hz=f0, target_midi=60.0, sample_rate=sr, hop_length=hop, voiced=form
        )
        for form in forms
    ]
    for other in results[1:]:
        np.testing.assert_array_equal(other, results[0])

    # The companion-length check still runs after the bulk conversion.
    with pytest.raises(ValueError):
        libsonare.pitch_correct_timevarying(
            samples,
            f0_hz=f0,
            sample_rate=sr,
            hop_length=hop,
            voiced=np.ones(n_frames + 2, dtype=np.int32),
        )


def test_pitch_correct_timevarying_scale_and_knobs() -> None:
    sr = 22050
    samples = _tone(sr, freq=220.0)
    hop = 512
    n_frames = len(samples) // hop + 1
    f0 = [220.0] * n_frames

    # NULL-equivalent defaults behave like the fixed-MIDI path.
    base = libsonare.pitch_correct_timevarying(samples, f0_hz=f0, sample_rate=sr, hop_length=hop)
    assert len(base) == len(samples)
    assert all(math.isfinite(x) for x in base)

    # Scale mode snaps the contour to the configured key.
    scaled = libsonare.pitch_correct_timevarying(
        samples, f0_hz=f0, sample_rate=sr, hop_length=hop, target_mode="scale", scale_root=0
    )
    assert len(scaled) == len(samples)
    assert all(math.isfinite(x) for x in scaled)

    # A lower retune_amount produces a different correction than a full snap.
    full = libsonare.pitch_correct_timevarying(
        samples, f0_hz=f0, sample_rate=sr, hop_length=hop, target_midi=59.0, retune_amount=1.0
    )
    gentle = libsonare.pitch_correct_timevarying(
        samples, f0_hz=f0, sample_rate=sr, hop_length=hop, target_midi=59.0, retune_amount=0.25
    )
    assert any(abs(a - b) > 1e-6 for a, b in zip(full, gentle, strict=True))

    # Out-of-range knobs are rejected.
    with raises_coded():
        libsonare.pitch_correct_timevarying(
            samples, f0_hz=f0, sample_rate=sr, hop_length=hop, retune_amount=2.0
        )
    with raises_coded():
        libsonare.pitch_correct_timevarying(
            samples, f0_hz=f0, sample_rate=sr, hop_length=hop, target_midi=200.0
        )
    with pytest.raises(ValueError, match=" must be one of "):
        libsonare.pitch_correct_timevarying(
            samples, f0_hz=f0, sample_rate=sr, hop_length=hop, target_mode="Scale"
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


def test_note_move_function() -> None:
    sr = 22050
    samples = _tone(sr)
    result = libsonare.note_move(
        samples, sample_rate=sr, onset_sample=2000, offset_sample=6000, target_onset_sample=4000
    )
    assert len(result) == len(samples)
    assert all(math.isfinite(x) for x in result)


def test_note_editing_defaults_offset_to_input_length() -> None:
    sr = 22050
    samples = _tone(sr)
    stretched = libsonare.note_stretch(samples, sample_rate=sr)
    moved = libsonare.note_move(samples, sample_rate=sr)
    assert len(stretched) > 0
    assert len(moved) == len(samples)


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


def test_voice_change_formant_mode() -> None:
    sr = 22050
    samples = _tone(sr)

    default = libsonare.voice_change(
        samples, sample_rate=sr, pitch_semitones=3.0, formant_factor=1.1
    )
    relative = libsonare.voice_change(
        samples, sample_rate=sr, pitch_semitones=3.0, formant_factor=1.1, formant_mode="relative"
    )
    assert list(default) == list(relative)

    absolute = libsonare.voice_change(
        samples, sample_rate=sr, pitch_semitones=3.0, formant_factor=1.1, formant_mode="absolute"
    )
    assert len(absolute) == len(relative)
    assert list(absolute) != list(relative)

    # No shift and unity factor: nothing is warped, so the input comes back.
    identity = libsonare.voice_change(samples, sample_rate=sr, formant_mode="absolute")
    assert list(identity) == list(np.asarray(samples, dtype=np.float32))


def test_voice_change_absolute_mode_refuses_unreachable_factor() -> None:
    sr = 22050
    samples = _tone(sr)
    # 2**(4/12) = 1.2599, so the warp range [0.55, 1.65] reaches [0.693, 2.079].
    with pytest.raises(libsonare.SonareValueError, match=r"\[0\.693, 2\.079\]"):
        libsonare.voice_change(
            samples,
            sample_rate=sr,
            pitch_semitones=4.0,
            formant_factor=2.5,
            formant_mode="absolute",
        )
    with pytest.raises(libsonare.SonareValueError, match="formant_mode"):
        libsonare.voice_change(samples, sample_rate=sr, formant_mode="Absolute")


def test_realtime_voice_changer_function_and_class() -> None:
    sr = 22050
    samples = _tone(sr, duration=0.1)

    result = libsonare.voice_change_realtime(samples, sample_rate=sr, config="brightIdol")
    # numpy fast path: results are returned as numpy arrays (Sequence-compatible).
    assert hasattr(result, "__len__")
    assert hasattr(result, "__iter__")
    assert len(result) == len(samples)
    assert all(math.isfinite(x) for x in result)

    with libsonare.RealtimeVoiceChanger(
        sr, max_block_size=128, num_channels=1, config="softWhisper"
    ) as changer:
        block = samples[:128]
        out = changer.process_mono(block)
        assert len(out) == len(block)
        assert changer.latency_samples > 0
        # config_json must reflect the active preset (B-1 regression).
        cfg = changer.config_json()
        assert isinstance(cfg, str) and cfg.startswith("{")
        assert "schemaVersion" in cfg
        changer.set_config("deepNarrator")
        cfg2 = changer.config_json()
        assert cfg2 != cfg  # config text must change after set_config
        changer.reset()

    names = libsonare.realtime_voice_changer_preset_names()
    assert "robotMascot" in names
    preset_json = libsonare.realtime_voice_changer_preset_json("brightIdol")
    assert "brightIdol" in preset_json
    assert libsonare.validate_realtime_voice_changer_preset_json(preset_json)["ok"] is True
    # Empty object lacks required schemaVersion/id/name/dsp — must be rejected.
    assert libsonare.validate_realtime_voice_changer_preset_json("{}")["ok"] is False


def test_voice_character_preset_id_round_trips() -> None:
    from typing import get_args

    ids = get_args(libsonare.VoicePresetId)
    # Every ordinal resolves to the id at that position of VoicePresetId.
    for ordinal, name in enumerate(ids):
        assert libsonare.voice_character_preset_id(ordinal) == name

    # Out-of-range ordinals resolve to None per the wrapper contract.
    assert libsonare.voice_character_preset_id(-1) is None
    assert libsonare.voice_character_preset_id(len(ids)) is None


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
        interleaved, sample_rate=sr, num_channels=2, config="brightIdol"
    )
    assert len(result) == len(interleaved)
    assert all(math.isfinite(x) for x in result)
    # Identical L/R input must produce identical L/R output (per-channel state
    # is initialized identically and the reverb seed is shared per instance).
    for i in range(n):
        assert result[2 * i] == result[2 * i + 1]


def test_realtime_voice_changer_rejects_odd_interleaved_stereo_length() -> None:
    with pytest.raises(ValueError, match="samples length must be a multiple of num_channels"):
        libsonare.voice_change_realtime([0.0, 0.1, 0.2], num_channels=2, sample_rate=48000)


@pytest.mark.parametrize("num_channels", [0, 3])
def test_realtime_voice_changer_refuses_a_channel_count_outside_mono_and_stereo(
    num_channels: int,
) -> None:
    with pytest.raises(ValueError, match=r"num_channels must be an integer in \[1, 2\]"):
        libsonare.voice_change_realtime([0.0] * 6, num_channels=num_channels, sample_rate=48000)


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

        realtime_changed = audio.voice_change_realtime(config="brightIdol")
        assert len(realtime_changed) == len(samples)
    finally:
        audio.close()


# ---------------------------------------------------------------------------
# Error-path tests for RealtimeVoiceChanger
# ---------------------------------------------------------------------------


def test_realtime_voice_changer_invalid_sample_rate() -> None:
    """Negative or zero sample_rate must be rejected at construction."""

    with raises_coded():
        libsonare.RealtimeVoiceChanger(-1, num_channels=1, config="neutralMonitor")

    with raises_coded():
        libsonare.RealtimeVoiceChanger(0, num_channels=1, config="neutralMonitor")


def test_realtime_voice_changer_invalid_channels() -> None:
    """channels > 2 must be rejected at construction."""

    with raises_coded():
        libsonare.RealtimeVoiceChanger(48000, num_channels=3, config="neutralMonitor")


def test_realtime_voice_changer_invalid_max_block_size() -> None:
    """max_block_size <= 0 must be rejected at construction."""

    with raises_coded():
        libsonare.RealtimeVoiceChanger(
            48000, max_block_size=-1, num_channels=1, config="neutralMonitor"
        )

    with raises_coded():
        libsonare.RealtimeVoiceChanger(
            48000, max_block_size=0, num_channels=1, config="neutralMonitor"
        )


def test_realtime_voice_changer_invalid_preset_id() -> None:
    """An unrecognised preset id is an argument refusal at construction."""

    with pytest.raises(libsonare.SonareValueError, match="config must be one of "):
        libsonare.RealtimeVoiceChanger(48000, num_channels=1, config="nonexistent-preset-id-xyz")


def test_a_voice_changer_config_string_is_a_preset_id_and_a_mapping_is_a_document() -> None:
    """JSON text is not read as a document, and a value of any other type is a TypeError."""
    document = json.loads(libsonare.realtime_voice_changer_preset_json("deepNarrator"))
    with libsonare.RealtimeVoiceChanger(48000, num_channels=1, config=document) as changer:
        changer.set_config("brightIdol")
        changer.set_config(document)
        with pytest.raises(libsonare.SonareValueError, match="config must be one of "):
            changer.set_config(json.dumps(document))
        with pytest.raises(TypeError, match="json.loads"):
            changer.set_config(3)  # type: ignore[arg-type]
    with pytest.raises(libsonare.SonareValueError, match="config must be one of "):
        libsonare.voice_change_realtime([0.0] * 64, sample_rate=48000, config=json.dumps(document))


def test_realtime_voice_changer_invalid_json_config() -> None:
    """The validator and the constructor read a preset document with one strict reader."""
    result = libsonare.validate_realtime_voice_changer_preset_json('{"malformed":"json structure"}')
    assert result["ok"] is False
    assert result["error"] == "unknown key malformed"
    with raises_coded(ErrorCode.INVALID_FORMAT, match="unknown key malformed"):
        libsonare.RealtimeVoiceChanger(
            48000, num_channels=1, config={"malformed": "json structure"}
        )
    # Non-JSON input must also be rejected.
    result_garbage = libsonare.validate_realtime_voice_changer_preset_json("not even json")
    assert result_garbage["ok"] is False


def test_realtime_voice_changer_close_then_process() -> None:
    """process_mono after close() must raise (NULL handle returns INVALID_PARAMETER)."""

    changer = libsonare.RealtimeVoiceChanger(
        48000, max_block_size=128, num_channels=1, config="neutralMonitor"
    )
    changer.close()
    with raises_coded(ErrorCode.INVALID_STATE):
        changer.process_mono([0.0] * 128)
    # Idempotent close should be a no-op even after handle is already null.
    changer.close()


def test_voice_change_realtime_default_preset_matches_every_entry_point() -> None:
    """One named operation, one default, wherever it is exposed.

    ``Audio.voice_change_realtime`` defaulted to ``"brightIdol"`` while the
    module function, the CLI, the Node and WASM facades and ordinal 0 of the C
    ABI's preset enum all defaulted to ``"neutralMonitor"``. The two Python
    entry points therefore rendered the same buffer audibly differently, with
    the handle form silently adding retune, a formant lift, EQ and reverb.
    """
    expected = json.loads(
        (Path(__file__).resolve().parents[3] / "tests/conformance/binding_defaults.json").read_text(
            encoding="utf-8"
        )
    )["defaults"]["voice_change_realtime.preset"]

    module_default = inspect.signature(libsonare.voice_change_realtime).parameters["config"].default
    handle_default = (
        inspect.signature(libsonare.Audio.voice_change_realtime).parameters["config"].default
    )
    assert module_default == expected
    assert handle_default == expected

    # Behavioural, not just declarative: the handle default must render what the
    # named default renders, so a future change to either cannot pass by editing
    # only the signature.
    sr = 22050
    samples = _tone(sr, duration=0.05)
    audio = libsonare.Audio.from_buffer(samples, sr)
    assert list(audio.voice_change_realtime()) == list(
        libsonare.voice_change_realtime(samples, sample_rate=sr, config=expected)
    )
