"""Empty / NaN-Inf input guards for metering / dynamics / voice-changer wrappers.

Source-side helpers (`_validate_samples` in `_runtime.py`) raise
`SonareValueError` with `{fn_name}: {arg_name}` prefixes so callers can
pattern-match on the wrapper rather than on a generic C error string. These
tests pin the behavior across all in-scope public wrappers, along with the
dual-base catch contract that keeps `except ValueError` and `except SonareError`
equally valid.
"""

from __future__ import annotations

import array
import ast
import math
import pathlib

import numpy as np
import pytest

import libsonare
from libsonare import (
    bass_chroma,
    chroma,
    chroma_cens,
    chroma_cqt,
    ebur128_loudness_range,
    harmonic,
    lufs,
    lufs_interleaved,
    mastering_dynamics_compressor,
    mastering_dynamics_gate,
    mastering_dynamics_transient_shaper,
    mel_spectrogram,
    metering_detect_clipping,
    metering_dynamic_range,
    metering_peak_db,
    metering_rms_db,
    metering_spectrum,
    metering_spectrum_frame,
    metering_stereo_correlation,
    metering_true_peak_db,
    mfcc,
    momentary_lufs,
    normalize,
    percussive,
    phase_vocoder,
    pitch_pyin,
    pitch_shift,
    pitch_tuning,
    pitch_yin,
    short_term_lufs,
    stft,
    time_stretch,
    waveform_peak_pyramid,
    waveform_peaks,
    zero_crossings,
)
from libsonare._effects import voice_change, voice_change_realtime
from libsonare._runtime import SonareError, SonareValueError

SR = 22050

# Every public wrapper that routes its primary buffer through
# ``_validate_samples``. Keeping this list aligned with the Node/WASM guard
# suites ensures a Python-only regression in the finite/empty preflight fails a
# test rather than propagating NaN downstream. Each entry maps a wrapper name to
# an invoker taking the validated buffer as its first argument.
_SAMPLE_WRAPPERS = {
    "lufs": lambda buf: lufs(buf, SR),
    "momentary_lufs": lambda buf: momentary_lufs(buf, SR),
    "short_term_lufs": lambda buf: short_term_lufs(buf, SR),
    "lufs_interleaved": lambda buf: lufs_interleaved(buf, 1, SR),
    "ebur128_loudness_range": lambda buf: ebur128_loudness_range(buf, SR),
    "metering_true_peak_db": lambda buf: metering_true_peak_db(buf, SR),
    "metering_detect_clipping": lambda buf: metering_detect_clipping(buf, SR),
    "metering_dynamic_range": lambda buf: metering_dynamic_range(buf, SR),
    "metering_spectrum": lambda buf: metering_spectrum(buf, SR),
    "metering_spectrum_frame": lambda buf: metering_spectrum_frame(buf, SR),
    "waveform_peaks": lambda buf: waveform_peaks(buf, 1),
    "waveform_peak_pyramid": lambda buf: waveform_peak_pyramid(buf, 1),
    "mel_spectrogram": lambda buf: mel_spectrogram(buf, SR),
    "mfcc": lambda buf: mfcc(buf, SR),
    "stft": lambda buf: stft(buf, SR),
    "chroma": lambda buf: chroma(buf, SR),
    "chroma_cens": lambda buf: chroma_cens(buf, SR),
    "chroma_cqt": lambda buf: chroma_cqt(buf, SR),
    "bass_chroma": lambda buf: bass_chroma(buf, SR),
    "zero_crossings": lambda buf: zero_crossings(buf),
    "pitch_tuning": lambda buf: pitch_tuning(buf),
    "harmonic": lambda buf: harmonic(buf, SR),
    "percussive": lambda buf: percussive(buf, SR),
    "normalize": lambda buf: normalize(buf, SR),
    "time_stretch": lambda buf: time_stretch(buf, SR),
    "pitch_shift": lambda buf: pitch_shift(buf, SR),
    "phase_vocoder": lambda buf: phase_vocoder(buf, SR),
    "pitch_pyin": lambda buf: pitch_pyin(buf, SR),
    "pitch_yin": lambda buf: pitch_yin(buf, SR),
    "voice_change": lambda buf: voice_change(buf),
    "voice_change_realtime": lambda buf: voice_change_realtime(buf),
    "mastering_dynamics_compressor": lambda buf: mastering_dynamics_compressor(buf, SR),
    "mastering_dynamics_gate": lambda buf: mastering_dynamics_gate(buf, SR),
    "mastering_dynamics_transient_shaper": lambda buf: mastering_dynamics_transient_shaper(buf, SR),
}


def _sine(n: int = 1024, freq: float = 440.0, amp: float = 0.5) -> np.ndarray:
    t = np.arange(n, dtype=np.float32) / SR
    return (amp * np.sin(2 * np.pi * freq * t)).astype(np.float32)


def _with_nan(n: int = 1024, index: int = 100) -> np.ndarray:
    buf = _sine(n)
    buf[index] = math.nan
    return buf


def _with_inf(n: int = 1024, index: int = 200) -> np.ndarray:
    buf = _sine(n)
    buf[index] = math.inf
    return buf


def _with_neg_inf(n: int = 1024, index: int = 321) -> np.ndarray:
    buf = _sine(n)
    buf[index] = -math.inf
    return buf


class TestEmptyGuards:
    def test_lufs_rejects_empty(self):
        with pytest.raises(ValueError, match=r"lufs: samples must not be empty"):
            lufs(np.empty(0, dtype=np.float32), SR)

    def test_metering_peak_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_peak_db: samples must not be empty"):
            metering_peak_db(np.empty(0, dtype=np.float32))

    def test_metering_rms_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_rms_db: samples must not be empty"):
            metering_rms_db(np.empty(0, dtype=np.float32))

    def test_metering_true_peak_db_rejects_empty(self):
        with pytest.raises(ValueError, match=r"metering_true_peak_db: samples must not be empty"):
            metering_true_peak_db(np.empty(0, dtype=np.float32), SR)

    def test_metering_stereo_correlation_rejects_empty_left(self):
        with pytest.raises(
            ValueError,
            match=r"metering_stereo_correlation: left must not be empty",
        ):
            metering_stereo_correlation(np.empty(0, dtype=np.float32), _sine())

    def test_mastering_dynamics_compressor_rejects_empty(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_compressor: samples must not be empty",
        ):
            mastering_dynamics_compressor(np.empty(0, dtype=np.float32), SR)

    def test_mastering_dynamics_gate_rejects_empty(self):
        with pytest.raises(ValueError, match=r"mastering_dynamics_gate: samples must not be empty"):
            mastering_dynamics_gate(np.empty(0, dtype=np.float32), SR)

    def test_mastering_dynamics_transient_shaper_rejects_empty(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_transient_shaper: samples must not be empty",
        ):
            mastering_dynamics_transient_shaper(np.empty(0, dtype=np.float32), SR)

    def test_voice_change_rejects_empty(self):
        with pytest.raises(ValueError, match=r"voice_change: samples must not be empty"):
            voice_change(np.empty(0, dtype=np.float32))

    def test_voice_change_realtime_rejects_empty(self):
        with pytest.raises(ValueError, match=r"voice_change_realtime: samples must not be empty"):
            voice_change_realtime(np.empty(0, dtype=np.float32))


class TestNanInfGuards:
    def test_lufs_rejects_nan_with_index(self):
        with pytest.raises(ValueError, match=r"lufs: samples contains NaN or Inf at index 100"):
            lufs(_with_nan(), SR)

    def test_lufs_rejects_inf_with_index(self):
        with pytest.raises(ValueError, match=r"lufs: samples contains NaN or Inf at index 200"):
            lufs(_with_inf(), SR)

    def test_mastering_dynamics_compressor_rejects_nan(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_compressor: samples contains NaN or Inf at index 100",
        ):
            mastering_dynamics_compressor(_with_nan(), SR)

    def test_mastering_dynamics_gate_rejects_inf(self):
        with pytest.raises(
            ValueError,
            match=r"mastering_dynamics_gate: samples contains NaN or Inf at index 200",
        ):
            mastering_dynamics_gate(_with_inf(), SR)

    def test_metering_peak_db_rejects_nan(self):
        with pytest.raises(
            ValueError, match=r"metering_peak_db: samples contains NaN or Inf at index 100"
        ):
            metering_peak_db(_with_nan())


class TestValidateFalseStillHasCAbiBackstop:
    def test_lufs_validate_false(self):
        # validate=False skips only the Python preflight; C-ABI validation is
        # mandatory and must still reject NaN input.
        with pytest.raises(SonareError) as exc_info:
            lufs(_with_nan(), SR, validate=False)
        assert exc_info.value.code == 4

    def test_mastering_dynamics_compressor_validate_false(self):
        with pytest.raises(SonareError) as exc_info:
            mastering_dynamics_compressor(_with_nan(), SR, validate=False)
        assert exc_info.value.code == 4


class TestPitchCAbiValidation:
    @pytest.mark.parametrize(
        "pitch_name, pitch", [("pitch_yin", pitch_yin), ("pitch_pyin", pitch_pyin)]
    )
    def test_rejects_negative_inf_with_index(self, pitch_name, pitch):
        with pytest.raises(
            ValueError,
            match=rf"{pitch_name}: samples contains NaN or Inf at index 321",
        ):
            pitch(_with_neg_inf(), sample_rate=SR)

    @pytest.mark.parametrize("pitch", [pitch_yin, pitch_pyin], ids=["yin", "pyin"])
    def test_invalid_sample_rate_keeps_sonare_error_code(self, pitch):
        with pytest.raises(SonareError) as exc_info:
            pitch(_sine(SR), sample_rate=0)
        assert exc_info.value.code == 4

    @pytest.mark.parametrize("pitch", [pitch_yin, pitch_pyin], ids=["yin", "pyin"])
    def test_normal_audio_returns_pitch_result(self, pitch):
        result = pitch(_sine(SR), sample_rate=SR)
        assert result.n_frames > 0
        assert len(result.f0) == result.n_frames
        assert len(result.voiced_prob) == result.n_frames
        assert len(result.voiced_flag) == result.n_frames


# Wrappers whose validated buffer is not called ``samples``; the guard names the
# real parameter, so the expected message differs.
_BUFFER_ARG_NAMES = {"pitch_tuning": "frequencies"}


def _buffer_arg(name: str) -> str:
    return _BUFFER_ARG_NAMES.get(name, "samples")


class TestSampleWrapperGuardCoverage:
    """Parametrized empty / NaN / Inf coverage across every guarded buffer wrapper."""

    @pytest.mark.parametrize("name", sorted(_SAMPLE_WRAPPERS))
    def test_rejects_empty(self, name):
        arg = _buffer_arg(name)
        with pytest.raises(ValueError, match=rf"{name}: {arg} must not be empty"):
            _SAMPLE_WRAPPERS[name](np.empty(0, dtype=np.float32))

    @pytest.mark.parametrize("name", sorted(_SAMPLE_WRAPPERS))
    def test_rejects_nan_with_index(self, name):
        arg = _buffer_arg(name)
        with pytest.raises(ValueError, match=rf"{name}: {arg} contains NaN or Inf at index 100"):
            _SAMPLE_WRAPPERS[name](_with_nan())

    @pytest.mark.parametrize("name", sorted(_SAMPLE_WRAPPERS))
    def test_rejects_inf_with_index(self, name):
        arg = _buffer_arg(name)
        with pytest.raises(ValueError, match=rf"{name}: {arg} contains NaN or Inf at index 200"):
            _SAMPLE_WRAPPERS[name](_with_inf())


class TestSonareValueErrorCatchContract:
    """``SonareValueError`` must satisfy every catch style callers already use."""

    def test_caught_as_value_error(self):
        with pytest.raises(ValueError):
            lufs(np.empty(0, dtype=np.float32), SR)

    def test_caught_as_sonare_error(self):
        with pytest.raises(SonareError):
            lufs(np.empty(0, dtype=np.float32), SR)

    def test_caught_as_sonare_value_error(self):
        with pytest.raises(SonareValueError):
            lufs(np.empty(0, dtype=np.float32), SR)

    def test_single_instance_satisfies_all_three_bases(self):
        with pytest.raises(SonareValueError) as exc_info:
            lufs(np.empty(0, dtype=np.float32), SR)
        raised = exc_info.value
        assert isinstance(raised, ValueError)
        assert isinstance(raised, SonareError)
        assert isinstance(raised, RuntimeError)

    def test_message_carries_no_error_code_prefix(self):
        with pytest.raises(SonareValueError) as exc_info:
            lufs(np.empty(0, dtype=np.float32), SR)
        assert str(exc_info.value) == "lufs: samples must not be empty"

    def test_reports_the_invalid_parameter_code(self):
        with pytest.raises(SonareValueError) as exc_info:
            lufs(np.empty(0, dtype=np.float32), SR)
        assert exc_info.value.code == 4
        assert exc_info.value.code_name == "InvalidParameter"

    def test_set_midi_clips_group_overflow_is_a_sonare_value_error(self):
        """The one site the sweep below found, pinned by behaviour as well.

        A group above 255 is caught in Python because ``c_uint8`` would wrap it
        onto a value the C ABI's own [0, 15] check accepts. The existing engine
        coverage passes ``group=16``, which the C ABI rejects, so this branch
        had no test and its error class went unnoticed.
        """
        from libsonare import EngineMidiClipSchedule, EngineMidiEvent, RealtimeEngine

        with (
            RealtimeEngine(sample_rate=48000.0, max_block_size=128) as engine,
            pytest.raises(SonareValueError) as exc_info,
        ):
            engine.set_midi_clips(
                [
                    EngineMidiClipSchedule(
                        id=1,
                        track_id=6,
                        destination_id=6,
                        events=[EngineMidiEvent(0, word0=0, word_count=1, group=256)],
                    )
                ]
            )
        assert isinstance(exc_info.value, SonareError)
        assert exc_info.value.code == 4

    def test_no_library_module_raises_a_bare_value_error(self):
        """The contract is a property of the package, not of one probe function.

        A bare ``ValueError`` satisfies ``except ValueError`` but escapes
        ``except SonareError`` and carries no ``.code``, so it cannot be mapped
        to an exit code. Probing a single entry point cannot see the one module
        that still raises it, which is how ``set_midi_clips`` stayed a plain
        ``ValueError`` after the hierarchy was introduced.

        The CLI modules are out of scope: their raises are argparse-level
        control flow that ``cli.main`` catches and turns into an exit code, not
        rejections of a library argument.
        """
        package = pathlib.Path(libsonare.__file__).parent
        offenders: list[str] = []
        for path in sorted(package.glob("*.py")):
            if path.name == "cli.py" or path.name.startswith("_cli_"):
                continue
            tree = ast.parse(path.read_text(encoding="utf-8"))
            for node in ast.walk(tree):
                if not isinstance(node, ast.Raise) or node.exc is None:
                    continue
                raised = node.exc.func if isinstance(node.exc, ast.Call) else node.exc
                if isinstance(raised, ast.Name) and raised.id == "ValueError":
                    offenders.append(f"{path.name}:{node.lineno}")
        assert offenders == [], (
            "library modules must raise SonareValueError so the rejection is "
            f"catchable as both ValueError and SonareError: {offenders}"
        )


# Spectral transforms whose buffer used to reach the C ABI unchecked, so an
# empty or non-finite input surfaced as a bare ``[4] Invalid parameter``.
_SPECTRAL_TRANSFORMS = {
    "mel_spectrogram": mel_spectrogram,
    "mfcc": mfcc,
    "stft": stft,
    "chroma": chroma,
    "chroma_cens": chroma_cens,
    "chroma_cqt": chroma_cqt,
    "bass_chroma": bass_chroma,
}


class TestSpectralTransformsMatchSiblingGuards:
    """Each spectral transform routes its buffer through the sibling preflight."""

    @pytest.mark.parametrize("name", sorted(_SPECTRAL_TRANSFORMS))
    def test_empty_raises_sibling_exception_type(self, name):
        empty = np.empty(0, dtype=np.float32)
        with pytest.raises(SonareValueError) as sibling_info:
            pitch_yin(empty, SR)
        with pytest.raises(SonareValueError) as exc_info:
            _SPECTRAL_TRANSFORMS[name](empty, SR)
        assert type(exc_info.value) is type(sibling_info.value)

    @pytest.mark.parametrize("name", sorted(_SPECTRAL_TRANSFORMS))
    def test_nan_raises_sibling_exception_type(self, name):
        buf = _with_nan()
        with pytest.raises(SonareValueError) as sibling_info:
            pitch_yin(buf, SR)
        with pytest.raises(SonareValueError) as exc_info:
            _SPECTRAL_TRANSFORMS[name](buf, SR)
        assert type(exc_info.value) is type(sibling_info.value)

    @pytest.mark.parametrize("name", sorted(_SPECTRAL_TRANSFORMS))
    def test_message_uses_the_wrapper_name(self, name):
        # The chroma variants share one private helper, so the message must name
        # the entry point the caller used rather than the helper or the C symbol.
        with pytest.raises(SonareValueError, match=rf"{name}: samples must not be empty"):
            _SPECTRAL_TRANSFORMS[name](np.empty(0, dtype=np.float32), SR)

    @pytest.mark.parametrize("name", sorted(_SPECTRAL_TRANSFORMS))
    def test_invalid_sample_rate_keeps_sonare_error_code(self, name):
        # Preflight covers the buffer only; scalar arguments stay the C ABI's job.
        with pytest.raises(SonareError) as exc_info:
            _SPECTRAL_TRANSFORMS[name](_sine(SR), sample_rate=0)
        assert type(exc_info.value) is SonareError
        assert exc_info.value.code == 4


class TestNonFiniteInputYieldsNoPlausibleResult:
    """Input with no usable data must fail rather than resolve to a normal answer."""

    def test_zero_crossings_rejects_nan_rather_than_fabricating_crossings(self):
        # A non-finite sample compares unequal to itself, so the sign test would
        # report crossings on both sides of it that the clean signal does not have.
        alternating = np.array([1.0, -1.0] * 16, dtype=np.float32)
        assert len(zero_crossings(alternating)) > 0
        corrupted = alternating.copy()
        corrupted[10] = math.nan
        with pytest.raises(SonareValueError, match=r"zero_crossings: samples contains NaN"):
            zero_crossings(corrupted)

    def test_pitch_tuning_rejects_an_all_non_finite_track(self):
        # Non-finite entries are dropped like non-positive ones, so a track with
        # nothing usable in it must not resolve to 0.0 -- a legitimate "perfectly
        # in tune" answer that no caller could distinguish from a real one.
        assert math.isfinite(pitch_tuning([440.0, 880.0, 660.0]))
        with pytest.raises(SonareValueError, match=r"pitch_tuning: frequencies contains NaN"):
            pitch_tuning(np.full(32, math.nan, dtype=np.float32))


class TestPositiveSmoke:
    def test_metering_rms_db_finite_on_sine(self):
        v = metering_rms_db(_sine(SR))
        assert math.isfinite(v)

    def test_mastering_dynamics_compressor_returns_same_length(self):
        sig = _sine(SR)
        out, latency = mastering_dynamics_compressor(sig, SR)
        assert len(out) == len(sig)
        assert isinstance(latency, int)


class TestDocumentedBufferInputForms:
    """Every input form `_as_float32_buffer` documents, thrown at a guarded entry.

    The helper's comment used to list ``generator`` alongside the sequence
    types. NumPy cannot convert one, so a generator escaped the facade as a bare
    ``TypeError`` — the one shape of bad input that bypassed the guard's promise
    to name the function and the argument. Tests only ever passed ndarrays, so
    nothing noticed.
    """

    ACCEPTED = {
        "list": lambda values: list(values),
        "tuple": lambda values: tuple(values),
        "array.array": lambda values: array.array("f", values),
        "range": lambda _values: range(1, 65),
        "memoryview": lambda values: memoryview(np.asarray(values, dtype=np.float32)),
        "ndarray_float32": lambda values: np.asarray(values, dtype=np.float32),
        "ndarray_float64": lambda values: np.asarray(values, dtype=np.float64),
    }

    @pytest.mark.parametrize("form", sorted(ACCEPTED))
    def test_documented_form_converts(self, form):
        samples = self.ACCEPTED[form](_sine(SR)[:1024].tolist())
        assert math.isfinite(metering_rms_db(samples))

    @pytest.mark.parametrize("form", sorted(ACCEPTED))
    def test_documented_form_still_reports_an_empty_buffer(self, form):
        empty = range(0) if form == "range" else self.ACCEPTED[form]([])
        with pytest.raises(SonareValueError, match=r"metering_rms_db: samples must not be empty"):
            metering_rms_db(empty)

    def test_generator_is_rejected_by_the_guard_not_by_numpy(self):
        # Not a documented form, but it must still fail the way the binding
        # promises rather than leaking NumPy's own TypeError.
        with pytest.raises(SonareValueError) as exc_info:
            metering_rms_db(float(i) for i in range(1024))
        assert "metering_rms_db: samples" in str(exc_info.value)
        assert exc_info.value.code == 4

    def test_uncoercible_object_is_rejected_by_the_guard(self):
        with pytest.raises(SonareValueError) as exc_info:
            metering_rms_db(["not", "numbers"])
        assert "metering_rms_db: samples" in str(exc_info.value)
