"""Streaming mastering and advanced analyzer API tests."""

from __future__ import annotations

import math

# ruff: noqa: F403,F405
from ._analyzer_helpers import *


def test_stream_analyzer_rejects_malformed_config_geometry() -> None:
    """StreamAnalyzer rejects malformed config geometry, matching every surface.

    The shared C++ constructor and the flat C ABI enforce the same relationship
    and positive-value contract, so Python construction raises instead of
    silently producing garbage spectra.
    """
    from libsonare import SonareError, StreamAnalyzer, StreamConfig

    for bad in (
        StreamConfig(sample_rate=0),
        StreamConfig(n_fft=0),
        StreamConfig(n_mels=0),
        StreamConfig(n_fft=1024, hop_length=2048),
        StreamConfig(fmin=8000.0, fmax=4000.0),
        StreamConfig(max_progression_entries=0),
    ):
        with pytest.raises(SonareError):
            StreamAnalyzer(bad)

    # A well-formed config still constructs.
    StreamAnalyzer(StreamConfig(sample_rate=22050)).close()


def test_stream_analyzer_bounds_unread_frames_and_reports_drops() -> None:
    from libsonare import StreamAnalyzer, StreamConfig

    config = StreamConfig(
        sample_rate=8000,
        n_fft=32,
        hop_length=32,
        n_mels=8,
        max_pending_frames=3,
        max_progression_entries=3,
    )
    with StreamAnalyzer(config) as analyzer:
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

    chain = StreamingMasteringChain({"eq.tilt.tiltDb": 1.0})
    chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
    block = [0.1] * 512
    out = chain.process_mono(block)
    assert len(out) == len(block)
    assert any(abs(out[i] - block[i]) > 1e-6 for i in range(len(out)))
    chain.reset()


def test_streaming_mastering_chain_flushes_delayed_output() -> None:
    """flush_mono drains latency once and then reports completion."""
    from libsonare import StreamingMasteringChain

    with StreamingMasteringChain({"maximizer.truePeakLimiter.enabled": 1}) as chain:
        chain.prepare(sample_rate=48000, max_block_size=64, num_channels=1)
        assert chain.latency_samples > 0
        chain.process_mono([0.5] + [0.0] * 63)
        tail: list[float] = []
        while True:
            block = chain.flush_mono()
            if not block:
                break
            tail.extend(block)
        assert len(tail) >= chain.latency_samples
        assert chain.flush_mono() == []


def test_streaming_mastering_chain_preserves_ndarray_input() -> None:
    """process_* must not overwrite a float32 ndarray input (non-destructive)."""
    import numpy as np

    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain({"eq.tilt.tiltDb": 3.0})
    chain.prepare(sample_rate=44100, max_block_size=256, num_channels=1)
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
            {"type": "Peak", "frequencyHz": 1000.0, "gainDb": 6.0, "q": 1.0, "enabled": True},
        )
        block = np.full(256, 0.25, dtype=np.float32)
        original = block.copy()
        eq.process_mono(block)
        assert np.array_equal(block, original)


def test_streaming_mastering_chain_reports_stage_names() -> None:
    """stage_names() exposes the realized stages after prepare()."""
    from libsonare import StreamingMasteringChain

    chain = StreamingMasteringChain({"eq.tilt.tiltDb": 1.0})
    # No stages are realized until prepare() runs.
    assert chain.stage_names() == []
    chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
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
                "type": "Peak",
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
            {"type": "Peak", "frequencyHz": 1000.0, "gainDb": 6.0, "q": 1.0, "enabled": True},
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
                "type": "Peak",
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
    """An AllPass band rotates phase only, so it moves nothing on the magnitude curve."""
    from libsonare import StreamingEqualizer

    with StreamingEqualizer(sample_rate=48000, max_block_size=256) as eq:
        eq.set_band(0, {"type": "AllPass", "frequencyHz": 1000.0, "q": 1.0, "enabled": True})
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


def test_streaming_mastering_chain_rejects_denoise() -> None:
    """StreamingMasteringChain refuses configurations enabling repair.denoise."""
    from libsonare import StreamingMasteringChain

    with pytest.raises(RuntimeError):
        StreamingMasteringChain({"repair.denoise.enabled": 1})


def test_streaming_mastering_chain_accepts_loudness_with_static_gain() -> None:
    """A loudness-enabled config no longer raises when a static gain is supplied."""
    from libsonare import StreamingMasteringChain

    # Without a static gain a loudness target raises (cannot measure online)...
    with pytest.raises(RuntimeError):
        StreamingMasteringChain({"loudness.targetLufs": -14.0})

    # ...but supplying a precomputed static gain makes it processable.
    chain = StreamingMasteringChain(
        {"loudness.targetLufs": -14.0},
        loudness_static_gain_db=3.0,
        loudness_static_gain_peak_db=-1.0,
    )
    chain.prepare(sample_rate=44100, max_block_size=512, num_channels=1)
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

    with StreamingMasteringChain({"maximizer.truePeakLimiter.enabled": 1}) as chain:
        chain.prepare(sample_rate=44100, max_block_size=256, num_channels=1)
        assert chain.non_finite_substitution_count() == 0
        for _ in range(4):
            out = chain.process_mono(
                [0.4 * math.sin(2 * math.pi * 220 * i / 44100) for i in range(256)]
            )
            assert all(math.isfinite(x) for x in out)
        count = chain.non_finite_substitution_count()
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

    with StreamingMasteringChain({"eq.tilt.tiltDb": 24.0}) as chain:
        chain.prepare(48000, 128, 1)
        # Without an enabled recursive stage the chain holds no state to lose,
        # so its count would stay at zero for a reason unrelated to the entry.
        assert chain.non_finite_discard_count() == 0

        # Control: an ordinary block counts nothing, so the rise below is
        # attributable to the poison rather than to processing at all.
        chain.process_mono([0.25] * 128)
        assert chain.non_finite_discard_count() == 0

        chain.process_mono([3.0e38] * 128)
        assert chain.non_finite_discard_count() == 1


def test_stft_result_types() -> None:
    """StftResult fields have correct types and shapes."""
    from libsonare import stft

    tone = _generate_sine(440, 22050, 1.0)
    result = stft(tone, sample_rate=22050)
    assert isinstance(result.n_bins, int)
    assert isinstance(result.n_frames, int)
    assert isinstance(result.n_fft, int)
    assert isinstance(result.hop_length, int)
    assert isinstance(result.sample_rate, int)
    assert isinstance(result.magnitude, list)
    assert isinstance(result.power, list)
    assert result.n_fft == 2048
    assert result.hop_length == 512
    assert len(result.magnitude) == result.n_bins * result.n_frames
    assert len(result.power) == result.n_bins * result.n_frames


def test_mastering_chain_invokes_progress_callback() -> None:
    """mastering_chain preserves void progress callback compatibility."""
    from libsonare import mastering_chain

    calls: list[tuple[float, str]] = []

    def on_progress(progress: float, stage: str) -> None:
        calls.append((progress, stage))

    result = mastering_chain(
        samples=[0.1] * 22050,
        sample_rate=22050,
        config={
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


def test_mastering_chain_false_progress_result_does_not_cancel() -> None:
    """A progress callback's boolean return value is ignored."""
    from libsonare import mastering_chain

    calls: list[float] = []

    def on_progress(progress: float, _stage: str) -> object:
        calls.append(progress)
        return False

    result = mastering_chain(
        samples=[0.1] * 22050,
        sample_rate=22050,
        config={
            "eq": {"tilt": {"tiltDb": 1.0}},
            "dynamics": {"compressor": {"thresholdDb": -24.0}},
        },
        on_progress=on_progress,
    )

    assert any(progress > 0.5 for progress in calls)
    assert result.samples


def test_mastering_chain_cancel_callable_cancels_without_a_result() -> None:
    """The keyword-only cancellation callback is forwarded to the C ABI."""
    from libsonare import SonareError, mastering_chain

    result = None
    with pytest.raises(SonareError) as exc:
        result = mastering_chain(
            samples=[0.1] * 22050,
            sample_rate=22050,
            config={"eq": {"tilt": {"tiltDb": 1.0}}},
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
        mastering_chain_stereo,
    )

    samples = [0.1] * 22050
    result = None
    with pytest.raises(SonareError) as exc:
        if operation == "chain-stereo":
            result = mastering_chain_stereo(
                samples,
                samples,
                sample_rate=22050,
                config={"eq": {"tilt": {"tiltDb": 1.0}}},
                cancel=lambda: True,
            )
        elif operation == "preset-mono":
            result = master_audio(samples, sample_rate=22050, cancel=lambda: True)
        else:
            result = master_audio_stereo(samples, samples, sample_rate=22050, cancel=lambda: True)

    assert exc.value.code == 8
    assert result is None


def test_mastering_chain_nested_and_flat_config_equivalent() -> None:
    """The nested (canonical) and flat dot-notation configs select the same stages."""
    from libsonare import mastering_chain

    samples = [0.1] * 22050
    nested = mastering_chain(
        samples=samples,
        sample_rate=22050,
        config={
            "eq": {"tilt": {"tiltDb": 1.0}},
            "dynamics": {"compressor": {"thresholdDb": -24.0}},
        },
    )
    flat = mastering_chain(
        samples=samples,
        sample_rate=22050,
        config={"eq.tilt.tiltDb": 1.0, "dynamics.compressor.thresholdDb": -24.0},
    )
    assert nested.stages == flat.stages
    assert "eq.tilt" in nested.stages
    assert "dynamics.compressor" in nested.stages


def test_color_saturation_stages_engage_only_when_meaningful() -> None:
    """Tape/exciter must not auto-engage on a zero-valued config (no coloration)."""
    from libsonare import mastering_chain

    def stages_for(config: dict) -> list[str]:
        return mastering_chain(samples=[0.1] * 22050, sample_rate=22050, config=config).stages

    # A zero-amount exciter / zero-drive tape is a no-op and stays bypassed.
    assert "saturation.exciter" not in stages_for({"saturation.exciter.amount": 0.0})
    assert "saturation.tape" not in stages_for(
        {"saturation.tape.driveDb": 0.0, "saturation.tape.saturation": 0.0}
    )
    # Meaningful params engage the stage.
    assert "saturation.exciter" in stages_for({"saturation.exciter.amount": 0.2})
    # An explicit enabled flag wins either way.
    assert "saturation.exciter" in stages_for(
        {"saturation.exciter.amount": 0.0, "saturation.exciter.enabled": True}
    )
    assert "saturation.tape" not in stages_for(
        {"saturation.tape.driveDb": 3.0, "saturation.tape.enabled": False}
    )


# ============================================================================
# Newly exposed analysis functions: onset envelope, Fourier tempogram,
# tempogram ratio, NNLS chroma, and LUFS loudness metering.
# ============================================================================


def _all_finite(values: list[float]) -> bool:
    return all(math.isfinite(v) for v in values)


def test_stream_analyzer_counts_a_call_whose_input_it_scrubbed() -> None:
    """One process() call adds one, however many samples in it were bad.

    The analyzer replaces a non-finite input sample before it reaches the STFT
    and the onset/chroma histories, so nothing is missing from the output and
    every estimate is produced as usual -- this count is the only report that
    they stopped describing the input.
    """
    from libsonare import StreamAnalyzer, StreamConfig

    config = StreamConfig(sample_rate=8000, n_fft=32, hop_length=32, n_mels=8)
    with StreamAnalyzer(config) as analyzer:
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
