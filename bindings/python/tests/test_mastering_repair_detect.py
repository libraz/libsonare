"""Tests for the eight measure-only repair wrappers.

These entries run the analysis half of a repair and stop: they take the repair's
own config, fill a small POD struct the wrapper supplies, and allocate nothing,
so unlike every repair wrapper on this surface there is no output buffer to size
and nothing to release. That makes the interesting assertions behavioural rather
than structural, and three of them separate a measurement from the repair it
describes:

* ``detect_noise_floor`` REFUSES a buffer shorter than ``n_fft`` while
  ``detect_reverb`` PADS one. The two are checked against the SAME buffer at the
  same ``n_fft``, so the pair pins the divergence rather than two buffer lengths.
* ``detect_crackle`` measures by the median criterion whatever ``mode`` says, and
  ``detect_hum`` always runs the estimation path whatever ``adaptive`` says. Both
  are checked by passing the other setting and getting the same answer.
* ``detect_trim_range_stereo`` unions the two channels' ranges, and a channel
  with nothing above the threshold contributes NO edge. A naive ``min``/``max``
  over the two ranges would push ``last_exclusive`` out to the buffer's end, so
  the silent-channel case is what separates the contract from the arithmetic.

Where a detector has a repair counterpart that reports its own analysis, the two
are compared directly: the stereo repairs carry the channel's own detection in
their report, so a detector running a cheaper or differently configured scan
would disagree with the repair it claims to describe.
"""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import numpy as np
import pytest
from numpy.typing import NDArray

import libsonare

from ._helpers import LIB_AVAILABLE

pytestmark = pytest.mark.skipif(not LIB_AVAILABLE, reason="libsonare shared library missing")

SR = 22050
N = SR // 2  # half a second: every pipeline below resolves on it
DURATION_SEC = N / SR

CLICK_INDICES = (1000, 5000)
BURST_SPAN = (4000, 8000)
LATE_SPAN = (9000, 10500)  # disjoint from BURST_SPAN and inside the buffer


def _tone(length: int = N, freq: float = 440.0, amp: float = 0.2) -> NDArray[np.float32]:
    t = np.arange(length) / SR
    return (amp * np.sin(2.0 * np.pi * freq * t)).astype(np.float32)


def _clicky() -> NDArray[np.float32]:
    """A tone with two full-scale single-sample impulses."""
    out = _tone()
    out[CLICK_INDICES[0]] = 1.0
    out[CLICK_INDICES[1]] = -1.0
    return out


def _clipped() -> NDArray[np.float32]:
    return np.clip(_tone() * 8.0, -1.0, 1.0).astype(np.float32)


def _hummy() -> NDArray[np.float32]:
    return (_tone() + _tone(freq=50.0, amp=0.05)).astype(np.float32)


def _noise(sigma: float, seed: int = 7) -> NDArray[np.float32]:
    return np.random.default_rng(seed).normal(0.0, sigma, N).astype(np.float32)


def _burst() -> NDArray[np.float32]:
    out = np.zeros(N, dtype=np.float32)
    out[BURST_SPAN[0] : BURST_SPAN[1]] = 0.5
    return out


def _silence() -> NDArray[np.float32]:
    return np.zeros(N, dtype=np.float32)


class TestDetectClicks:
    def test_counts_the_runs_the_repair_would_act_on(self) -> None:
        detected = libsonare.mastering_repair_detect_clicks(_clicky(), SR)

        assert isinstance(detected, libsonare.ClickDetection)
        assert detected.count == len(CLICK_INDICES)
        assert detected.rejected == 0
        assert detected.longest_run_samples == 1
        assert detected.per_second == pytest.approx(detected.count / DURATION_SEC)

    def test_agrees_with_the_analysis_the_repair_reports(self) -> None:
        """The claim that this runs the repair's own LPC analysis.

        The stereo declicker reports each channel's own detection, so feeding it
        the same buffer twice under the same config gives an independent copy of
        what the repair measured. A cheaper threshold-only scan here would count
        runs the repair leaves alone and the two would part.
        """
        clicky = _clicky()

        detected = libsonare.mastering_repair_detect_clicks(clicky, SR)
        repaired = libsonare.mastering_repair_declick_stereo(clicky, clicky, SR)

        assert detected == repaired.left_report.detected
        assert detected == repaired.right_report.detected

    def test_clean_material_measures_zero(self) -> None:
        detected = libsonare.mastering_repair_detect_clicks(_tone(), SR)

        assert detected.count == 0
        assert detected.per_second == 0.0
        assert detected.longest_run_samples == 0

    def test_a_threshold_too_tight_for_the_material_shows_up_as_rejections(self) -> None:
        """``rejected`` is the field that says the config is wrong, not the audio.

        At the default the two impulses are counted and nothing is rejected; at a
        threshold low enough to select the tone itself the criteria throw most of
        it out again, which is the documented reading of a large ``rejected``.
        """
        clicky = _clicky()

        default = libsonare.mastering_repair_detect_clicks(clicky, SR)
        tight = libsonare.mastering_repair_detect_clicks(clicky, SR, threshold=0.1)

        assert default.rejected == 0
        assert tight.rejected > 0

    def test_refuses_a_non_positive_run_length_by_name(self) -> None:
        with pytest.raises(ValueError, match="max_click_samples"):
            libsonare.mastering_repair_detect_clicks(_clicky(), SR, max_click_samples=0)


class TestDetectNoiseFloor:
    def test_measures_the_floor_of_the_material_it_is_given(self) -> None:
        quiet = libsonare.mastering_repair_detect_noise_floor(_noise(0.001), SR)
        loud = libsonare.mastering_repair_detect_noise_floor(_noise(0.1), SR)

        assert isinstance(quiet, libsonare.NoiseDetection)
        assert len(quiet.band_floor_dbfs) == 32
        assert quiet.floor_dbfs < loud.floor_dbfs
        # Two decades of level separate the two buffers; a wrapper reading the
        # wrong struct field would not track them.
        assert loud.floor_dbfs - quiet.floor_dbfs > 20.0

    def test_refuses_a_buffer_shorter_than_n_fft(self) -> None:
        """One half of the sharpest divergence in this family.

        The other half is
        :meth:`TestDetectReverb.test_pads_a_buffer_shorter_than_n_fft`, which
        measures the SAME buffer at the SAME ``n_fft`` and accepts it.
        """
        short = _tone(length=1000)

        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_detect_noise_floor(short, SR, n_fft=1024)

    def test_the_refusal_is_about_n_fft_and_not_about_the_length(self) -> None:
        """The control for the refusal above: the same buffer, a smaller window.

        Without this the refusal would be equally consistent with "short buffers
        are refused", which is not the rule.
        """
        short = _tone(length=1000)

        detected = libsonare.mastering_repair_detect_noise_floor(
            short, SR, n_fft=256, hop_length=64
        )
        assert np.isfinite(detected.floor_dbfs)

    def test_refuses_a_non_power_of_two_window_by_name(self) -> None:
        with pytest.raises(ValueError, match="n_fft"):
            libsonare.mastering_repair_detect_noise_floor(_noise(0.01), SR, n_fft=1000)

    def test_refuses_a_non_positive_hop_by_name(self) -> None:
        with pytest.raises(ValueError, match="hop_length"):
            libsonare.mastering_repair_detect_noise_floor(_noise(0.01), SR, hop_length=0)

    def test_rejects_an_unknown_mode_and_estimator(self) -> None:
        noisy = _noise(0.01)

        with pytest.raises(ValueError, match="mode"):
            libsonare.mastering_repair_detect_noise_floor(noisy, SR, mode="nonexistent")
        with pytest.raises(ValueError, match="estimator"):
            libsonare.mastering_repair_detect_noise_floor(noisy, SR, noise_estimator="nonexistent")


class TestDetectClipping:
    def test_counts_samples_at_or_past_the_threshold(self) -> None:
        detected = libsonare.mastering_repair_detect_clipping(_clipped(), SR)

        assert isinstance(detected, libsonare.ClipDetection)
        assert detected.sample_count > 0
        assert detected.run_count > 0
        assert detected.sample_fraction == pytest.approx(detected.sample_count / N, rel=1e-5)
        assert detected.longest_run_samples > 0

    def test_agrees_with_the_analysis_the_repair_reports(self) -> None:
        clipped = _clipped()

        detected = libsonare.mastering_repair_detect_clipping(clipped, SR)
        repaired = libsonare.mastering_repair_declip_stereo(clipped, clipped, SR)

        assert detected == repaired.left_report.detected

    def test_clip_threshold_is_the_only_config_field_that_reaches_the_result(self) -> None:
        """Both halves are needed.

        The solver half alone would pass against a wrapper that marshalled
        nothing, and the threshold half alone would pass against one that
        marshalled every field into a detector that read them.
        """
        clipped = _clipped()

        default = libsonare.mastering_repair_detect_clipping(clipped, SR)
        other_solver = libsonare.mastering_repair_detect_clipping(
            clipped, SR, lpc_order=8, iterations=1, lpc_blend=0.1
        )
        assert other_solver == default

        lenient = libsonare.mastering_repair_detect_clipping(clipped, SR, clip_threshold=0.5)
        strict = libsonare.mastering_repair_detect_clipping(clipped, SR, clip_threshold=0.999)
        assert lenient.sample_count > strict.sample_count

    def test_clean_material_measures_zero(self) -> None:
        detected = libsonare.mastering_repair_detect_clipping(_tone(), SR)

        assert detected.sample_count == 0
        assert detected.run_count == 0
        assert detected.sample_fraction == 0.0


class TestDetectCrackle:
    def test_counts_samples_deviating_from_the_local_median(self) -> None:
        detected = libsonare.mastering_repair_detect_crackle(_clicky(), SR)

        assert isinstance(detected, libsonare.CrackleDetection)
        assert detected.sample_count == len(CLICK_INDICES)
        assert detected.sample_fraction == pytest.approx(detected.sample_count / N, rel=1e-5)
        assert detected.per_second == pytest.approx(detected.sample_count / DURATION_SEC)

    def test_measures_by_the_median_criterion_whatever_mode_says(self) -> None:
        """Wavelet mode never decides a sample is crackle, so it reports nothing.

        A wrapper that let ``mode`` reach a mode-dependent detector would answer
        differently here; the two must agree exactly.
        """
        clicky = _clicky()

        median = libsonare.mastering_repair_detect_crackle(clicky, SR, mode="median")
        wavelet = libsonare.mastering_repair_detect_crackle(clicky, SR, mode="waveletShrinkage")

        assert wavelet == median

    def test_threshold_is_live(self) -> None:
        clicky = _clicky()

        lenient = libsonare.mastering_repair_detect_crackle(clicky, SR, threshold=0.05)
        strict = libsonare.mastering_repair_detect_crackle(clicky, SR, threshold=0.9)

        assert strict.sample_count < lenient.sample_count

    def test_agrees_with_the_analysis_the_repair_reports(self) -> None:
        clicky = _clicky()

        detected = libsonare.mastering_repair_detect_crackle(clicky, SR)
        repaired = libsonare.mastering_repair_decrackle_stereo(clicky, clicky, SR)

        assert detected == repaired.left_report.detected

    def test_rejects_an_unknown_mode(self) -> None:
        with pytest.raises(ValueError, match="mode"):
            libsonare.mastering_repair_detect_crackle(_clicky(), SR, mode="nonexistent")


class TestDetectHum:
    def test_finds_the_fundamental_and_its_level(self) -> None:
        detected = libsonare.mastering_repair_detect_hum(_hummy(), SR)

        assert isinstance(detected, libsonare.HumDetection)
        assert len(detected.harmonic_dbfs) == 16
        assert detected.fundamental_hz == pytest.approx(50.0, abs=2.0)
        assert detected.harmonics >= 1
        # The hum sits 35 dB above where the same bin reads on the clean tone,
        # so the level is being measured rather than defaulted.
        clean = libsonare.mastering_repair_detect_hum(_tone(), SR)
        assert detected.harmonic_dbfs[0] - clean.harmonic_dbfs[0] > 30.0

    def test_runs_the_estimation_path_whatever_adaptive_says(self) -> None:
        """The fixed path never looks for hum, so a detector following the flag
        would hand back its own input. Both settings must measure the same."""
        hummy = _hummy()

        off = libsonare.mastering_repair_detect_hum(hummy, SR, adaptive=False)
        on = libsonare.mastering_repair_detect_hum(hummy, SR, adaptive=True)

        assert on == off

    def test_agrees_with_the_analysis_the_repair_reports(self) -> None:
        hummy = _hummy()

        detected = libsonare.mastering_repair_detect_hum(hummy, SR)
        repaired = libsonare.mastering_repair_dehum_stereo(hummy, hummy, SR)

        assert detected == repaired.left_report.detected

    def test_a_harmonic_past_nyquist_reads_the_floor(self) -> None:
        """16 harmonics of a 5 kHz fundamental cannot all fit under Nyquist.

        The trailing entries are the dB floor because nothing is there to
        measure, not because they went unmeasured.
        """
        detected = libsonare.mastering_repair_detect_hum(
            _tone(freq=5000.0), SR, fundamental_hz=5000.0
        )

        assert detected.harmonic_dbfs[-1] == pytest.approx(-120.0)


class TestDetectReverb:
    def test_measures_the_module_s_own_late_decay_statistic(self) -> None:
        detected = libsonare.mastering_repair_detect_reverb(_tone(), SR)

        assert isinstance(detected, libsonare.ReverbDetection)
        assert np.isfinite(detected.late_decay_ratio_db)

    def test_pads_a_buffer_shorter_than_n_fft(self) -> None:
        """The other half of the divergence pinned in ``TestDetectNoiseFloor``.

        Same buffer, same ``n_fft``: the denoise detector refuses it and this one
        measures it. The result is checked for a non-zero statistic so the
        acceptance is a measurement rather than the zeroed struct a refused call
        would leave.
        """
        short = _tone(length=1000)

        detected = libsonare.mastering_repair_detect_reverb(short, SR, n_fft=1024)

        assert detected.late_decay_ratio_db != 0.0
        with pytest.raises(libsonare.SonareError):
            libsonare.mastering_repair_detect_noise_floor(short, SR, n_fft=1024)

    def test_late_predictability_is_zero_unless_the_wpe_stage_runs(self) -> None:
        tone = _tone()

        off = libsonare.mastering_repair_detect_reverb(tone, SR)
        on = libsonare.mastering_repair_detect_reverb(tone, SR, wpe_enabled=True)

        assert off.late_predictability == 0.0
        assert on.late_predictability > 0.0

    def test_agrees_with_the_analysis_the_repair_reports(self) -> None:
        """Every field here is a ratio, so the stereo mask's figure is comparable."""
        tone = _tone()

        detected = libsonare.mastering_repair_detect_reverb(tone, SR)
        repaired = libsonare.mastering_repair_dereverb_classical_stereo(tone, tone, SR)

        assert detected == repaired.report.detected

    def test_refuses_a_non_power_of_two_window_by_name(self) -> None:
        with pytest.raises(ValueError, match="n_fft"):
            libsonare.mastering_repair_detect_reverb(_tone(), SR, n_fft=1000)

    def test_refuses_a_hop_outside_the_window(self) -> None:
        with pytest.raises(ValueError, match="hop_length"):
            libsonare.mastering_repair_detect_reverb(_tone(), SR, n_fft=1024, hop_length=2048)


class TestDetectTrimRange:
    def test_reports_the_range_the_repair_would_cut_to(self) -> None:
        burst = _burst()

        kept = libsonare.mastering_repair_detect_trim_range(burst, SR)

        assert isinstance(kept, libsonare.TrimRange)
        assert (kept.first, kept.last_exclusive) == BURST_SPAN
        assert len(libsonare.mastering_repair_trim_silence(burst, SR)) == (
            kept.last_exclusive - kept.first
        )

    def test_the_padding_is_already_inside_the_returned_range(self) -> None:
        """The documented trap: this is the cut, not the detected extent.

        A caller adding ``padding_samples`` to the range itself would double it.
        """
        pad = 500

        kept = libsonare.mastering_repair_detect_trim_range(_burst(), SR, padding_samples=pad)

        assert kept.first == BURST_SPAN[0] - pad
        assert kept.last_exclusive == BURST_SPAN[1] + pad

    def test_a_buffer_with_nothing_above_the_threshold_reports_length_length(self) -> None:
        kept = libsonare.mastering_repair_detect_trim_range(_silence(), SR)

        assert (kept.first, kept.last_exclusive) == (N, N)

    def test_threshold_is_live(self) -> None:
        burst = _burst()

        assert (
            libsonare.mastering_repair_detect_trim_range(burst, SR, threshold=0.001).first
            == BURST_SPAN[0]
        )
        above_the_burst = libsonare.mastering_repair_detect_trim_range(burst, SR, threshold=1.0)
        assert (above_the_burst.first, above_the_burst.last_exclusive) == (N, N)

    def test_refuses_a_negative_padding_count_before_it_becomes_a_size_t(self) -> None:
        with pytest.raises(ValueError, match="padding_samples"):
            libsonare.mastering_repair_detect_trim_range(_burst(), SR, padding_samples=-1)


class TestDetectTrimRangeStereo:
    def test_unions_the_two_channels_ranges(self) -> None:
        """Disjoint spans, so each edge comes from a different channel.

        A pair that agreed would pass equally against a wrapper reading only one
        of the two scans.
        """
        left = _burst()
        right = np.zeros(N, dtype=np.float32)
        right[LATE_SPAN[0] : LATE_SPAN[1]] = 0.5

        kept = libsonare.mastering_repair_detect_trim_range_stereo(left, right, SR)

        assert (kept.first, kept.last_exclusive) == (BURST_SPAN[0], LATE_SPAN[1])

    def test_a_silent_channel_contributes_no_edge(self) -> None:
        """The one assertion a naive ``min``/``max`` implementation fails.

        A silent channel's own range is ``(N, N)``, so unioning it numerically
        would leave ``last_exclusive`` at the end of the buffer and trim nothing
        off the tail. The union of a silent channel and an active one has to be
        the active channel's range exactly, and the silent channel is checked on
        BOTH sides so an implementation special-casing only one would fail.
        """
        burst = _burst()
        silence = _silence()

        right_silent = libsonare.mastering_repair_detect_trim_range_stereo(burst, silence, SR)
        left_silent = libsonare.mastering_repair_detect_trim_range_stereo(silence, burst, SR)

        assert (right_silent.first, right_silent.last_exclusive) == BURST_SPAN
        assert (left_silent.first, left_silent.last_exclusive) == BURST_SPAN

    def test_two_silent_channels_report_length_length(self) -> None:
        silence = _silence()

        kept = libsonare.mastering_repair_detect_trim_range_stereo(silence, silence, SR)

        assert (kept.first, kept.last_exclusive) == (N, N)

    def test_matches_the_range_the_stereo_repair_applies(self) -> None:
        left = _burst()
        right = _silence()

        kept = libsonare.mastering_repair_detect_trim_range_stereo(left, right, SR)
        trimmed = libsonare.mastering_repair_trim_silence_stereo(left, right, SR)

        assert kept == trimmed.report.range
        assert trimmed.length == kept.last_exclusive - kept.first

    def test_no_downmix_is_read(self) -> None:
        """An antiphase pair cancels to nothing under a mono sum.

        Each channel is scanned on its own, so full-level antiphase audio is kept
        rather than read as silence.
        """
        burst = _burst()

        kept = libsonare.mastering_repair_detect_trim_range_stereo(burst, -burst, SR)

        assert (kept.first, kept.last_exclusive) == BURST_SPAN

    def test_rejects_mismatched_channel_lengths(self) -> None:
        burst = _burst()

        with pytest.raises(ValueError, match="length"):
            libsonare.mastering_repair_detect_trim_range_stereo(burst, burst[:-10], SR)

    def test_refuses_a_negative_padding_count(self) -> None:
        burst = _burst()

        with pytest.raises(ValueError, match="padding_samples"):
            libsonare.mastering_repair_detect_trim_range_stereo(
                burst, burst, SR, padding_samples=-1
            )


# The six mono detectors plus the mono trim scan, each with a buffer its own
# analysis accepts. Parametrized so a detector added later without its refusals
# wired up fails here rather than going unchecked.
_MONO_ENTRIES: tuple[tuple[str, Callable[..., Any]], ...] = (
    ("detect_clicks", libsonare.mastering_repair_detect_clicks),
    ("detect_noise_floor", libsonare.mastering_repair_detect_noise_floor),
    ("detect_clipping", libsonare.mastering_repair_detect_clipping),
    ("detect_crackle", libsonare.mastering_repair_detect_crackle),
    ("detect_hum", libsonare.mastering_repair_detect_hum),
    ("detect_reverb", libsonare.mastering_repair_detect_reverb),
    ("detect_trim_range", libsonare.mastering_repair_detect_trim_range),
)


@pytest.mark.parametrize(("name", "entry"), _MONO_ENTRIES, ids=[n for n, _ in _MONO_ENTRIES])
class TestMonoRefusals:
    def test_refuses_an_empty_buffer_by_name(self, name: str, entry: Callable[..., Any]) -> None:
        with pytest.raises(libsonare.SonareValueError, match="samples"):
            entry(np.zeros(0, dtype=np.float32), SR)

    def test_refuses_a_non_finite_sample_by_name(
        self, name: str, entry: Callable[..., Any]
    ) -> None:
        tone = _tone()
        tone[7] = np.nan

        with pytest.raises(libsonare.SonareValueError, match="samples"):
            entry(tone, SR)

    def test_the_native_layer_refuses_an_out_of_range_sample_rate(
        self, name: str, entry: Callable[..., Any]
    ) -> None:
        """Validated natively, so the refusal is a ``SonareError`` and not the
        facade's own ``SonareValueError``. The following call proves the refused
        one left nothing behind: the out-struct is zeroed before the argument
        checks, so there is no half-filled result to carry over."""
        tone = _tone()

        with pytest.raises(libsonare.SonareError) as excinfo:
            entry(tone, 0)
        assert not isinstance(excinfo.value, libsonare.SonareValueError)

        assert entry(tone, SR) is not None

    def test_leaves_the_input_untouched(self, name: str, entry: Callable[..., Any]) -> None:
        """These measure; nothing here may write through the caller's buffer.

        The wrapper hands the C side a zero-copy view of a contiguous float32
        array, so an entry that wrote to its input would corrupt the caller's.
        """
        tone = _tone()
        before = tone.copy()

        entry(tone, SR)

        assert np.array_equal(tone, before)


class TestDefaultsPath:
    """Omitting every keyword must equal spelling the documented defaults out.

    The C ABI takes a NULL config to mean "library defaults", but this surface
    always materializes one from its keyword defaults, so what is pinned here is
    that those keyword defaults are the values the docstrings claim. A default
    drifting away from its sibling repair's would otherwise be invisible.
    """

    def test_detect_clicks(self) -> None:
        clicky = _clicky()
        assert libsonare.mastering_repair_detect_clicks(
            clicky,
            SR,
            threshold=0.8,
            neighbor_ratio=4.0,
            max_click_samples=8,
            lpc_order=20,
            residual_ratio=8.0,
        ) == libsonare.mastering_repair_detect_clicks(clicky, SR)

    def test_detect_noise_floor(self) -> None:
        noisy = _noise(0.01)
        assert libsonare.mastering_repair_detect_noise_floor(
            noisy,
            SR,
            mode="logMmse",
            noise_estimator="quantile",
            n_fft=1024,
            hop_length=256,
            dd_alpha=0.98,
            reduction_db=26.0,
            over_subtraction=2.0,
            spectral_floor=0.05,
            noise_estimation_quantile=0.1,
            speech_presence_gain=True,
            gain_smoothing=True,
        ) == libsonare.mastering_repair_detect_noise_floor(noisy, SR)

    def test_detect_clipping(self) -> None:
        clipped = _clipped()
        assert libsonare.mastering_repair_detect_clipping(
            clipped, SR, clip_threshold=0.98, lpc_order=36, iterations=2, lpc_blend=0.65
        ) == libsonare.mastering_repair_detect_clipping(clipped, SR)

    def test_detect_crackle(self) -> None:
        clicky = _clicky()
        assert libsonare.mastering_repair_detect_crackle(
            clicky, SR, threshold=0.4, mode="median", levels=4
        ) == libsonare.mastering_repair_detect_crackle(clicky, SR)

    def test_detect_hum(self) -> None:
        hummy = _hummy()
        assert libsonare.mastering_repair_detect_hum(
            hummy,
            SR,
            fundamental_hz=50.0,
            harmonics=4,
            q=20.0,
            adaptive=False,
            search_range_hz=2.0,
            adaptation=0.25,
            frame_size=2048,
            pll_bandwidth=0.01,
        ) == libsonare.mastering_repair_detect_hum(hummy, SR)

    def test_detect_reverb(self) -> None:
        tone = _tone()
        assert libsonare.mastering_repair_detect_reverb(
            tone,
            SR,
            threshold=0.0,
            attenuation=1.0,
            n_fft=1024,
            hop_length=256,
            t60_sec=0.4,
            late_delay_ms=50.0,
            over_subtraction=1.0,
            spectral_floor=0.08,
            wpe_enabled=False,
            wpe_iterations=2,
            wpe_taps=3,
            wpe_strength=0.7,
        ) == libsonare.mastering_repair_detect_reverb(tone, SR)

    def test_detect_trim_range(self) -> None:
        burst = _burst()
        spelled = libsonare.mastering_repair_detect_trim_range(
            burst,
            SR,
            threshold=0.001,
            padding_samples=0,
            mode="peak",
            gate_lufs=-60.0,
            window_ms=400.0,
        )
        assert spelled == libsonare.mastering_repair_detect_trim_range(burst, SR)

    def test_detect_trim_range_stereo(self) -> None:
        burst = _burst()
        silence = _silence()
        spelled = libsonare.mastering_repair_detect_trim_range_stereo(
            burst,
            silence,
            SR,
            threshold=0.001,
            padding_samples=0,
            mode="peak",
            gate_lufs=-60.0,
            window_ms=400.0,
        )
        assert spelled == libsonare.mastering_repair_detect_trim_range_stereo(burst, silence, SR)


class TestStereoRefusals:
    def test_refuses_an_empty_buffer_by_name(self) -> None:
        empty = np.zeros(0, dtype=np.float32)

        with pytest.raises(libsonare.SonareValueError, match="left"):
            libsonare.mastering_repair_detect_trim_range_stereo(empty, empty, SR)

    def test_refuses_a_non_finite_sample_by_name(self) -> None:
        burst = _burst()
        spoiled = burst.copy()
        spoiled[7] = np.inf

        with pytest.raises(libsonare.SonareValueError, match="left"):
            libsonare.mastering_repair_detect_trim_range_stereo(spoiled, burst, SR)
        with pytest.raises(libsonare.SonareValueError, match="right"):
            libsonare.mastering_repair_detect_trim_range_stereo(burst, spoiled, SR)

    def test_the_native_layer_refuses_an_out_of_range_sample_rate(self) -> None:
        burst = _burst()

        with pytest.raises(libsonare.SonareError) as excinfo:
            libsonare.mastering_repair_detect_trim_range_stereo(burst, burst, 0)
        assert not isinstance(excinfo.value, libsonare.SonareValueError)

        assert libsonare.mastering_repair_detect_trim_range_stereo(burst, burst, SR) is not None
