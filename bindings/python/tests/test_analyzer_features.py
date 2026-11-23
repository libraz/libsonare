"""Feature function analyzer API tests."""

from __future__ import annotations

# ruff: noqa: F403,F405
import numpy as np

from ._analyzer_helpers import *
from ._helpers import same_value


def test_audio_analyze() -> None:
    """Audio.analyze returns a full AnalysisResult with documented silence fallbacks."""
    from libsonare import Audio, PitchClass

    audio = Audio.from_buffer([0.0] * 22050, sample_rate=22050)
    result = audio.analyze()
    assert result.bpm is None
    # Silence has no tonal content, so detect_key falls back to C major.
    assert result.key.root == PitchClass.C
    assert result.key.mode == "major"
    assert result.key.name == "C major"
    assert isinstance(result.beats, list)


def test_audio_analyze_matches_the_module_level_function_field_for_field() -> None:
    """Audio.analyze() must not be a second, narrower analysis path.

    It used to call sonare_audio_analyze directly, which fills only bpm, key,
    time signature and beat times and leaves chords / sections / timbre /
    dynamics / rhythm / melody / form at their defaults -- a Python port of JS
    reading ``audio.analyze().chords`` got ``[]``, indistinguishable from "no
    chords detected." Comparing the two results field for field (the dataclass
    is frozen, so ``==`` is structural) is what a partial result cannot fake:
    an unfixed regression would disagree on chords/sections/timbre and every
    other field the narrow C call never populated, not just on one of them.
    """
    from libsonare import Audio, analyze

    samples = _generate_sine(220.0, 22050, 3.0)
    via_handle = Audio.from_buffer(samples, sample_rate=22050).analyze()
    via_module = analyze(samples, 22050)
    assert same_value(via_handle, via_module)


def test_audio_analyze_forwards_its_keyword_options() -> None:
    """A caller-supplied option must reach the same analysis the module-level
    function runs, not a narrow call the class method used to make with no
    options parameter at all."""
    from libsonare import Audio, analyze

    samples = _generate_sine(220.0, 22050, 3.0)
    audio = Audio.from_buffer(samples, sample_rate=22050)
    narrow = audio.analyze(bpm_min=190.0, bpm_max=210.0)
    wide = audio.analyze(bpm_min=60.0, bpm_max=200.0)
    assert narrow.bpm != wide.bpm
    assert same_value(narrow, analyze(samples, 22050, bpm_min=190.0, bpm_max=210.0))


def test_analysis_primitives() -> None:
    """Detailed BPM and rhythm APIs expose reusable primitives."""
    from libsonare import (
        Audio,
        analyze_bpm,
        analyze_dynamics,
        analyze_rhythm,
        analyze_timbre,
        detect_chords,
    )

    sr = 22050
    duration = 4.0
    bpm = 120
    samples = [0.0] * int(sr * duration)
    samples_per_beat = sr * 60 / bpm
    for beat in range(int(duration * bpm / 60)):
        start = int(beat * samples_per_beat)
        for i in range(start, min(start + 200, len(samples))):
            samples[i] = 1.0

    bpm_result = analyze_bpm(samples, sample_rate=sr, max_candidates=5)
    assert bpm_result.bpm > 0
    assert bpm_result.confidence >= 0.0
    assert len(bpm_result.candidates) <= 5
    assert len(bpm_result.autocorrelation) > 0
    assert len(bpm_result.tempogram) > 0

    rhythm = analyze_rhythm(samples, sample_rate=sr)
    assert rhythm.bpm > 0
    assert rhythm.time_signature.numerator > 0
    assert rhythm.groove_type in {"straight", "shuffle", "swing"}
    assert rhythm.pattern_regularity >= 0.0
    assert rhythm.tempo_stability >= 0.0

    dynamics = analyze_dynamics(samples, sample_rate=sr)
    assert dynamics.peak_db <= 1.0
    assert len(dynamics.loudness_times_sec) == len(dynamics.loudness_rms_db)

    tone = [
        0.25
        * (
            math.sin(2 * math.pi * 261.63 * i / sr)
            + math.sin(2 * math.pi * 329.63 * i / sr)
            + math.sin(2 * math.pi * 392.00 * i / sr)
        )
        for i in range(sr * 2)
    ]
    timbre = analyze_timbre(tone, sample_rate=sr)
    assert 0.0 <= timbre.brightness <= 1.0
    assert len(timbre.spectral_centroid) > 0

    chords = detect_chords(tone, sample_rate=sr, use_beat_sync=False)
    assert isinstance(chords.chords, list)
    for chord in chords.chords:
        assert chord.root_name == str(chord.root)
        assert chord.bass_name == str(chord.root if chord.bass is None else chord.bass)

    audio = Audio.from_buffer(samples, sample_rate=sr)
    assert audio.analyze_bpm().bpm > 0
    assert audio.analyze_rhythm().bpm > 0
    assert len(audio.analyze_dynamics().loudness_times_sec) > 0
    assert len(audio.analyze_timbre().spectral_centroid) > 0
    assert isinstance(audio.detect_chords(use_beat_sync=False).chords, list)


def test_chord_functional_analysis() -> None:
    """Functional analysis returns the timed chords labelled like chord_functions."""
    from libsonare import (
        PitchClass,
        chord_functional_analysis,
        chord_functions,
        detect_chords,
    )

    sr = 22050
    tone = [
        0.25
        * (
            math.sin(2 * math.pi * 261.63 * i / sr)
            + math.sin(2 * math.pi * 329.63 * i / sr)
            + math.sin(2 * math.pi * 392.00 * i / sr)
        )
        for i in range(sr * 2)
    ]

    chords = detect_chords(tone, sample_rate=sr, use_beat_sync=False)
    romans = chord_functional_analysis(
        tone,
        key_root=PitchClass.C,
        key_mode="major",
        sample_rate=sr,
        use_beat_sync=False,
    )
    assert isinstance(romans, list)
    assert len(romans) == len(chords.chords)
    assert all(label.roman_numeral and label.function for label in romans)
    # Detection followed by chord_functions gives the same entries.
    assert romans == chord_functions(chords, (PitchClass.C, "major"))

    with pytest.raises(ValueError):
        chord_functional_analysis(
            tone,
            key_root=PitchClass.C,
            sample_rate=sr,
            chroma_method="bogus",
        )


def test_chord_threshold_emits_no_chord_for_ambiguous_input() -> None:
    """Low/high thresholds differ and rejected intervals have the N.C. name."""
    from libsonare import detect_chords

    silence = [0.0] * 4096
    common = {
        "sample_rate": 22050,
        "min_duration_sec": 0.0,
        "smoothing_window_sec": 0.01,
        "use_triads_only": True,
        "n_fft": 2048,
        "hop_length": 512,
        "use_beat_sync": False,
    }
    low = detect_chords(silence, threshold=0.0, **common)
    high = detect_chords(silence, threshold=0.99, **common)
    assert len(low.chords) == 1
    assert low.chords[0].quality != "unknown"
    assert len(high.chords) == 1
    assert high.chords[0].quality == "unknown"
    assert high.chords[0].name == "N.C."
    assert high.chords[0].start_sec == 0.0
    assert high.chords[0].end_sec > high.chords[0].start_sec


## Effects tests


def test_hpss() -> None:
    """hpss separates harmonic and percussive components."""
    from libsonare import hpss

    tone = _generate_sine(440, 22050, 1.0)
    result = hpss(tone, sample_rate=22050)
    assert len(result.harmonic) == len(tone)
    assert len(result.percussive) == len(tone)


def test_harmonic() -> None:
    """harmonic extracts harmonic component."""
    from libsonare import harmonic

    tone = _generate_sine(440, 22050, 1.0)
    result = harmonic(tone, sample_rate=22050)
    assert len(result) == len(tone)


def test_percussive() -> None:
    """percussive extracts percussive component."""
    from libsonare import percussive

    tone = _generate_sine(440, 22050, 1.0)
    result = percussive(tone, sample_rate=22050)
    assert len(result) == len(tone)


def test_time_stretch() -> None:
    """time_stretch changes audio duration."""
    from libsonare import time_stretch

    tone = _generate_sine(440, 22050, 1.0)
    fast = time_stretch(tone, sample_rate=22050, speed_ratio=2.0)
    assert len(fast) < len(tone)
    slow = time_stretch(tone, sample_rate=22050, speed_ratio=0.5)
    assert len(slow) > len(tone)

    import libsonare

    for invalid in (float("nan"), float("inf"), 1.1754943508222875e-38):
        with pytest.raises(libsonare.SonareError):
            time_stretch(tone[:4096], sample_rate=22050, speed_ratio=invalid)
    assert all(math.isfinite(value) for value in time_stretch(tone[:4096], 22050, 1.0))


def test_pitch_shift() -> None:
    """pitch_shift returns non-empty audio."""
    import libsonare
    from libsonare import pitch_shift

    tone = _generate_sine(440, 22050, 1.0)
    result = pitch_shift(tone, sample_rate=22050, pitch_semitones=2.0)
    assert len(result) > 0
    with pytest.raises(libsonare.SonareError):
        pitch_shift([0.0] * 4, sample_rate=48000, pitch_semitones=-120.0)


def test_normalize() -> None:
    """normalize boosts quiet audio toward target dB."""
    from libsonare import normalize

    quiet = [0.1 * math.sin(2 * math.pi * 440 * i / 22050) for i in range(22050)]
    result = normalize(quiet, sample_rate=22050, target_db=0.0)
    assert len(result) == len(quiet)
    peak = max(abs(v) for v in result)
    assert peak > 0.8


def test_trim() -> None:
    """trim removes leading and trailing silence."""
    from libsonare import trim

    # silence + tone + silence
    n = 22050
    samples = [0.0] * n
    start, end = n // 4, 3 * n // 4
    for i in range(start, end):
        samples[i] = 0.5 * math.sin(2 * math.pi * 440 * i / 22050)
    result = trim(samples, sample_rate=22050, threshold_db=-40.0)
    assert len(result) < len(samples)
    assert len(result) > 0


## Feature tests


def test_stft() -> None:
    """stft returns correct bin and frame dimensions."""
    from libsonare import stft

    tone = _generate_sine(440, 22050, 1.0)
    result = stft(tone, sr=22050)
    n_bins, n_frames = result.magnitude.shape
    assert n_bins == 1025
    assert n_frames > 0
    assert result.power.shape == (n_bins, n_frames)


def test_stft_db() -> None:
    """stft(db=True) carries a dB plane shaped like the magnitude."""
    from libsonare import stft

    tone = _generate_sine(440, 22050, 1.0)
    result = stft(tone, sr=22050, db=True)
    assert result.db is not None
    assert result.db.dtype == np.float32
    assert result.db.shape == result.magnitude.shape
    assert result.db.shape[0] == 1025
    assert result.db.shape[1] > 0
    # Bins well above the top_db clip follow 20*log10(magnitude).
    audible = result.magnitude > 1.0
    assert audible.any()
    expected = 20.0 * np.log10(result.magnitude[audible])
    np.testing.assert_allclose(result.db[audible], expected, atol=1e-2)


def test_stft_without_db_matches_the_db_call_on_every_other_field() -> None:
    """The absent option leaves db None and the other planes untouched."""
    from libsonare import stft

    tone = _generate_sine(440, 22050, 1.0)
    plain = stft(tone, sr=22050)
    with_db = stft(tone, sr=22050, db=True)
    assert plain.db is None
    assert (plain.n_fft, plain.hop_length, plain.sample_rate) == (
        with_db.n_fft,
        with_db.hop_length,
        with_db.sample_rate,
    )
    np.testing.assert_array_equal(plain.magnitude, with_db.magnitude)
    np.testing.assert_array_equal(plain.power, with_db.power)


def test_mel_spectrogram() -> None:
    """mel_spectrogram returns correct mel band count."""
    from libsonare import mel_spectrogram

    tone = _generate_sine(440, 22050, 1.0)
    result = mel_spectrogram(tone, sample_rate=22050)
    assert result.power.shape[0] == 128
    assert result.db.shape == result.power.shape


def test_mel_delta_returns_linear_slope() -> None:
    """mel_delta preserves the unit slope of a linear feature row."""
    import numpy as np

    from libsonare import mel_delta

    features = np.arange(10, dtype=np.float32).reshape(2, 5)
    result = mel_delta(features, width=5)
    assert result.shape == (2, 5)
    np.testing.assert_allclose(result, np.ones((2, 5)), rtol=1e-6)


def test_piptrack_returns_equally_shaped_matrices() -> None:
    """piptrack exposes the per-bin pitch and magnitude matrices."""
    from libsonare import piptrack

    result = piptrack(_generate_sine(440.0, 22050, 1.0), sr=22050, n_fft=1024, hop_length=256)
    assert result.pitches.shape[0] == 513
    assert result.pitches.shape[1] > 0
    assert result.magnitudes.shape == result.pitches.shape


def test_reassigned_spectrogram_returns_equally_shaped_coordinate_matrices() -> None:
    """reassigned_spectrogram exposes the row-major coordinate matrices."""
    from libsonare import reassigned_spectrogram

    result = reassigned_spectrogram(
        _generate_sine(440.0, 22050, 1.0), sr=22050, n_fft=1024, hop_length=256
    )
    assert result.magnitude.shape[0] == 513
    assert result.magnitude.shape[1] > 0
    assert result.times.shape == result.magnitude.shape
    assert result.frequencies.shape == result.magnitude.shape


def test_librosa_segment_operations_are_public() -> None:
    """All seven librosa.segment-compatible functions cross the Python C ABI."""
    import numpy as np

    from libsonare import (
        segment_agglomerative,
        segment_cross_similarity,
        segment_lag_to_recurrence,
        segment_path_enhance,
        segment_recurrence_matrix,
        segment_recurrence_to_lag,
        segment_subsegment,
    )

    data = np.array([[1.0, 0.0, 1.0], [0.0, 1.0, 1.0]], dtype=np.float32)
    cross = segment_cross_similarity(data, data)
    assert cross.shape == (3, 3)
    recurrence = segment_recurrence_matrix(data)
    assert recurrence.shape == (3, 3)
    lag = segment_recurrence_to_lag(recurrence)
    assert lag.shape == (6, 3)
    assert segment_lag_to_recurrence(lag).shape == (3, 3)
    # librosa 0.11.0 segment_recurrence_to_lag on rec[r][c] = 4r + c (pad defaults to True).
    padded = segment_recurrence_to_lag(np.arange(16, dtype=np.float32).reshape(4, 4))
    # fmt: off
    expected = np.array([
        [0, 5, 10, 15],
        [4, 9, 14, 0],
        [8, 13, 0, 0],
        [12, 0, 0, 0],
        [0, 0, 0, 0],
        [0, 0, 0, 3],
        [0, 0, 2, 7],
        [0, 1, 6, 11],
    ], dtype=np.float32)
    # fmt: on
    np.testing.assert_array_equal(padded, expected)
    assert segment_subsegment(data, [0, 3])[0] == 0
    assert segment_agglomerative(data, 2).shape == (3,)
    assert segment_path_enhance(recurrence, 1).shape == (3, 3)


def test_mel_spectrogram_explicit_range() -> None:
    """fmin/fmax/htk change the Mel filterbank, so the power output differs."""
    from libsonare import mel_spectrogram

    tone = _generate_sine(440, 22050, 1.0)
    base = mel_spectrogram(tone, sample_rate=22050, n_mels=40)
    ranged = mel_spectrogram(tone, sample_rate=22050, n_mels=40, fmin_hz=500.0, fmax_hz=4000.0)
    htk = mel_spectrogram(tone, sample_rate=22050, n_mels=40, htk=True)

    assert base.power.shape[0] == ranged.power.shape[0] == 40
    assert base.power.shape == ranged.power.shape == htk.power.shape
    # An explicit Mel range / HTK formula yields a measurably different filterbank.
    assert np.sum((base.power - ranged.power) ** 2) > 1e-6
    assert np.sum((base.power - htk.power) ** 2) > 1e-6


def test_mel_range_is_applied_or_refused_never_defaulted() -> None:
    """A negative or non-finite Mel bound reaches a refusal, not the default.

    The positive controls are consumed rather than merely accepted: the default
    upper edge must land on the same bands as spelling sample_rate / 2 out, and a legal
    range must move the filterbank off the default one. A refusal-only test
    passes against a reader that rejects everything, and the defect this covers
    is a *successful* call carrying a value the caller did not choose.
    """
    import math

    import pytest

    from libsonare import SonareError, mel_spectrogram

    tone = _generate_sine(440, 22050, 0.5)
    defaulted = mel_spectrogram(tone, sample_rate=22050, n_mels=40)
    nyquist = mel_spectrogram(tone, sample_rate=22050, n_mels=40, fmax_hz=11025.0)
    ranged = mel_spectrogram(tone, sample_rate=22050, n_mels=40, fmin_hz=500.0, fmax_hz=4000.0)

    np.testing.assert_allclose(defaulted.power, nyquist.power, atol=1e-6)
    assert np.sum((defaulted.power - ranged.power) ** 2) > 1e-6

    for bad in (-1.0, -11025.0, math.nan, math.inf, -math.inf):
        with pytest.raises(SonareError):
            mel_spectrogram(tone, sample_rate=22050, n_mels=40, fmin_hz=bad)
        with pytest.raises(SonareError):
            mel_spectrogram(tone, sample_rate=22050, n_mels=40, fmax_hz=bad)
    # An explicit 0 is an empty band, not a request for the Nyquist.
    with pytest.raises(SonareError):
        mel_spectrogram(tone, sample_rate=22050, n_mels=40, fmax_hz=0.0)


def test_mfcc() -> None:
    """mfcc returns correct coefficient dimensions."""
    from libsonare import mfcc

    tone = _generate_sine(440, 22050, 1.0)
    result = mfcc(tone, sr=22050, n_mels=64, n_mfcc=13)
    assert result.coefficients.shape[0] == 13
    assert result.coefficients.shape[1] > 0


def test_mfcc_explicit_range() -> None:
    """fmin/fmax change the underlying Mel range, so the coefficients differ."""
    from libsonare import mfcc

    tone = _generate_sine(440, 22050, 1.0)
    base = mfcc(tone, sr=22050, n_mels=64, n_mfcc=13)
    ranged = mfcc(tone, sr=22050, n_mels=64, n_mfcc=13, fmin=500.0, fmax=4000.0)
    assert base.coefficients.shape == ranged.coefficients.shape
    assert np.sum((base.coefficients - ranged.coefficients) ** 2) > 1e-6


def test_chroma() -> None:
    """chroma returns 12 pitch classes with mean energy."""
    from libsonare import chroma

    tone = _generate_sine(440, 22050, 1.0)
    result = chroma(tone, sample_rate=22050)
    assert result.features.shape[0] == 12
    assert result.features.shape[1] > 0
    assert result.mean_energy.shape == (12,)


@pytest.mark.slow  # bass_chroma's low-frequency CQT (~10 s); run via `make test-python-slow`
def test_chroma_cens_and_bass_chroma() -> None:
    from libsonare import bass_chroma, chroma_cens

    tone = _generate_sine(110, 22050, 1.0)
    for result in (
        chroma_cens(tone, sr=22050),
        bass_chroma(tone, sample_rate=22050),
    ):
        assert result.features.shape[0] == 12
        assert result.features.shape[1] > 0
        assert result.mean_energy.shape == (12,)


def test_spectral_centroid() -> None:
    """spectral_centroid returns non-empty list."""
    from libsonare import spectral_centroid

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_centroid(tone, sr=22050)
    assert len(result) > 0


def test_spectral_bandwidth() -> None:
    """spectral_bandwidth returns non-empty list."""
    from libsonare import spectral_bandwidth

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_bandwidth(tone, sr=22050)
    assert len(result) > 0
    assert not np.array_equal(spectral_bandwidth(tone, sr=22050, p=1.0), result)


def test_spectral_flux() -> None:
    """spectral_flux returns non-empty frames and accepts a lag."""
    from libsonare import spectral_flux

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_flux(tone, sample_rate=22050)
    assert len(result) > 0
    assert not np.array_equal(spectral_flux(tone, sample_rate=22050, lag=2), result)


def test_onset_backtrack() -> None:
    """onset_backtrack returns the preceding energy minimum."""
    from libsonare import onset_backtrack

    result = onset_backtrack([6], [1.0, 0.8, 0.5, 0.2, 0.6, 0.9, 1.2])
    assert result.tolist() == [3]


def test_griffin_lim() -> None:
    """griffin_lim reconstructs an STFT magnitude matrix."""
    from libsonare import griffin_lim, stft

    samples = _generate_sine(440, 22050, 1.0)
    spec = stft(samples, sr=22050, n_fft=1024, hop_length=256)
    result = griffin_lim(
        spec.magnitude,
        n_fft=1024,
        hop_length=256,
        n_iter=1,
        sample_rate=22050,
    )
    assert len(result) > 0


def test_griffin_lim_infers_n_fft_from_the_bin_count() -> None:
    """An omitted n_fft is (n_bins - 1) * 2; one whose bin count disagrees is refused."""
    import numpy as np
    import pytest

    from libsonare import SonareValueError, griffin_lim, stft

    samples = _generate_sine(440, 22050, 0.5)
    magnitude = stft(samples, sr=22050, n_fft=1024, hop_length=256).magnitude
    assert magnitude.shape[0] == 513
    inferred = griffin_lim(magnitude, 22050, hop_length=256, n_iter=1)
    given = griffin_lim(magnitude, 22050, n_fft=1024, hop_length=256, n_iter=1)
    np.testing.assert_array_equal(inferred, given)
    with pytest.raises(
        SonareValueError,
        match=r"^griffin_lim: n_bins must equal n_fft / 2 \+ 1$",
    ):
        griffin_lim(magnitude, 22050, n_fft=2048, hop_length=256, n_iter=1)
    with pytest.raises(
        SonareValueError,
        match=r"^griffin_lim: n_fft must be an even integer in \[4, 524288\]$",
    ):
        griffin_lim(magnitude, 22050, n_fft=1025, hop_length=256, n_iter=1)


def test_an_omitted_inverse_hop_is_a_quarter_of_n_fft() -> None:
    """A small n_fft with no hop equals the explicit n_fft // 4 call; a hop below 1 is refused."""
    import numpy as np

    from libsonare import griffin_lim, mel_to_audio, mfcc_to_audio, stft

    rng = np.random.default_rng(7)
    mel = rng.uniform(0.01, 1.0, size=(40, 16)).astype(np.float32)
    mfcc = rng.uniform(-1.0, 1.0, size=(13, 16)).astype(np.float32)
    magnitude = stft(_generate_sine(440, 22050, 0.25), sr=22050, n_fft=256, hop_length=64).magnitude
    pairs = [
        (
            lambda: mel_to_audio(mel, sr=22050, n_fft=256, n_iter=1),
            lambda: mel_to_audio(mel, sr=22050, n_fft=256, hop_length=64, n_iter=1),
        ),
        (
            lambda: mfcc_to_audio(mfcc, n_mels=40, sr=22050, n_fft=256, n_iter=1),
            lambda: mfcc_to_audio(mfcc, n_mels=40, sr=22050, n_fft=256, hop_length=64, n_iter=1),
        ),
        (
            lambda: griffin_lim(magnitude, 22050, n_iter=1),
            lambda: griffin_lim(magnitude, 22050, n_fft=256, hop_length=64, n_iter=1),
        ),
    ]
    for omitted, explicit in pairs:
        np.testing.assert_array_equal(omitted(), explicit())
    from libsonare import SonareValueError

    refusals = {
        "mel_to_audio": lambda hop: mel_to_audio(mel, sr=22050, n_fft=256, hop_length=hop),
        "mfcc_to_audio": lambda hop: mfcc_to_audio(
            mfcc, n_mels=40, sr=22050, n_fft=256, hop_length=hop
        ),
        "griffin_lim": lambda hop: griffin_lim(magnitude, 22050, hop_length=hop),
    }
    for name, call in refusals.items():
        for hop in (0, -1):
            with pytest.raises(
                SonareValueError, match=rf"^{name}: hop_length must be an integer in \[1, "
            ):
                call(hop)


def test_an_omitted_hpss_hop_is_a_quarter_of_n_fft() -> None:
    """hpss with and without a residual, n_fft 512 and no hop equal the hop-128 call."""
    from libsonare import hpss

    tone = _generate_sine(440, 22050, 0.5)
    omitted = hpss(tone, 22050, n_fft=512)
    explicit = hpss(tone, 22050, n_fft=512, hop_length=128)
    np.testing.assert_array_equal(omitted.harmonic, explicit.harmonic)
    np.testing.assert_array_equal(omitted.percussive, explicit.percussive)
    with_residual = hpss(tone, 22050, n_fft=512, with_residual=True)
    with_residual_explicit = hpss(tone, 22050, n_fft=512, hop_length=128, with_residual=True)
    for key in ("harmonic", "percussive", "residual"):
        np.testing.assert_array_equal(
            getattr(with_residual, key), getattr(with_residual_explicit, key)
        )


def test_an_omitted_time_stretch_hop_is_a_quarter_of_n_fft() -> None:
    """time_stretch with n_fft 512 and no hop equals the hop-128 call."""
    from libsonare import time_stretch

    tone = _generate_sine(440, 22050, 0.5)
    np.testing.assert_array_equal(
        time_stretch(tone, 22050, 1.25, n_fft=512),
        time_stretch(tone, 22050, 1.25, n_fft=512, hop_length=128),
    )


def test_an_omitted_pitch_shift_hop_is_a_quarter_of_n_fft() -> None:
    """pitch_shift with n_fft 512 and no hop equals the hop-128 call."""
    from libsonare import pitch_shift

    tone = _generate_sine(440, 22050, 0.5)
    np.testing.assert_array_equal(
        pitch_shift(tone, 22050, 2.0, n_fft=512),
        pitch_shift(tone, 22050, 2.0, n_fft=512, hop_length=128),
    )


def test_an_omitted_phase_vocoder_hop_is_a_quarter_of_n_fft() -> None:
    """phase_vocoder with n_fft 512 and no hop equals the hop-128 call."""
    from libsonare import phase_vocoder

    tone = _generate_sine(440, 22050, 0.5)
    np.testing.assert_array_equal(
        phase_vocoder(tone, 22050, 1.25, n_fft=512),
        phase_vocoder(tone, 22050, 1.25, n_fft=512, hop_length=128),
    )


@pytest.mark.parametrize("hop_length", [0, -1])
def test_phase_vocoder_refuses_a_given_hop_below_one(hop_length: int) -> None:
    from libsonare import SonareValueError, phase_vocoder

    with pytest.raises(SonareValueError) as caught:
        phase_vocoder(_generate_sine(440, 22050, 0.25), 22050, 1.25, hop_length=hop_length)
    assert str(caught.value) == "phase_vocoder: hop_length must be an integer in [1, 2147483647]"


@pytest.mark.parametrize("n_fft", [3, 2])
def test_phase_vocoder_refuses_an_n_fft_outside_the_stft_domain(n_fft: int) -> None:
    # The same STFT-size refusal time_stretch and pitch_shift give, before the core.
    from libsonare import SonareValueError, phase_vocoder

    with pytest.raises(SonareValueError) as caught:
        phase_vocoder(_generate_sine(440, 22050, 0.25), 22050, 1.25, n_fft=n_fft)
    assert str(caught.value) == "phase_vocoder: n_fft must be an even integer in [4, 524288]"


def test_spectral_rolloff() -> None:
    """spectral_rolloff returns non-empty list."""
    from libsonare import spectral_rolloff

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_rolloff(tone, sr=22050)
    assert len(result) > 0


def test_spectral_flatness() -> None:
    """spectral_flatness returns non-empty list."""
    from libsonare import spectral_flatness

    tone = _generate_sine(440, 22050, 1.0)
    result = spectral_flatness(tone, sr=22050)
    assert len(result) > 0


def test_zero_crossing_rate() -> None:
    """zero_crossing_rate returns values in [0, 1]."""
    from libsonare import zero_crossing_rate

    tone = _generate_sine(440, 22050, 1.0)
    result = zero_crossing_rate(tone, sr=22050)
    assert len(result) > 0
    assert np.all((result >= 0) & (result <= 1))


def test_zero_crossing_rate_uses_edge_padding() -> None:
    """zero_crossing_rate matches librosa-style edge padding for constant signals."""
    from libsonare import zero_crossing_rate

    result = zero_crossing_rate([-1.0] * 8, sr=22050, frame_length=8, hop_length=4)
    assert len(result) > 0
    assert np.all(result == 0.0)


def test_rms_energy() -> None:
    """rms_energy returns non-negative values."""
    from libsonare import rms_energy

    tone = _generate_sine(440, 22050, 1.0)
    result = rms_energy(tone, sample_rate=22050)
    assert len(result) > 0
    assert np.all(result >= 0)


def test_spectral_scalars_repeated_calls_are_stable() -> None:
    """Repeated calls to the spectral/scalar helpers stay correct.

    Regression guard for the C-buffer free path: each helper now frees its
    ``sonare_free_floats`` output inside a ``try/finally``. Calling them many
    times must keep returning identical, well-formed results (a smoke test for
    the buffer lifecycle; it does not directly measure native heap growth).
    """
    from libsonare import (
        rms_energy,
        spectral_bandwidth,
        spectral_centroid,
        spectral_flatness,
        spectral_rolloff,
        zero_crossing_rate,
    )

    tone = _generate_sine(440, 22050, 1.0)
    helpers = (
        spectral_centroid,
        spectral_bandwidth,
        spectral_rolloff,
        spectral_flatness,
        zero_crossing_rate,
        rms_energy,
    )
    for helper in helpers:
        first = helper(tone, 22050)
        assert len(first) > 0
        for _ in range(64):
            again = helper(tone, 22050)
            np.testing.assert_array_equal(again, first)


def test_pitch_yin() -> None:
    """pitch_yin detects 440 Hz tone."""
    from libsonare import pitch_yin

    tone = _generate_sine(440, 22050, 1.0)
    result = pitch_yin(tone, sample_rate=22050)
    assert len(result.f0) > 0
    assert 400 < result.median_f0 < 480


def test_pitch_pyin() -> None:
    """pitch_pyin detects 440 Hz tone."""
    from libsonare import pitch_pyin

    tone = _generate_sine(440, 22050, 1.0)
    result = pitch_pyin(tone, sample_rate=22050)
    assert len(result.f0) > 0
    assert 400 < result.median_f0 < 480


## Conversion tests


def test_hz_to_mel() -> None:
    """hz_to_mel returns positive value for 440 Hz."""
    from libsonare import hz_to_mel

    assert hz_to_mel(440.0) > 0


def test_mel_to_hz() -> None:
    """mel_to_hz round-trips with hz_to_mel."""
    from libsonare import hz_to_mel, mel_to_hz

    mel = hz_to_mel(440.0)
    hz = mel_to_hz(mel)
    assert abs(hz - 440.0) < 1.0


def test_hz_to_midi() -> None:
    """hz_to_midi maps A4 (440 Hz) to MIDI 69."""
    from libsonare import hz_to_midi

    assert abs(hz_to_midi(440.0) - 69.0) < 0.01


def test_midi_to_hz() -> None:
    """midi_to_hz maps MIDI 69 to 440 Hz."""
    from libsonare import midi_to_hz

    assert abs(midi_to_hz(69.0) - 440.0) < 0.01


def test_hz_to_note() -> None:
    """hz_to_note maps 440 Hz to A4."""
    from libsonare import hz_to_note

    assert hz_to_note(440.0) == "A4"


def test_note_to_hz() -> None:
    """note_to_hz maps A4 to ~440 Hz."""
    from libsonare import note_to_hz

    assert abs(note_to_hz("A4") - 440.0) < 1.0


@pytest.mark.parametrize("name", ["H4", "C#x", "", "A4junk", "C2147483647", "C10", "A-2"])
def test_note_to_hz_refuses_an_unparsable_name(name: str) -> None:
    """note_to_hz raises the coded InvalidParameter error instead of answering 0."""
    from libsonare import ErrorCode, SonareValueError, note_to_hz

    with pytest.raises(SonareValueError) as caught:
        note_to_hz(name)
    assert caught.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_note_to_hz_spans_the_midi_note_octaves() -> None:
    """The octaves of C-1 (MIDI 0) through G9 (MIDI 127) are answered."""
    from libsonare import note_to_hz

    assert abs(note_to_hz("C-1") - 8.1758) < 1e-3
    assert abs(note_to_hz("G9") - 12543.85) < 0.1


@pytest.mark.parametrize("hz", [0.0, -440.0, float("nan"), float("inf")])
def test_hz_to_note_refuses_a_frequency_with_no_note(hz: float) -> None:
    """hz_to_note raises the coded InvalidParameter error instead of answering '?'."""
    from libsonare import ErrorCode, SonareValueError, hz_to_note

    with pytest.raises(SonareValueError) as caught:
        hz_to_note(hz)
    assert caught.value.code == int(ErrorCode.INVALID_PARAMETER)


def test_frames_to_time() -> None:
    """frames_to_time converts frame index to seconds."""
    from libsonare import frames_to_time

    t = frames_to_time(1, 22050, 512)
    assert abs(t - 512 / 22050) < 1e-5


def test_time_to_frames() -> None:
    """time_to_frames converts seconds to frame index."""
    from libsonare import time_to_frames

    f = time_to_frames(1.0, 22050, 512)
    assert f == 22050 // 512


@pytest.mark.parametrize("bad", [{"sr": 0}, {"hop_length": 0}, {"sr": -1}])
def test_frame_time_conversions_refuse_a_non_positive_rate_or_hop(bad) -> None:
    from libsonare import SonareValueError, frames_to_time, time_to_frames

    with pytest.raises(SonareValueError):
        frames_to_time(1, **bad)
    with pytest.raises(SonareValueError):
        time_to_frames(1.0, **bad)
    assert time_to_frames(0.0, 22050, 512) == 0


## Resample test
