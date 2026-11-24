"""Streaming mastering and advanced analyzer API tests."""

from __future__ import annotations

import math
import re

import numpy as np
import pytest

import libsonare

# ruff: noqa: F403,F405
from ._analyzer_helpers import *
from ._helpers import chain_unknown_key_text, raises_coded


def test_stream_analyzer_rejects_malformed_config_geometry() -> None:
    """StreamAnalyzer rejects malformed config geometry, matching every surface.

    The shared C++ constructor and the flat C ABI enforce the same relationship
    and positive-value contract, so Python construction raises instead of
    silently producing garbage spectra.
    """
    from libsonare import SonareError, StreamAnalyzer

    for bad in (
        dict(sample_rate=0),
        dict(sample_rate=22050, n_fft=0),
        dict(sample_rate=22050, n_mels=0),
        dict(sample_rate=22050, n_fft=1024, hop_length=2048),
        dict(sample_rate=22050, fmin_hz=8000.0, fmax_hz=4000.0),
        dict(sample_rate=22050, max_progression_entries=0),
    ):
        with pytest.raises(SonareError):
            StreamAnalyzer(**bad)

    # A well-formed set of keywords still constructs.
    StreamAnalyzer(sample_rate=22050).close()


def test_stream_analyzer_bounds_unread_frames_and_reports_drops() -> None:
    from libsonare import StreamAnalyzer

    config = dict(
        sample_rate=8000,
        n_fft=32,
        hop_length=32,
        n_mels=8,
        max_pending_frames=3,
        max_progression_entries=3,
    )
    with StreamAnalyzer(**config) as analyzer:
        analyzer.process([0.0] * (32 * 64))
        stats = analyzer.stats()
        assert stats.pending_frames == 3
        assert stats.dropped_output_frames > 0
        assert stats.pending_frames + stats.dropped_output_frames == stats.total_frames
        assert stats.dropped_chord_progression_entries == 0
        assert stats.dropped_bar_progression_entries == 0


def test_streaming_mastering_chain_processes_mono_block() -> None:
    """StreamingMasteringChain processes a 512-sample mono block in place."""
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain(
        {"eq": {"tilt": {"tiltDb": 1.0}}}, sample_rate=44100, max_block_size=512, num_channels=1
    )
    block = [0.1] * 512
    out = chain.process_mono(block)
    assert len(out) == len(block)
    assert any(abs(out[i] - block[i]) > 1e-6 for i in range(len(out)))
    chain.reset()


def test_streaming_mastering_chain_flushes_delayed_output() -> None:
    """flush_mono drains latency once and then reports completion."""
    from libsonare import StreamingMasteringChain

    with StreamingMasteringChain(
        {"maximizer": {"truePeakLimiter": {"enabled": 1}}},
        sample_rate=48000,
        max_block_size=64,
        num_channels=1,
    ) as chain:
        assert chain.latency_samples > 0
        chain.process_mono([0.5] + [0.0] * 63)
        tail: list[float] = []
        while True:
            block = chain.flush_mono()
            if block.size == 0:
                break
            tail.extend(block)
        assert len(tail) >= chain.latency_samples
        assert chain.flush_mono().size == 0


@pytest.mark.parametrize("num_channels", [1, 2])
def test_streaming_mastering_chain_refuses_to_flush_an_unbounded_tail(num_channels: int) -> None:
    """Tape saturation with a bias rings forever, so no finite flush drains it."""
    from libsonare import ErrorCode, StreamingMasteringChain

    entry = "flush_mono" if num_channels == 1 else "flush_stereo"
    text = (
        rf"^\[6\] StreamingMasteringChain\.{entry}: "
        r"cannot automatically flush a chain with an unbounded tail$"
    )
    with StreamingMasteringChain(
        {"saturation": {"tape": {"enabled": 1, "bias": 0.1}}},
        sample_rate=48000,
        max_block_size=64,
        num_channels=num_channels,
    ) as chain:
        block = [0.5] * 64
        if num_channels == 1:
            chain.process_mono(block)
        else:
            chain.process_stereo(block, block)
        with raises_coded(ErrorCode.NOT_SUPPORTED, match=text):
            chain.flush_mono() if num_channels == 1 else chain.flush_stereo()


@pytest.mark.parametrize(
    ("band", "current"),
    [
        ({"type": "Peak", "enabled": True}, "peak"),
        ({"type": "AllPass", "enabled": True}, "allPass"),
        ({"type": "peak", "lookaheadMs": 1.0, "enabled": True}, "detectorDelayMs"),
    ],
)
def test_streaming_equalizer_refuses_earlier_band_spellings(band: dict, current: str) -> None:
    """Band types are lowerCamel and ``lookaheadMs`` is gone; the refusal names what is accepted."""
    from libsonare import ErrorCode, StreamingEqualizer

    with (
        StreamingEqualizer(sample_rate=44100, max_block_size=256) as eq,
        raises_coded(int(ErrorCode.INVALID_PARAMETER), match=current),
    ):
        eq.set_band(0, band)


def test_streaming_equalizer_takes_a_band_mapping_and_refuses_json_text() -> None:
    """A band is an ``EqBandInput`` mapping; JSON text is not a second input form."""
    from libsonare import StreamingEqualizer

    with (
        StreamingEqualizer(sample_rate=44100, max_block_size=256) as eq,
        pytest.raises(TypeError, match="json.loads"),
    ):
        eq.set_band(0, '{"type":"peak","enabled":true}')  # type: ignore[arg-type]


def test_streaming_mastering_chain_preserves_ndarray_input() -> None:
    """process_* must not overwrite a float32 ndarray input (non-destructive)."""
    import numpy as np

    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain(
        {"eq": {"tilt": {"tiltDb": 3.0}}}, sample_rate=44100, max_block_size=256, num_channels=1
    )
    block = np.full(256, 0.25, dtype=np.float32)
    original = block.copy()
    out = chain.process_mono(block)
    # The dry input is untouched; the returned wet block differs from it.
    assert np.array_equal(block, original)
    assert any(abs(out[i] - float(original[i])) > 1e-6 for i in range(len(out)))
    chain.close()


def test_streaming_equalizer_preserves_ndarray_input() -> None:
    """StreamingEqualizer.process_mono must not overwrite a float32 ndarray input."""
    import numpy as np

    from libsonare import StreamingEqualizer

    with StreamingEqualizer(sample_rate=44100, max_block_size=256) as eq:
        eq.set_band(
            0,
            {"type": "peak", "frequencyHz": 1000.0, "gainDb": 6.0, "q": 1.0, "enabled": True},
        )
        block = np.full(256, 0.25, dtype=np.float32)
        original = block.copy()
        eq.process_mono(block)
        assert np.array_equal(block, original)


def test_streaming_mastering_chain_reports_stage_names() -> None:
    """stage_names() exposes the stages the constructor realized."""
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain(
        {"eq": {"tilt": {"tiltDb": 1.0}}}, sample_rate=44100, max_block_size=512, num_channels=1
    )
    names = chain.stage_names()
    assert isinstance(names, list)
    assert all(isinstance(name, str) for name in names)
    assert any("eq.tilt" in name for name in names)
    chain.close()


def test_streaming_equalizer_processes_blocks_and_exposes_spectrum() -> None:
    """StreamingEqualizer exposes the native SonareEq handle to Python."""
    from libsonare import StreamingEqualizer

    sr = 48000
    block = [0.2 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(512)]

    with StreamingEqualizer(sample_rate=sr, max_block_size=512) as eq:
        eq.set_gain_scale(0.5)
        eq.set_output_gain_db(3.0)
        eq.set_output_pan(0.0)
        eq.set_band(
            0,
            {
                "type": "peak",
                "frequencyHz": 1000.0,
                "gainDb": 6.0,
                "q": 1.0,
                "enabled": True,
            },
        )
        out = eq.process_mono(block)
        assert len(out) == len(block)
        assert max(abs(sample) for sample in out) > max(abs(sample) for sample in block)

        snapshot = eq.spectrum()
        assert snapshot.seq == 1
        assert len(snapshot.pre_left) == 256
        assert len(snapshot.post_left) == 256
        assert 2.5 < snapshot.band_gain_db[0] < 3.5

        eq.set_phase_mode("linear")
        assert eq.latency_samples > 0


def test_eq_spectrum_band_lists_are_sized_by_the_ctypes_mirror() -> None:
    """The published lengths, the mirror and the constants are one chain."""
    from libsonare import StreamingEqualizer
    from libsonare._ffi_types_mastering_project import (
        SONARE_EQ_MAX_BANDS,
        SONARE_EQ_SPECTRUM_PROFILE_BANDS,
        SonareEqSnapshot,
    )

    declared = dict(SonareEqSnapshot._fields_)
    assert declared["band_gain_db"]._length_ == SONARE_EQ_MAX_BANDS
    assert declared["profile_db"]._length_ == SONARE_EQ_SPECTRUM_PROFILE_BANDS

    with StreamingEqualizer(sample_rate=48000, max_block_size=256) as eq:
        eq.process_mono([0.0] * 256)
        snapshot = eq.spectrum()

    assert len(snapshot.band_gain_db) == declared["band_gain_db"]._length_
    assert len(snapshot.profile_db) == declared["profile_db"]._length_


def test_eq_spectrum_follows_a_resized_ctypes_mirror(monkeypatch) -> None:
    """A grown C array reaches the caller instead of being cut to the old length.

    The literals this used to carry equalled the constants, so the duplication
    was invisible while the sizes agreed. Resizing the mirror is the only way to
    assert the derivation rather than today's numbers.
    """
    import ctypes
    from types import SimpleNamespace

    from libsonare import StreamingEqualizer, _mastering_streaming
    from libsonare._ffi_types_mastering_project import SonareEqSnapshot

    declared = dict(SonareEqSnapshot._fields_)
    grown_bands = declared["band_gain_db"]._length_ + 2
    grown_profile = declared["profile_db"]._length_ + 3
    grown_fields = []
    for name, kind in SonareEqSnapshot._fields_:
        if name == "band_gain_db":
            kind = ctypes.c_float * grown_bands
        elif name == "profile_db":
            kind = ctypes.c_float * grown_profile
        grown_fields.append((name, kind))

    grown_snapshot = type("_GrownEqSnapshot", (ctypes.Structure,), {"_fields_": grown_fields})
    monkeypatch.setattr(_mastering_streaming, "SonareEqSnapshot", grown_snapshot)

    with StreamingEqualizer(sample_rate=48000, max_block_size=256) as eq:
        # The native call cannot fill a layout it was not compiled against, so
        # it stands in as a success that writes nothing: the lengths under test
        # come from the mirror, not from the payload. Restored before the
        # context manager exits, which needs the real destroy.
        real_lib = eq._lib
        eq._lib = SimpleNamespace(sonare_eq_spectrum=lambda handle, ref: 0)
        try:
            snapshot = eq.spectrum()
        finally:
            eq._lib = real_lib

    assert len(snapshot.band_gain_db) == grown_bands
    assert len(snapshot.profile_db) == grown_profile


def test_streaming_equalizer_match_configures_bands() -> None:
    """StreamingEqualizer.match forwards to the live EQ match C API."""
    from libsonare import StreamingEqualizer

    sr = 48000
    source = [0.08 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(1024)]
    reference = [0.35 * math.sin(2 * math.pi * 1000 * i / sr) for i in range(1024)]

    with StreamingEqualizer(sample_rate=sr, max_block_size=len(source)) as eq:
        eq.match(source, reference, max_bands=4)
        out = eq.process_mono(source)
        assert len(out) == len(source)
        assert any(gain > 0.5 for gain in eq.spectrum().band_gain_db)


def test_streaming_equalizer_magnitude_response_follows_a_peak_band() -> None:
    """magnitude_response() reads a +6 dB peak at its centre and flat away from it."""
    from libsonare import StreamingEqualizer

    with StreamingEqualizer(sample_rate=48000, max_block_size=256) as eq:
        eq.set_band(
            0,
            {"type": "peak", "frequencyHz": 1000.0, "gainDb": 6.0, "q": 1.0, "enabled": True},
        )
        centre, low, high = eq.magnitude_response([1000.0, 40.0, 16000.0])
        assert 5.5 < centre < 6.5
        assert abs(low) < 0.5
        assert abs(high) < 0.5


def test_streaming_equalizer_magnitude_response_is_flat_without_bands() -> None:
    """With nothing enabled every placement reads 0 dB at every frequency."""
    from libsonare import StreamingEqualizer

    frequencies = [20.0, 100.0, 1000.0, 10000.0]
    with StreamingEqualizer(sample_rate=48000, max_block_size=256) as eq:
        for placement in ("stereo", "left", "right", "mid", "side"):
            response = eq.magnitude_response(frequencies, placement=placement)
            assert len(response) == len(frequencies)
            assert all(abs(value) < 1e-3 for value in response)


def test_streaming_equalizer_magnitude_response_respects_band_placement() -> None:
    """A band placed on one side is absent from the other side's curve."""
    from libsonare import StreamingEqualizer

    with StreamingEqualizer(sample_rate=48000, max_block_size=256) as eq:
        eq.set_band(
            0,
            {
                "type": "peak",
                "frequencyHz": 1000.0,
                "gainDb": 6.0,
                "q": 1.0,
                "enabled": True,
                "placement": "left",
            },
        )
        (left,) = eq.magnitude_response([1000.0], placement="left")
        (right,) = eq.magnitude_response([1000.0], placement="right")
        assert 5.5 < left < 6.5
        assert abs(right) < 0.5


def test_streaming_equalizer_magnitude_response_flat_for_all_pass_band() -> None:
    """An allPass band rotates phase only, so it moves nothing on the magnitude curve."""
    from libsonare import StreamingEqualizer

    with StreamingEqualizer(sample_rate=48000, max_block_size=256) as eq:
        eq.set_band(0, {"type": "allPass", "frequencyHz": 1000.0, "q": 1.0, "enabled": True})
        response = eq.magnitude_response([200.0, 1000.0, 8000.0])
        assert all(abs(value) < 0.1 for value in response)


def test_streaming_equalizer_magnitude_response_rejects_unknown_placement() -> None:
    """An unknown placement is rejected by the binding, not forwarded as an ordinal."""
    from libsonare import SonareValueError, StreamingEqualizer

    with (
        StreamingEqualizer(sample_rate=48000, max_block_size=256) as eq,
        pytest.raises(SonareValueError),
    ):
        eq.magnitude_response([1000.0], placement="rear")


def test_streaming_mastering_chain_rejects_denoise_with_the_quantile_estimator() -> None:
    """The whole-signal estimator is NotSupported by name; the stream's default prepares."""
    from libsonare import SonareError, SonareValueError, StreamingMasteringChain

    with pytest.raises(
        SonareError,
        match=r"^\[6\] StreamingMasteringChain: config\.repair\.denoise\.noiseEstimator "
        r"quantile is not "
        r"supported for a streaming chain: ",
    ) as refused:
        StreamingMasteringChain(
            {"repair": {"denoise": {"enabled": 1, "noiseEstimator": "quantile"}}},
            sample_rate=44100,
            num_channels=1,
        )
    assert refused.value.code_name == "NotSupported"
    assert not isinstance(refused.value, SonareValueError)
    StreamingMasteringChain(
        {"repair": {"denoise": {"enabled": 1}}}, sample_rate=44100, num_channels=1
    ).close()


@pytest.mark.parametrize("stage", ["declick", "declip", "decrackle", "dehum", "dereverb"])
def test_streaming_mastering_chain_refuses_a_whole_signal_repair_stage(stage: str) -> None:
    """A stage that needs the whole signal is NotSupported, not an argument error."""
    from libsonare import SonareError, SonareValueError, StreamingMasteringChain

    with pytest.raises(
        SonareError,
        match=rf"^\[6\] StreamingMasteringChain: config\.repair\.{stage} is not supported for a "
        r"streaming chain: it needs the whole signal$",
    ) as refused:
        StreamingMasteringChain(
            {"repair": {stage: {"enabled": True}}}, sample_rate=44100, num_channels=1
        )
    assert refused.value.code_name == "NotSupported"
    assert not isinstance(refused.value, SonareValueError)


@pytest.mark.parametrize("value", [{}, "x", None, []])
def test_master_audio_refuses_any_value_under_an_undeclared_key(value: object) -> None:
    from libsonare import ErrorCode, master_audio

    with raises_coded(
        ErrorCode.INVALID_PARAMETER,
        match=re.escape(chain_unknown_key_text("unknown key overrides.eq.tilt.undeclared")) + "$",
    ):
        master_audio(
            np.zeros(4800, dtype=np.float32),
            48000,
            overrides={"eq": {"tilt": {"undeclared": value}}},
        )


def test_master_audio_accepts_annotation_keys() -> None:
    from libsonare import master_audio

    samples = np.zeros(4800, dtype=np.float32)
    annotated = master_audio(
        samples, 48000, overrides={"x-note": [], "eq": {"tilt": {"$comment": {}}}}
    )
    assert np.array_equal(annotated.samples, master_audio(samples, 48000, overrides={}).samples)


def test_streaming_set_parameter_refuses_an_undeclared_parameter() -> None:
    from libsonare import ErrorCode, StreamingMasteringChain

    with (
        StreamingMasteringChain(
            {"eq": {"tilt": {"enabled": True}}}, sample_rate=48000, num_channels=1
        ) as chain,
        raises_coded(
            ErrorCode.INVALID_PARAMETER,
            match=re.escape(chain_unknown_key_text("unknown key eq.tilt.undeclared")) + "$",
        ),
    ):
        chain.set_parameter("eq.tilt.undeclared", 2.0)


def test_streaming_mastering_chain_refuses_a_renamed_key_as_any_other_unknown_key() -> None:
    from libsonare import SonareError, StreamingMasteringChain

    with pytest.raises(
        SonareError,
        match=re.escape(chain_unknown_key_text("unknown key config.dynamics.deesser.bandpassQ"))
        + "$",
    ):
        StreamingMasteringChain(
            {"dynamics": {"deesser": {"enabled": True, "bandpassQ": 2.0}}},
            sample_rate=48000,
            num_channels=1,
        )


def test_streaming_mastering_chain_accepts_loudness_with_static_gain() -> None:
    """A loudness-enabled config builds with no static gain, which applies none, or with one."""
    from libsonare import StreamingMasteringChain

    with StreamingMasteringChain(
        {"loudness": {"targetLufs": -14.0}}, sample_rate=44100, num_channels=1
    ) as no_gain:
        assert all(math.isfinite(x) for x in no_gain.process_mono([0.1] * 128))

    chain = StreamingMasteringChain(
        {"loudness": {"targetLufs": -14.0}},
        sample_rate=44100,
        max_block_size=512,
        loudness_static_gain_db=3.0,
        loudness_static_gain_peak_dbtp=-1.0,
        num_channels=1,
    )
    out = chain.process_mono([0.1] * 512)
    assert len(out) == 512
    assert all(math.isfinite(x) for x in out)
    chain.reset()


def test_streaming_mastering_chain_reports_no_substitution_on_a_clean_stream() -> None:
    """The cumulative substitution count is reachable and stays zero for finite input.

    A substituting stage leaves the output finite, in range and free of any
    error while carrying samples unrelated to the input, so zero is what says
    the blocks were computed from what was supplied.
    """
    from libsonare import StreamingMasteringChain

    with StreamingMasteringChain(
        {"maximizer": {"truePeakLimiter": {"enabled": 1}}},
        sample_rate=44100,
        max_block_size=256,
        num_channels=1,
    ) as chain:
        assert chain.non_finite_substitution_count == 0
        for _ in range(4):
            out = chain.process_mono(
                [0.4 * math.sin(2 * math.pi * 220 * i / 44100) for i in range(256)]
            )
            assert all(math.isfinite(x) for x in out)
        count = chain.non_finite_substitution_count
        assert isinstance(count, int)
        assert count == 0


def test_streaming_mastering_chain_discard_count_rises_after_a_stage_overflow() -> None:
    """A stage's own recursive state overflowing moves the discard count.

    Non-finite input is rejected before any stage runs, so the poison here is
    a finite sample (3e38, under FLT_MAX) large enough that a +24 dB tilt
    shelf's own coefficients overflow it to infinity internally -- the same
    failure mode the strip/bus/EQ discard counters exist to catch, not a
    substitution (nothing here replaces a sample the caller supplied).
    """
    from libsonare import StreamingMasteringChain

    with StreamingMasteringChain(
        {"eq": {"tilt": {"tiltDb": 24.0}}}, sample_rate=48000, num_channels=1
    ) as chain:
        # Without an enabled recursive stage the chain holds no state to lose,
        # so its count would stay at zero for a reason unrelated to the entry.
        assert chain.non_finite_discard_count == 0

        # Control: an ordinary block counts nothing, so the rise below is
        # attributable to the poison rather than to processing at all.
        chain.process_mono([0.25] * 128)
        assert chain.non_finite_discard_count == 0

        chain.process_mono([3.0e38] * 128)
        assert chain.non_finite_discard_count == 1


def test_stft_result_types() -> None:
    """StftResult fields have correct types and shapes."""
    from libsonare import stft

    tone = _generate_sine(440, 22050, 1.0)
    result = stft(tone, sr=22050)
    assert isinstance(result.n_fft, int)
    assert isinstance(result.hop_length, int)
    assert isinstance(result.sample_rate, int)
    assert isinstance(result.magnitude, np.ndarray)
    assert isinstance(result.power, np.ndarray)
    assert result.n_fft == 2048
    assert result.hop_length == 512
    assert result.magnitude.shape[0] == 2048 // 2 + 1
    assert result.magnitude.shape[1] > 0
    assert result.power.shape == result.magnitude.shape


def test_master_audio_invokes_progress_callback() -> None:
    """master_audio reports each stage to a progress callback."""
    from libsonare import master_audio

    calls: list[tuple[float, str]] = []

    def on_progress(progress: float, stage: str) -> None:
        calls.append((progress, stage))

    result = master_audio(
        samples=[0.1] * 22050,
        sample_rate=22050,
        overrides={
            "eq": {"tilt": {"tiltDb": 1.0}},
            "dynamics": {"compressor": {"thresholdDb": -24.0}},
        },
        on_progress=on_progress,
    )
    assert len(result.samples) == 22050
    assert len(calls) >= 2
    stages = [s for _, s in calls]
    assert "eq.tilt" in stages
    assert "dynamics.compressor" in stages
    # Final progress reaches 1.0.
    assert calls[-1][0] == pytest.approx(1.0, abs=1e-5)


def test_master_audio_false_progress_result_does_not_cancel() -> None:
    """A progress callback's boolean return value is ignored."""
    from libsonare import master_audio

    calls: list[float] = []

    def on_progress(progress: float, _stage: str) -> object:
        calls.append(progress)
        return False

    result = master_audio(
        samples=[0.1] * 22050,
        sample_rate=22050,
        overrides={
            "eq": {"tilt": {"tiltDb": 1.0}},
            "dynamics": {"compressor": {"thresholdDb": -24.0}},
        },
        on_progress=on_progress,
    )

    assert any(progress > 0.5 for progress in calls)
    assert result.samples.size > 0


def test_master_audio_cancel_callable_cancels_without_a_result() -> None:
    """The keyword-only cancellation callback is forwarded to the C ABI."""
    from libsonare import SonareError, master_audio

    result = None
    with pytest.raises(SonareError) as exc:
        result = master_audio(
            samples=[0.1] * 22050,
            sample_rate=22050,
            overrides={"eq": {"tilt": {"tiltDb": 1.0}}},
            cancel=lambda: True,
        )

    assert exc.value.code == 8
    assert result is None


@pytest.mark.parametrize(
    "operation",
    ["chain-stereo", "preset-mono", "preset-stereo"],
)
def test_remaining_mastering_progress_variants_forward_cancellation(operation: str) -> None:
    """Every chain/preset mono/stereo progress variant uses its matching _ex ABI."""
    from libsonare import (
        SonareError,
        master_audio,
        master_audio_stereo,
    )

    samples = [0.1] * 22050
    result = None
    with pytest.raises(SonareError) as exc:
        if operation == "chain-stereo":
            result = master_audio_stereo(
                samples,
                samples,
                sample_rate=22050,
                overrides={"eq": {"tilt": {"tiltDb": 1.0}}},
                cancel=lambda: True,
            )
        elif operation == "preset-mono":
            result = master_audio(samples, sample_rate=22050, preset="pop", cancel=lambda: True)
        else:
            result = master_audio_stereo(
                samples, samples, sample_rate=22050, preset="pop", cancel=lambda: True
            )

    assert exc.value.code == 8
    assert result is None


def test_master_audio_reads_the_nested_config() -> None:
    """The nested config selects the stages it names."""
    from libsonare import master_audio

    result = master_audio(
        samples=[0.1] * 22050,
        sample_rate=22050,
        overrides={
            "eq": {"tilt": {"tiltDb": 1.0}},
            "dynamics": {"compressor": {"thresholdDb": -24.0}},
        },
    )
    assert "eq.tilt" in result.stages
    assert "dynamics.compressor" in result.stages


_CHAIN_CONFIG_CALLS = {
    "master_audio_overrides_only": lambda c: libsonare.master_audio(
        [0.1] * 2205, 22050, overrides=c
    ),
    "master_audio": lambda c: libsonare.master_audio(
        [0.1] * 2205, 22050, preset="pop", overrides=c
    ),
    "streaming_loudness_gain": lambda c: libsonare.streaming_loudness_gain([0.1] * 2205, 22050, c),
    "StreamingMasteringChain": lambda c: libsonare.StreamingMasteringChain(
        c, sample_rate=22050, num_channels=1
    ),
    "master_audio_stereo_overrides_only": lambda c: libsonare.master_audio_stereo(
        [0.1] * 2205, [0.1] * 2205, 22050, overrides=c
    ),
    "master_audio_stereo": lambda c: libsonare.master_audio_stereo(
        [0.1] * 2205, [0.1] * 2205, 22050, preset="pop", overrides=c
    ),
    "streaming_loudness_gain_stereo": lambda c: libsonare.streaming_loudness_gain_stereo(
        [0.1] * 2205, [0.1] * 2205, 22050, c
    ),
}


# The accepted-key list a chain reader appends to its unknown-key refusal, as a pattern.
_CHAIN_KEYS_TAIL = re.escape(chain_unknown_key_text("")) + "$"

_CHAIN_CONTENT_REFUSALS = {
    "duplicate_path": (
        {"eq": {"tilt": {"tiltDb": 1.0}}, "eq.tilt.tiltDb": 1.0},
        r"{root}\.eq\.tilt\.tiltDb must not be given twice$",
    ),
    "null_leaf": (
        {"loudness": {"targetLufs": None}},
        r"{root}\.loudness\.targetLufs must be a number, a boolean or a token",
    ),
    "number_for_a_token": (
        {"dynamics": {"compressor": {"detector": 1}}},
        r"{root}\.dynamics\.compressor\.detector must be one of peak, rms, logRms$",
    ),
    "clip_threshold_above_full_scale": (
        {"repair": {"declip": {"clipThresholdDb": 1.0}}},
        r"{root}\.repair\.declip\.clipThresholdDb must be finite and in \[-890, 0\]$",
    ),
    "earlier_true_peak_ceiling_spelling": (
        {"loudness": {"ceilingDb": -1.0}},
        r"unknown key {root}\.loudness\.ceilingDb" + _CHAIN_KEYS_TAIL,
    ),
}


@pytest.mark.parametrize("name", sorted(_CHAIN_CONFIG_CALLS))
def test_a_dotted_chain_key_names_the_path_its_nesting_does(name: str) -> None:
    """A dotted key is the path a nested object spells; the core reads both."""
    call = _CHAIN_CONFIG_CALLS[name]
    call({"eq.tilt.tiltDb": 1.0})
    call({"eq": {"tilt": {"tiltDb": 1.0}}})


@pytest.mark.parametrize("refusal", sorted(_CHAIN_CONTENT_REFUSALS))
@pytest.mark.parametrize("name", sorted(_CHAIN_CONFIG_CALLS))
def test_chain_config_content_is_refused_by_the_core_naming_the_path(
    name: str, refusal: str
) -> None:
    config, message = _CHAIN_CONTENT_REFUSALS[refusal]
    # A per-call bag is an argument, so it is refused under the caller's own name.
    root = "config" if name.lower().startswith("streaming") else "overrides"
    with raises_coded(libsonare.ErrorCode.INVALID_PARAMETER, match=message.replace("{root}", root)):
        _CHAIN_CONFIG_CALLS[name](config)


def test_color_saturation_stages_engage_only_when_meaningful() -> None:
    """Tape/exciter must not auto-engage on a zero-valued config (no coloration)."""
    from libsonare import master_audio

    def stages_for(config: dict) -> list[str]:
        return master_audio(samples=[0.1] * 22050, sample_rate=22050, overrides=config).stages

    # A zero-amount exciter / zero-drive tape is a no-op and stays bypassed.
    assert "saturation.exciter" not in stages_for({"saturation": {"exciter": {"amount": 0.0}}})
    assert "saturation.tape" not in stages_for(
        {"saturation": {"tape": {"driveDb": 0.0, "saturation": 0.0}}}
    )
    # Meaningful params engage the stage.
    assert "saturation.exciter" in stages_for({"saturation": {"exciter": {"amount": 0.2}}})
    # An explicit enabled flag wins either way.
    assert "saturation.exciter" in stages_for(
        {"saturation": {"exciter": {"amount": 0.0, "enabled": True}}}
    )
    assert "saturation.tape" not in stages_for(
        {"saturation": {"tape": {"driveDb": 3.0, "enabled": False}}}
    )


# ============================================================================
# Newly exposed analysis functions: onset envelope, Fourier tempogram,
# tempogram ratio, NNLS chroma, and LUFS loudness metering.
# ============================================================================


def test_stream_analyzer_counts_a_call_whose_input_it_scrubbed() -> None:
    """One process() call adds one, however many samples in it were bad.

    The analyzer replaces a non-finite input sample before it reaches the STFT
    and the onset/chroma histories, so nothing is missing from the output and
    every estimate is produced as usual -- this count is the only report that
    they stopped describing the input.
    """
    from libsonare import StreamAnalyzer

    config = dict(sample_rate=8000, n_fft=32, hop_length=32, n_mels=8)
    with StreamAnalyzer(**config) as analyzer:
        clean = [0.5 * math.sin(2 * math.pi * 220 * i / 8000) for i in range(256)]
        analyzer.process(clean)
        # The stream really was analyzed, so the zero below is a measurement
        # rather than the absence of one.
        assert analyzer.stats().total_frames > 0
        assert analyzer.stats().non_finite_discard_blocks == 0

        # Half the block is non-finite, with both infinities and NaN present. A
        # count that followed the samples rather than the call would land far
        # from one; that is what this asserts.
        poisoned = list(clean)
        for i in range(0, len(poisoned), 2):
            poisoned[i] = float("nan") if i % 4 == 0 else float("inf")
        poisoned[1] = float("-inf")
        analyzer.process(poisoned)
        assert analyzer.stats().non_finite_discard_blocks == 1

        # A clean call adds nothing; a second bad one does, so it is not stuck.
        analyzer.process(clean)
        assert analyzer.stats().non_finite_discard_blocks == 1
        analyzer.process(poisoned)
        assert analyzer.stats().non_finite_discard_blocks == 2

        analyzer.reset()
        assert analyzer.stats().non_finite_discard_blocks == 0


def _chord_progression_samples(sample_rate: int) -> list[float]:
    chords = [
        [261.63, 329.63, 392.0],
        [392.0, 493.88, 587.33],
        [220.0, 261.63, 329.63],
        [349.23, 440.0, 523.25],
    ]
    out: list[float] = []
    for i in range(sample_rate * 8):
        t = i / sample_rate
        freqs = chords[int(t // 2)]
        out.append(sum(math.sin(2 * math.pi * f * t) / len(freqs) for f in freqs) * 0.3)
    return out


def test_stream_analyzer_names_agree_with_numeric_fields() -> None:
    from libsonare import StreamAnalyzer

    pitches = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
    sr = 22050
    samples = _chord_progression_samples(sr)
    with StreamAnalyzer(sample_rate=sr) as analyzer:
        for i in range(0, len(samples), 4096):
            analyzer.process(samples[i : i + 4096])
        stats = analyzer.stats()

    estimate = stats.estimate
    assert estimate.chord_progression
    chords = [*estimate.chord_progression, *estimate.bar_chord_progression]
    chords += [chord for chord in estimate.voted_pattern if chord is not None]
    if estimate.chord is not None:
        chords.append(estimate.chord)
    for chord in chords:
        assert chord.name.startswith(pitches[chord.root])
        if chord.quality == "major":
            assert chord.name == pitches[chord.root]
        if chord.quality == "minor":
            assert chord.name == pitches[chord.root] + "m"

    key = estimate.key
    assert key is not None
    assert key.name == f"{pitches[key.root]} {key.mode}"
    assert key.short_name == pitches[key.root] + ("m" if key.mode == "minor" else "")


def test_stream_analyzer_key_name_matches_batch_spelling() -> None:
    from libsonare import StreamAnalyzer, detect_key

    sr = 22050
    notes = [261.63, 293.66, 329.63, 349.23, 392.0, 440.0, 493.88]
    samples = [math.sin(2 * math.pi * note * i / sr) * 0.5 for note in notes for i in range(sr)]
    with StreamAnalyzer(sample_rate=sr) as analyzer:
        for i in range(0, len(samples), 4096):
            analyzer.process(samples[i : i + 4096])
        stats = analyzer.stats()

    batch = detect_key(samples, sr)
    key = stats.estimate.key
    assert key is not None
    assert key.name == batch.name == "C major"
    assert key.short_name == batch.short_name == "C"


def test_stream_estimates_are_absent_before_any_audio() -> None:
    from libsonare import StreamAnalyzer

    with StreamAnalyzer(sample_rate=22050) as analyzer:
        estimate = analyzer.stats().estimate
    assert estimate.key is None
    assert estimate.chord is None
    assert estimate.bpm is None and estimate.bpm_confidence is None
    assert estimate.current_bar is None and estimate.bar_duration_sec is None
    assert estimate.detected_pattern_name is None and estimate.detected_pattern_score is None
