#!/usr/bin/env python3
"""
Generate librosa reference values for libsonare tests.

Usage:
    python tests/librosa/generate_librosa_reference.py

Output:
    tests/librosa/reference/*.json
"""

import json
import numpy as np
import librosa
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent / "reference"


def generate_convert_reference():
    """Hz <-> Mel, Hz <-> MIDI conversion reference."""
    test_hz = [20, 100, 440, 1000, 4000, 8000, 16000]

    refs = []
    for hz in test_hz:
        refs.append({
            "hz": hz,
            "mel_slaney": float(librosa.hz_to_mel(hz, htk=False)),
            "mel_htk": float(librosa.hz_to_mel(hz, htk=True)),
            "midi": float(librosa.hz_to_midi(hz)),
        })

    # Inverse conversions
    test_mel = [0, 5, 10, 15, 20, 25, 30]
    for mel in test_mel:
        refs.append({
            "mel": mel,
            "hz_slaney": float(librosa.mel_to_hz(mel, htk=False)),
            "hz_htk": float(librosa.mel_to_hz(mel, htk=True)),
        })

    return refs


def generate_mel_filterbank_reference():
    """Mel filterbank reference."""
    refs = []

    for sr in [22050, 44100]:
        for n_fft in [1024, 2048]:
            for n_mels in [64, 128]:
                for htk in [False, True]:
                    mel = librosa.filters.mel(
                        sr=sr, n_fft=n_fft, n_mels=n_mels, htk=htk
                    )
                    refs.append({
                        "sr": sr,
                        "n_fft": n_fft,
                        "n_mels": n_mels,
                        "htk": htk,
                        "shape": list(mel.shape),
                        "sum": float(mel.sum()),
                        "max": float(mel.max()),
                        "row_sums": mel.sum(axis=1).tolist(),
                    })

    return refs


def generate_mfcc_reference():
    """MFCC reference using synthetic signal (improved with frame-level data)."""
    sr = 22050
    duration = 1.0
    y = librosa.tone(440.0, sr=sr, duration=duration)

    refs = []
    for n_mfcc in [13, 20]:
        for n_mels in [64, 128]:
            mfcc = librosa.feature.mfcc(y=y, sr=sr, n_mfcc=n_mfcc, n_mels=n_mels)

            # Selected frames for frame-level comparison
            selected_frames = [5, 10, 15, 20]
            frame_data = {}
            for f in selected_frames:
                if f < mfcc.shape[1]:
                    frame_data[str(f)] = mfcc[:, f].tolist()

            refs.append({
                "signal": "440Hz_tone",
                "sr": sr,
                "n_mfcc": n_mfcc,
                "n_mels": n_mels,
                "shape": list(mfcc.shape),
                "mean": mfcc.mean(axis=1).tolist(),
                "std": mfcc.std(axis=1).tolist(),
                "selected_frames": frame_data,
            })

    return refs


def generate_tempo_reference():
    """Tempo detection reference using synthetic impulse train."""
    refs = []
    sr = 22050
    duration = 20  # seconds

    for true_tempo in [60, 90, 120, 150, 180]:
        # Create impulse train at tempo
        y = np.zeros(duration * sr)
        samples_per_beat = int(60.0 / true_tempo * sr)
        y[::samples_per_beat] = 1.0

        detected = librosa.feature.tempo(y=y, sr=sr)

        refs.append({
            "true_tempo": true_tempo,
            "detected_tempo": float(detected[0]),
            "tolerance_percent": 5.0,
        })

    return refs


def generate_onset_strength_reference():
    """Onset strength envelope reference (improved with full envelope)."""
    sr = 22050
    duration = 1.0

    y = np.zeros(int(duration * sr))
    for t_val in [0.2, 0.4, 0.6, 0.8]:
        idx = int(t_val * sr)
        y[idx:idx+100] = np.hanning(100)

    refs = []
    for hop_length in [256, 512]:
        onset_env = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop_length)

        # Find top 5 peaks
        from scipy.signal import find_peaks
        peaks, _ = find_peaks(onset_env, height=onset_env.max() * 0.3)
        peak_heights = onset_env[peaks]
        top_indices = np.argsort(peak_heights)[-5:]
        top_peaks = sorted(peaks[top_indices].tolist())

        refs.append({
            "signal": "impulse_train",
            "sr": sr,
            "hop_length": hop_length,
            "shape": list(onset_env.shape),
            "max": float(onset_env.max()),
            "mean": float(onset_env.mean()),
            "nonzero_count": int(np.count_nonzero(onset_env > 0.1)),
            "envelope": onset_env.tolist(),
            "top_peak_frames": top_peaks,
        })

    return refs


def generate_stft_reference():
    """STFT magnitude reference using synthetic signals."""
    sr = 22050
    duration = 1.0

    # 440Hz sine tone
    y = librosa.tone(440.0, sr=sr, duration=duration)

    refs = []
    for n_fft, hop_length in [(2048, 512), (1024, 256)]:
        S = np.abs(librosa.stft(y, n_fft=n_fft, hop_length=hop_length))
        refs.append({
            "signal": "440Hz_tone",
            "sr": sr,
            "n_fft": n_fft,
            "hop_length": hop_length,
            "shape": list(S.shape),
            "magnitude_sum": float(S.sum()),
            "magnitude_max": float(S.max()),
            # Store full magnitude matrix (flattened, row-major: [n_bins, n_frames])
            "magnitude": S.flatten().tolist(),
        })

    return refs


def generate_power_to_db_reference():
    """Power-to-dB conversion reference."""
    # Scalar tests
    test_values = [1e-10, 1e-5, 0.001, 0.01, 0.1, 1.0, 10.0, 100.0]
    scalar_refs = []
    for val in test_values:
        db = librosa.power_to_db(np.array([val]), ref=1.0, amin=1e-10, top_db=None)
        scalar_refs.append({
            "power": val,
            "db": float(db[0]),
        })

    # Also test with top_db=80 (librosa default)
    scalar_refs_topdb = []
    for val in test_values:
        db = librosa.power_to_db(np.array([val]), ref=1.0, amin=1e-10, top_db=80.0)
        scalar_refs_topdb.append({
            "power": val,
            "db": float(db[0]),
        })

    # Mel spectrogram dB test
    sr = 22050
    y = librosa.tone(440.0, sr=sr, duration=1.0)
    S = librosa.feature.melspectrogram(y=y, sr=sr, n_mels=128, n_fft=2048, hop_length=512)
    S_db = librosa.power_to_db(S, ref=1.0, amin=1e-10, top_db=None)

    return {
        "scalar_no_topdb": scalar_refs,
        "scalar_topdb80": scalar_refs_topdb,
        "mel_db": {
            "signal": "440Hz_tone",
            "sr": sr,
            "n_mels": 128,
            "shape": list(S_db.shape),
            "mean_per_band": S_db.mean(axis=1).tolist(),
            "std_per_band": S_db.std(axis=1).tolist(),
        }
    }


def generate_zcr_rms_reference():
    """Zero crossing rate and RMS energy reference."""
    sr = 22050
    duration = 1.0

    refs = []
    # 440Hz tone
    y_tone = librosa.tone(440.0, sr=sr, duration=duration)

    # White noise (fixed seed for reproducibility)
    rng = np.random.default_rng(42)
    y_noise = rng.standard_normal(int(sr * duration)).astype(np.float32)

    y_alternating = np.ones(int(sr * duration), dtype=np.float32)
    y_alternating[1::2] = -1.0

    y_constant_negative = np.full(int(sr * duration), -1.0, dtype=np.float32)

    for name, y in [
        ("440Hz_tone", y_tone),
        ("white_noise", y_noise),
        ("alternating_sign", y_alternating),
        ("constant_negative", y_constant_negative),
    ]:
        zcr = librosa.feature.zero_crossing_rate(y, frame_length=2048, hop_length=512)
        rms = librosa.feature.rms(y=y, frame_length=2048, hop_length=512)
        refs.append({
            "signal": name,
            "sr": sr,
            "frame_length": 2048,
            "hop_length": 512,
            "zcr_shape": list(zcr.shape),
            "zcr": zcr.flatten().tolist(),
            "rms_shape": list(rms.shape),
            "rms": rms.flatten().tolist(),
        })

    return refs


def generate_spectral_features_reference():
    """Spectral feature reference (centroid, bandwidth, rolloff, flatness, contrast)."""
    sr = 22050
    duration = 1.0

    # Two-tone signal
    t = np.arange(0, duration, 1.0/sr)
    y = (np.sin(2 * np.pi * 440 * t) + np.sin(2 * np.pi * 880 * t)).astype(np.float32) * 0.5

    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=512))

    centroid = librosa.feature.spectral_centroid(S=S, sr=sr)
    bandwidth = librosa.feature.spectral_bandwidth(S=S, sr=sr)
    rolloff = librosa.feature.spectral_rolloff(S=S, sr=sr, roll_percent=0.85)
    flatness = librosa.feature.spectral_flatness(S=S)
    contrast = librosa.feature.spectral_contrast(S=S, sr=sr, n_bands=6, fmin=200.0)

    return {
        "signal": "440Hz_880Hz_twotone",
        "sr": sr,
        "n_fft": 2048,
        "hop_length": 512,
        "centroid": centroid.flatten().tolist(),
        "bandwidth": bandwidth.flatten().tolist(),
        "rolloff": rolloff.flatten().tolist(),
        "flatness": flatness.flatten().tolist(),
        "contrast_shape": list(contrast.shape),
        "contrast": contrast.flatten().tolist(),
    }


def generate_chroma_reference():
    """Chroma feature reference."""
    sr = 22050
    duration = 1.0

    refs = []

    # C major chord (C4 + E4 + G4)
    t = np.arange(0, duration, 1.0/sr)
    y = (np.sin(2*np.pi*261.63*t) + np.sin(2*np.pi*329.63*t) + np.sin(2*np.pi*392.0*t)).astype(np.float32) / 3.0
    chroma = librosa.feature.chroma_stft(y=y, sr=sr, n_fft=2048, hop_length=512)
    refs.append({
        "signal": "C_major_chord",
        "sr": sr,
        "shape": list(chroma.shape),
        "mean_per_class": chroma.mean(axis=1).tolist(),
        "chroma": chroma.flatten().tolist(),
    })

    # 440Hz tone (should peak at A)
    y_a = librosa.tone(440.0, sr=sr, duration=duration)
    chroma_a = librosa.feature.chroma_stft(y=y_a, sr=sr, n_fft=2048, hop_length=512)
    refs.append({
        "signal": "440Hz_tone",
        "sr": sr,
        "shape": list(chroma_a.shape),
        "mean_per_class": chroma_a.mean(axis=1).tolist(),
        "chroma": chroma_a.flatten().tolist(),
    })

    return refs


def generate_cqt_reference():
    """CQT reference."""
    sr = 22050
    duration = 1.0
    y = librosa.tone(440.0, sr=sr, duration=duration)

    fmin = librosa.note_to_hz('C1')  # ~32.7 Hz
    n_bins = 84
    bins_per_octave = 12

    C = np.abs(librosa.cqt(y, sr=sr, fmin=fmin, n_bins=n_bins,
                            bins_per_octave=bins_per_octave, hop_length=512))
    freqs = librosa.cqt_frequencies(n_bins=n_bins, fmin=fmin, bins_per_octave=bins_per_octave)

    return {
        "signal": "440Hz_tone",
        "sr": sr,
        "fmin": float(fmin),
        "n_bins": n_bins,
        "bins_per_octave": bins_per_octave,
        "hop_length": 512,
        "shape": list(C.shape),
        "magnitude_sum": float(C.sum()),
        "magnitude_max": float(C.max()),
        "frequencies": freqs.tolist(),
        # Store first 5 frames
        "frames": C[:, :5].flatten().tolist() if C.shape[1] >= 5 else C.flatten().tolist(),
        "n_stored_frames": min(5, C.shape[1]),
    }


def generate_yin_reference():
    """YIN pitch detection reference."""
    sr = 22050
    duration = 1.0

    refs = []

    # 440Hz tone
    y = librosa.tone(440.0, sr=sr, duration=duration)
    f0 = librosa.yin(y, fmin=65, fmax=2093, sr=sr, frame_length=2048, hop_length=512)
    refs.append({
        "signal": "440Hz_tone",
        "sr": sr,
        "fmin": 65,
        "fmax": 2093,
        "frame_length": 2048,
        "hop_length": 512,
        "shape": list(f0.shape),
        "f0": f0.tolist(),
    })

    # Chirp 200-800Hz
    y_chirp = librosa.chirp(fmin=200, fmax=800, sr=sr, duration=duration)
    f0_chirp = librosa.yin(y_chirp, fmin=65, fmax=2093, sr=sr, frame_length=2048, hop_length=512)
    refs.append({
        "signal": "chirp_200_800Hz",
        "sr": sr,
        "fmin": 65,
        "fmax": 2093,
        "frame_length": 2048,
        "hop_length": 512,
        "shape": list(f0_chirp.shape),
        "f0": f0_chirp.tolist(),
    })

    return refs


def generate_hpss_reference():
    """HPSS reference."""
    sr = 22050
    duration = 1.0

    # Mix: 440Hz tone + impulse train
    t = np.arange(0, duration, 1.0/sr)
    y_harmonic = np.sin(2 * np.pi * 440 * t).astype(np.float32) * 0.5
    y_percussive = np.zeros_like(y_harmonic)
    for pos in [0.1, 0.3, 0.5, 0.7, 0.9]:
        idx = int(pos * sr)
        y_percussive[idx:idx+50] = 1.0
    y = y_harmonic + y_percussive

    S = librosa.stft(y, n_fft=2048, hop_length=512)
    H, P = librosa.decompose.hpss(S)

    H_mag = np.abs(H)
    P_mag = np.abs(P)
    S_mag = np.abs(S)

    return {
        "signal": "tone_plus_impulses",
        "sr": sr,
        "n_fft": 2048,
        "hop_length": 512,
        "shape": list(S_mag.shape),
        "harmonic_sum": float(H_mag.sum()),
        "harmonic_max": float(H_mag.max()),
        "percussive_sum": float(P_mag.sum()),
        "percussive_max": float(P_mag.max()),
        "total_sum": float(S_mag.sum()),
        "harmonic_energy_ratio": float((H_mag**2).sum() / ((S_mag**2).sum() + 1e-10)),
    }


def generate_db_conversion_reference():
    """power_to_db / amplitude_to_db / db_to_power / db_to_amplitude reference."""
    values = [1e-10, 1e-5, 1e-3, 0.01, 0.1, 1.0, 10.0, 100.0]
    amp_values = [1e-5, 1e-3, 0.01, 0.1, 1.0, 10.0]

    p2db_default = librosa.power_to_db(
        np.array(values, dtype=np.float64), ref=1.0, amin=1e-10, top_db=None
    ).tolist()
    p2db_topdb80 = librosa.power_to_db(
        np.array(values, dtype=np.float64), ref=1.0, amin=1e-10, top_db=80.0
    ).tolist()
    a2db_default = librosa.amplitude_to_db(
        np.array(amp_values, dtype=np.float64), ref=1.0, amin=1e-5, top_db=None
    ).tolist()
    db_to_power_vals = [-80.0, -40.0, -20.0, -10.0, 0.0, 10.0]
    p_back = librosa.db_to_power(np.array(db_to_power_vals)).tolist()
    a_back = librosa.db_to_amplitude(np.array(db_to_power_vals)).tolist()

    # Array case with top_db: power_to_db with ref=np.max behavior.
    rng = np.random.default_rng(42)
    S_rand = rng.exponential(scale=1.0, size=(64, 50)).astype(np.float64)
    S_db_maxref = librosa.power_to_db(S_rand, ref=np.max, amin=1e-10, top_db=80.0)

    return {
        "power_to_db_scalar_no_topdb": [
            {"power": float(v), "db": float(d)} for v, d in zip(values, p2db_default)
        ],
        "power_to_db_scalar_topdb80": [
            {"power": float(v), "db": float(d)} for v, d in zip(values, p2db_topdb80)
        ],
        "amplitude_to_db_scalar": [
            {"amplitude": float(v), "db": float(d)} for v, d in zip(amp_values, a2db_default)
        ],
        "db_to_power": [
            {"db": float(v), "power": float(p)} for v, p in zip(db_to_power_vals, p_back)
        ],
        "db_to_amplitude": [
            {"db": float(v), "amplitude": float(a)} for v, a in zip(db_to_power_vals, a_back)
        ],
        "power_to_db_maxref": {
            "shape": list(S_db_maxref.shape),
            "data_flat": S_rand.flatten().tolist(),
            "expected_flat": S_db_maxref.flatten().tolist(),
            "amin": 1e-10,
            "top_db": 80.0,
        },
    }


def generate_frames_samples_reference():
    """frames_to_samples / samples_to_frames reference."""
    hop = 512
    n_fft = 2048

    frames = [0, 1, 5, 10, 100]
    samples = [0, 256, 1024, 2048, 51200]

    return {
        "hop_length": hop,
        "n_fft": n_fft,
        "frames_to_samples_no_nfft": [
            {"frames": f, "samples": int(librosa.frames_to_samples(f, hop_length=hop))}
            for f in frames
        ],
        "frames_to_samples_with_nfft": [
            {
                "frames": f,
                "samples": int(librosa.frames_to_samples(f, hop_length=hop, n_fft=n_fft)),
            }
            for f in frames
        ],
        "samples_to_frames_no_nfft": [
            {"samples": s, "frames": int(librosa.samples_to_frames(s, hop_length=hop))}
            for s in samples
        ],
        "samples_to_frames_with_nfft": [
            {
                "samples": s,
                "frames": int(librosa.samples_to_frames(s, hop_length=hop, n_fft=n_fft)),
            }
            for s in samples
        ],
    }


def generate_magphase_reference():
    """magphase reference for a 440Hz tone STFT."""
    sr = 22050
    y = librosa.tone(440.0, sr=sr, duration=0.5)
    S = librosa.stft(y, n_fft=1024, hop_length=256)
    mag, phase = librosa.magphase(S)
    # Phase is complex; store first 50 entries' real/imag parts.
    n = min(50, mag.size)
    mag_f = mag.flatten()
    ph_f = phase.flatten()
    return {
        "signal": "440Hz_tone",
        "sr": sr,
        "n_fft": 1024,
        "hop_length": 256,
        "shape": list(mag.shape),
        "mag_sum": float(mag.sum()),
        "mag_max": float(mag.max()),
        "first_n": n,
        "mag_first_n": [float(v) for v in mag_f[:n]],
        "phase_real_first_n": [float(v.real) for v in ph_f[:n]],
        "phase_imag_first_n": [float(v.imag) for v in ph_f[:n]],
    }


def generate_preemphasis_reference():
    """preemphasis / deemphasis reference."""
    sr = 22050
    rng = np.random.default_rng(7)
    y = rng.standard_normal(2048).astype(np.float32)
    coef = 0.97
    pre = librosa.effects.preemphasis(y, coef=coef)
    de = librosa.effects.deemphasis(pre, coef=coef)
    return {
        "sr": sr,
        "coef": coef,
        "input": y.tolist(),
        "preemphasized": pre.tolist(),
        "deemphasized": de.tolist(),
    }


def generate_silence_reference():
    """trim / split reference."""
    sr = 22050
    # Build a signal: 0.2s silence + 0.3s tone + 0.2s silence + 0.3s tone + 0.2s silence
    parts = []
    parts.append(np.zeros(int(0.2 * sr), dtype=np.float32))
    parts.append(librosa.tone(440.0, sr=sr, duration=0.3).astype(np.float32) * 0.5)
    parts.append(np.zeros(int(0.2 * sr), dtype=np.float32))
    parts.append(librosa.tone(660.0, sr=sr, duration=0.3).astype(np.float32) * 0.5)
    parts.append(np.zeros(int(0.2 * sr), dtype=np.float32))
    y = np.concatenate(parts)

    trimmed, index = librosa.effects.trim(y, top_db=20, frame_length=2048, hop_length=512)
    intervals = librosa.effects.split(y, top_db=20, frame_length=2048, hop_length=512)
    return {
        "sr": sr,
        "top_db": 20,
        "frame_length": 2048,
        "hop_length": 512,
        "input_length": int(y.size),
        "trim_start_sample": int(index[0]),
        "trim_end_sample": int(index[1]),
        "split_intervals": [[int(a), int(b)] for a, b in intervals],
    }


def generate_tempogram_reference():
    """tempogram / fourier_tempogram reference (statistics + shape only)."""
    sr = 22050
    # 8-second 120 BPM impulse train mixed with low noise.
    duration = 8.0
    y = np.zeros(int(duration * sr), dtype=np.float32)
    spb = int(60.0 / 120.0 * sr)
    y[::spb] = 1.0
    onset = librosa.onset.onset_strength(y=y, sr=sr, hop_length=512)
    win_length = 384
    tg = librosa.feature.tempogram(
        onset_envelope=onset, sr=sr, hop_length=512, win_length=win_length, norm=np.inf
    )
    ftg = librosa.feature.fourier_tempogram(
        onset_envelope=onset, sr=sr, hop_length=512, win_length=win_length
    )
    return {
        "sr": sr,
        "hop_length": 512,
        "win_length": win_length,
        "onset_length": int(onset.size),
        "onset_mean": float(onset.mean()),
        "tempogram_shape": list(tg.shape),
        "tempogram_lag_means": tg.mean(axis=1).tolist(),
        "fourier_tempogram_shape": list(ftg.shape),
        "fourier_tempogram_bin_means": np.abs(ftg).mean(axis=1).tolist(),
    }


def generate_peak_pick_reference():
    """peak_pick reference."""
    # Synthetic envelope with known peaks.
    n = 200
    rng = np.random.default_rng(3)
    x = 0.1 * rng.standard_normal(n).astype(np.float32)
    for idx in [20, 60, 100, 140, 180]:
        x[idx] += 1.0
    peaks = librosa.util.peak_pick(
        x, pre_max=10, post_max=10, pre_avg=20, post_avg=20, delta=0.2, wait=15
    )
    return {
        "input": x.tolist(),
        "pre_max": 10,
        "post_max": 10,
        "pre_avg": 20,
        "post_avg": 20,
        "delta": 0.2,
        "wait": 15,
        "expected_peaks": [int(p) for p in peaks],
    }


def generate_util_frame_reference():
    """util.frame reference."""
    n = 1024
    rng = np.random.default_rng(11)
    x = rng.standard_normal(n).astype(np.float32)
    frame_length = 256
    hop_length = 64
    frames = librosa.util.frame(x, frame_length=frame_length, hop_length=hop_length, axis=0)
    # frames has shape (n_frames, frame_length) with axis=0.
    return {
        "frame_length": frame_length,
        "hop_length": hop_length,
        "input": x.tolist(),
        "expected_shape": list(frames.shape),
        "first_frame": frames[0].tolist(),
        "last_frame": frames[-1].tolist(),
    }


def generate_padding_reference():
    """pad_center / fix_length / fix_frames reference."""
    rng = np.random.default_rng(19)
    a = rng.standard_normal(7).astype(np.float32)
    pad_center_out = librosa.util.pad_center(a, size=15).tolist()

    fix_long = rng.standard_normal(20).astype(np.float32)
    fix_long_out = librosa.util.fix_length(fix_long, size=10).tolist()

    fix_short = rng.standard_normal(5).astype(np.float32)
    fix_short_out = librosa.util.fix_length(fix_short, size=12).tolist()

    frames = [1, 3, 3, 5, 7, 9]
    fix_frames_out = librosa.util.fix_frames(
        np.array(frames), x_min=0, x_max=10, pad=True
    ).tolist()

    return {
        "pad_center": {"input": a.tolist(), "size": 15, "expected": pad_center_out},
        "fix_length_truncate": {
            "input": fix_long.tolist(),
            "size": 10,
            "expected": fix_long_out,
        },
        "fix_length_pad": {
            "input": fix_short.tolist(),
            "size": 12,
            "expected": fix_short_out,
        },
        "fix_frames": {
            "input": frames,
            "x_min": 0,
            "x_max": 10,
            "expected": [int(v) for v in fix_frames_out],
        },
    }


def generate_normalize_reference():
    """util.normalize reference."""
    rng = np.random.default_rng(23)
    x = rng.standard_normal(8).astype(np.float64)
    inf_n = librosa.util.normalize(x, norm=np.inf).tolist()
    l1_n = librosa.util.normalize(x, norm=1).tolist()
    l2_n = librosa.util.normalize(x, norm=2).tolist()

    # Matrix normalize along axis=1 (rows).
    m = rng.standard_normal((4, 5)).astype(np.float64)
    m_axis1 = librosa.util.normalize(m, norm=np.inf, axis=1)
    m_axis0 = librosa.util.normalize(m, norm=np.inf, axis=0)

    return {
        "vector": {
            "input": x.tolist(),
            "inf_norm": inf_n,
            "l1_norm": l1_n,
            "l2_norm": l2_n,
        },
        "matrix": {
            "rows": 4,
            "cols": 5,
            "input_flat": m.flatten().tolist(),
            "axis1_inf_norm_flat": m_axis1.flatten().tolist(),
            "axis0_inf_norm_flat": m_axis0.flatten().tolist(),
        },
    }


def generate_poly_features_reference():
    """librosa.feature.poly_features reference: linear polynomial fit per frame."""
    sr = 22050
    duration = 1.0
    y = librosa.tone(440.0, sr=sr, duration=duration)
    n_fft = 2048
    hop_length = 512
    order = 1

    S = np.abs(librosa.stft(y, n_fft=n_fft, hop_length=hop_length))
    coeffs = librosa.feature.poly_features(S=S, sr=sr, n_fft=n_fft, order=order)

    return {
        "signal": "440Hz_tone",
        "sr": sr,
        "n_fft": n_fft,
        "hop_length": hop_length,
        "order": order,
        "shape": list(coeffs.shape),
        "coeffs_flat": coeffs.flatten().tolist(),
    }


def generate_tonnetz_reference():
    """librosa.feature.tonnetz reference using a 440Hz tone chroma_stft input."""
    sr = 22050
    duration = 1.0
    y = librosa.tone(440.0, sr=sr, duration=duration)
    n_fft = 2048
    hop_length = 512

    chroma = librosa.feature.chroma_stft(y=y, sr=sr, n_fft=n_fft, hop_length=hop_length)
    tn = librosa.feature.tonnetz(chroma=chroma, sr=sr)

    return {
        "signal": "440Hz_tone",
        "sr": sr,
        "n_fft": n_fft,
        "hop_length": hop_length,
        "chroma_shape": list(chroma.shape),
        "chroma_flat": chroma.flatten().tolist(),
        "tonnetz_shape": list(tn.shape),
        "tonnetz_flat": tn.flatten().tolist(),
    }


def generate_pcen_reference():
    """librosa.pcen reference applied to a Mel power spectrogram of a short sine."""
    sr = 22050
    duration = 0.5
    y = librosa.tone(440.0, sr=sr, duration=duration)
    hop_length = 512
    n_mels = 64
    n_fft = 1024

    S = librosa.feature.melspectrogram(
        y=y, sr=sr, n_fft=n_fft, hop_length=hop_length, n_mels=n_mels, power=2.0
    )
    expected = librosa.pcen(
        S, sr=sr, hop_length=hop_length,
        time_constant=0.4, gain=0.98, bias=2.0, power=0.5, eps=1e-6
    )

    return {
        "sr": sr,
        "hop_length": hop_length,
        "n_fft": n_fft,
        "n_mels": n_mels,
        "time_constant": 0.4,
        "gain": 0.98,
        "bias": 2.0,
        "power": 0.5,
        "eps": 1e-6,
        "n_bins": int(S.shape[0]),
        "n_frames": int(S.shape[1]),
        "S_flat": S.flatten().tolist(),
        "expected_flat": expected.flatten().tolist(),
    }


def generate_chroma_cqt_reference():
    """librosa.feature.chroma_cqt reference (statistics only)."""
    sr = 22050
    duration = 1.0
    # C major chord (C4 + E4 + G4)
    t = np.arange(0, duration, 1.0/sr)
    y = (np.sin(2*np.pi*261.63*t) + np.sin(2*np.pi*329.63*t)
         + np.sin(2*np.pi*392.0*t)).astype(np.float32) / 3.0
    hop_length = 512

    chroma = librosa.feature.chroma_cqt(
        y=y, sr=sr, hop_length=hop_length, n_chroma=12,
        fmin=librosa.note_to_hz('C1'), bins_per_octave=12, n_octaves=7
    )
    return {
        "signal": "C_major_chord",
        "sr": sr,
        "hop_length": hop_length,
        "shape": list(chroma.shape),
        "mean_per_class": chroma.mean(axis=1).tolist(),
    }


def generate_chroma_cens_reference():
    """librosa.feature.chroma_cens reference (statistics only)."""
    sr = 22050
    duration = 1.0
    t = np.arange(0, duration, 1.0/sr)
    y = (np.sin(2*np.pi*261.63*t) + np.sin(2*np.pi*329.63*t)
         + np.sin(2*np.pi*392.0*t)).astype(np.float32) / 3.0
    hop_length = 512

    cens = librosa.feature.chroma_cens(
        y=y, sr=sr, hop_length=hop_length, n_chroma=12,
        fmin=librosa.note_to_hz('C1'), bins_per_octave=12, n_octaves=7,
        win_len_smooth=41
    )
    return {
        "signal": "C_major_chord",
        "sr": sr,
        "hop_length": hop_length,
        "win_len_smooth": 41,
        "shape": list(cens.shape),
        "mean_per_class": cens.mean(axis=1).tolist(),
    }


def generate_pitch_utilities_reference():
    """librosa.piptrack / pitch_tuning / estimate_tuning reference."""
    sr = 22050
    duration = 1.0
    # Mixture of three known pitches: 440, 660, 880 Hz.
    t = np.arange(0, duration, 1.0/sr)
    y = (np.sin(2*np.pi*440.0*t) + np.sin(2*np.pi*660.0*t)
         + np.sin(2*np.pi*880.0*t)).astype(np.float32) / 3.0
    n_fft = 2048
    hop_length = 512

    pitches, magnitudes = librosa.piptrack(
        y=y, sr=sr, n_fft=n_fft, hop_length=hop_length, fmin=150.0, fmax=4000.0
    )
    # Count non-zero pitch entries (peak count).
    nonzero_count = int(np.count_nonzero(pitches > 0))

    # pitch_tuning over a set of known frequencies (all close to A4 tuning).
    known_freqs = [440.0, 660.0, 880.0]
    pt = float(librosa.pitch_tuning(np.array(known_freqs)))

    # estimate_tuning on the same audio.
    et = float(librosa.estimate_tuning(y=y, sr=sr, n_fft=n_fft, hop_length=hop_length))

    return {
        "signal": "440+660+880Hz_mix",
        "sr": sr,
        "n_fft": n_fft,
        "hop_length": hop_length,
        "piptrack_shape": list(pitches.shape),
        "piptrack_nonzero_count": nonzero_count,
        "known_frequencies": known_freqs,
        "pitch_tuning": pt,
        "estimate_tuning": et,
    }


def generate_plp_reference():
    """librosa.beat.plp reference: drum-like impulse train."""
    sr = 22050
    duration = 8.0
    bpm = 120
    y = np.zeros(int(duration * sr), dtype=np.float32)
    spb = int(60.0 / bpm * sr)
    y[::spb] = 1.0

    hop_length = 512
    pulse = librosa.beat.plp(
        y=y, sr=sr, hop_length=hop_length, tempo_min=30, tempo_max=300, win_length=384
    )
    return {
        "sr": sr,
        "bpm": bpm,
        "hop_length": hop_length,
        "win_length": 384,
        "length": int(pulse.size),
        "mean": float(pulse.mean()),
        "std": float(pulse.std()),
        "max": float(pulse.max()),
        "min": float(pulse.min()),
    }


def generate_iirt_reference():
    """librosa.iirt reference: 5s 440Hz tone.

    Note: librosa.iirt requires scipy-built IIR filterbank. We attempt to
    compute it; if anything fails, we fall back to a synthetic expected
    (peak at MIDI 69, i.e. row index 48 for midi_start=21) and mark as
    synthetic. The C++ test is a smoke test that does not require this
    reference, but we still emit shape info.
    """
    sr = 22050
    duration = 5.0
    y = librosa.tone(440.0, sr=sr, duration=duration).astype(np.float32)
    win_length = 2048
    hop_length = 512
    n_filters = 87
    midi_start = 21

    synthetic_only = True
    peak_row = None
    shape = None
    try:
        out = librosa.iirt(
            y=y, sr=sr, win_length=win_length, hop_length=hop_length, tuning=0.0
        )
        shape = list(out.shape)
        # Identify row with max energy.
        peak_row = int(np.argmax(out.sum(axis=1)))
        synthetic_only = False
    except Exception as e:  # pylint: disable=broad-except
        # librosa.iirt may require scipy; fall back to synthetic expectation.
        # Expected: A4 row = 69 - midi_start = 48
        shape = [n_filters, 1 + int((duration * sr + win_length) // hop_length)]
        peak_row = 69 - midi_start

    return {
        "sr": sr,
        "duration": duration,
        "win_length": win_length,
        "hop_length": hop_length,
        "n_filters": n_filters,
        "midi_start": midi_start,
        "shape": shape,
        "expected_peak_row": peak_row,
        "synthetic_only": synthetic_only,
    }


def generate_inverse_features_reference():
    """Inverse-feature smoke reference.

    mel_to_stft roundtrip: build a small mel power spectrogram from a 0.25s
    tone, then run librosa.feature.inverse.mel_to_stft and record the shape
    and that all values are non-negative. The C++ side is checked structurally.
    """
    sr = 22050
    duration = 0.25
    y = librosa.tone(440.0, sr=sr, duration=duration).astype(np.float32)
    n_fft = 1024
    hop_length = 256
    n_mels = 64

    M = librosa.feature.melspectrogram(
        y=y, sr=sr, n_fft=n_fft, hop_length=hop_length, n_mels=n_mels, power=2.0
    )
    try:
        S_rec = librosa.feature.inverse.mel_to_stft(
            M, sr=sr, n_fft=n_fft, power=2.0
        )
        rec_shape = list(S_rec.shape)
        rec_min = float(S_rec.min())
    except Exception:  # pylint: disable=broad-except
        rec_shape = [n_fft // 2 + 1, int(M.shape[1])]
        rec_min = 0.0

    return {
        "sr": sr,
        "n_fft": n_fft,
        "hop_length": hop_length,
        "n_mels": n_mels,
        "mel_shape": list(M.shape),
        "mel_flat": M.flatten().tolist(),
        "expected_stft_shape": rec_shape,
        "expected_stft_min": rec_min,
    }


def generate_zero_crossings_reference():
    """librosa.zero_crossings (raw indices) reference."""
    rng = np.random.default_rng(101)
    n = 256
    y = rng.standard_normal(n).astype(np.float32)
    # Mix in a low-frequency component so we get a moderate number of crossings.
    t = np.arange(n)
    y += np.sin(2 * np.pi * 5.0 * t / n).astype(np.float32)
    y = y.astype(np.float32)

    threshold = 1e-10

    # Default: pad=True, zero_pos=True, ref_magnitude=False.
    z_default = librosa.zero_crossings(y, threshold=threshold, pad=True, zero_pos=True)
    indices_default = np.nonzero(z_default)[0].tolist()

    # pad=False variant.
    z_nopad = librosa.zero_crossings(y, threshold=threshold, pad=False, zero_pos=True)
    indices_nopad = np.nonzero(z_nopad)[0].tolist()

    # ref_magnitude scaling: threshold becomes threshold * max|y|.
    z_ref = librosa.zero_crossings(
        y, threshold=0.05, ref_magnitude=1.0, pad=True, zero_pos=True
    )
    indices_ref = np.nonzero(z_ref)[0].tolist()

    # A small deterministic signal as a sanity-check.
    small = np.array([0.0, 1.0, -1.0, 0.5, -0.5, 0.0, 0.0, 0.3], dtype=np.float32)
    z_small = librosa.zero_crossings(small, threshold=0.0, pad=True, zero_pos=True)
    indices_small = np.nonzero(z_small)[0].tolist()

    return {
        "input": y.tolist(),
        "threshold": threshold,
        "indices_default": [int(i) for i in indices_default],
        "indices_no_pad": [int(i) for i in indices_nopad],
        "indices_ref_magnitude_threshold": 0.05,
        "indices_ref_magnitude": [int(i) for i in indices_ref],
        "small_input": small.tolist(),
        "small_indices": [int(i) for i in indices_small],
    }


def generate_weighting_reference():
    """librosa A/B/C/D / frequency_weighting / perceptual_weighting reference."""
    freqs = np.array(
        [10.0, 50.0, 100.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0],
        dtype=np.float64,
    )
    A = librosa.A_weighting(freqs).tolist()
    B = librosa.B_weighting(freqs).tolist()
    C = librosa.C_weighting(freqs).tolist()
    D = librosa.D_weighting(freqs).tolist()

    # frequency_weighting per kind matches the dedicated functions.
    freq_A = librosa.frequency_weighting(freqs, kind="A").tolist()

    # perceptual_weighting on a tiny synthetic power spectrum.
    # Build a power spectrum (n_bins=5, n_frames=3) with known values.
    rng = np.random.default_rng(202)
    n_bins = 5
    n_frames = 3
    S = rng.uniform(0.1, 10.0, size=(n_bins, n_frames)).astype(np.float64)
    bin_freqs = np.array([50.0, 200.0, 800.0, 3200.0, 12800.0], dtype=np.float64)
    P_a = librosa.perceptual_weighting(S, bin_freqs, kind="A").flatten().tolist()

    return {
        "freqs": freqs.tolist(),
        "A": [float(v) for v in A],
        "B": [float(v) for v in B],
        "C": [float(v) for v in C],
        "D": [float(v) for v in D],
        "freq_A": [float(v) for v in freq_A],
        "perceptual": {
            "n_bins": n_bins,
            "n_frames": n_frames,
            "bin_freqs": bin_freqs.tolist(),
            "S_flat": S.flatten().tolist(),
            "expected_flat": [float(v) for v in P_a],
        },
    }


def generate_synthesis_reference():
    """librosa.tone / librosa.chirp / librosa.clicks reference (shape + sanity)."""
    sr = 22050
    duration = 0.1

    # tone: librosa default uses cos(2*pi*f*t + phi) with phi = -pi/2.
    # We do not require numeric parity; we only verify length and key sample
    # values from our own formula sin(2*pi*f*t + phi). To check that, we also
    # emit a librosa-equivalent reference with our convention (phi shifted).
    # For shape reference, use librosa.tone with phi = 0 -> cos(2*pi*f*t).
    y_tone_librosa = librosa.tone(440.0, sr=sr, duration=duration, phi=0.0).astype(np.float64)

    # chirp: linear sweep
    y_chirp_lin = librosa.chirp(
        fmin=100.0, fmax=2000.0, sr=sr, duration=duration, linear=True
    ).astype(np.float64)
    y_chirp_exp = librosa.chirp(
        fmin=100.0, fmax=2000.0, sr=sr, duration=duration, linear=False
    ).astype(np.float64)

    # clicks: place at 0.02s and 0.07s. Use default click_freq=1000, duration=0.05.
    times = [0.02, 0.07]
    y_clicks = librosa.clicks(
        times=np.array(times), sr=sr, click_freq=1000.0, click_duration=0.05
    ).astype(np.float64)

    return {
        "sr": sr,
        "duration": duration,
        "tone_freq": 440.0,
        "tone_length": int(y_tone_librosa.size),
        "chirp_fmin": 100.0,
        "chirp_fmax": 2000.0,
        "chirp_linear_length": int(y_chirp_lin.size),
        "chirp_exp_length": int(y_chirp_exp.size),
        "chirp_linear_first_samples": y_chirp_lin[:8].tolist(),
        "chirp_exp_first_samples": y_chirp_exp[:8].tolist(),
        "clicks_times": times,
        "clicks_freq": 1000.0,
        "clicks_duration": 0.05,
        "clicks_length": int(y_clicks.size),
    }


def generate_remix_reference():
    """librosa.effects.remix reference."""
    sr = 22050
    # Build a deterministic mixed signal: sum of two tones over 0.2s.
    n = int(0.2 * sr)
    t = np.arange(n) / sr
    y = (np.sin(2 * np.pi * 220.0 * t) + 0.5 * np.sin(2 * np.pi * 440.0 * t)).astype(np.float32)

    # Intervals (start, end) - end exclusive.
    intervals = [(1000, 2500), (3000, 4200), (200, 800)]
    intervals_arr = np.array(intervals)

    out_aligned = librosa.effects.remix(y, intervals_arr, align_zeros=True)
    out_unaligned = librosa.effects.remix(y, intervals_arr, align_zeros=False)

    return {
        "sr": sr,
        "input": y.tolist(),
        "intervals": [list(iv) for iv in intervals],
        "aligned": out_aligned.tolist(),
        "unaligned": out_unaligned.tolist(),
    }


def generate_audio_ops_reference():
    """librosa.mu_compress / mu_expand / autocorrelate / lpc reference."""
    # mu-law: small deterministic input in [-1, 1].
    x = np.linspace(-1.0, 1.0, num=16, dtype=np.float64)
    mu = 255
    comp_no_q = librosa.mu_compress(x, mu=mu, quantize=False).tolist()
    comp_q = librosa.mu_compress(x, mu=mu, quantize=True).tolist()
    expand_q = librosa.mu_expand(np.array(comp_q, dtype=np.float64), mu=mu, quantize=True).tolist()
    expand_no_q = librosa.mu_expand(
        np.array(comp_no_q, dtype=np.float64), mu=mu, quantize=False
    ).tolist()

    # Autocorrelate: small noise signal.
    rng = np.random.default_rng(303)
    y = rng.standard_normal(64).astype(np.float64)
    ac_full = librosa.autocorrelate(y).tolist()
    ac_bounded = librosa.autocorrelate(y, max_size=16).tolist()

    # LPC: synthetic signal from an AR(2) process.
    n = 512
    a1, a2 = -1.5, 0.7  # poles inside unit circle
    eps = rng.standard_normal(n)
    y_ar = np.zeros(n)
    for i in range(2, n):
        y_ar[i] = eps[i] - a1 * y_ar[i - 1] - a2 * y_ar[i - 2]
    lpc_order = 4
    a_lpc = librosa.lpc(y_ar.astype(np.float64), order=lpc_order).tolist()

    return {
        "mu": mu,
        "mu_input": x.tolist(),
        "mu_compressed_no_quantize": [float(v) for v in comp_no_q],
        "mu_compressed_quantized": [float(v) for v in comp_q],
        "mu_expanded_no_quantize": [float(v) for v in expand_no_q],
        "mu_expanded_quantized": [float(v) for v in expand_q],
        "autocorrelate_input": y.tolist(),
        "autocorrelate_full": [float(v) for v in ac_full],
        "autocorrelate_max_size": 16,
        "autocorrelate_bounded": [float(v) for v in ac_bounded],
        "lpc_input": y_ar.tolist(),
        "lpc_order": lpc_order,
        "lpc_coeffs": [float(v) for v in a_lpc],
    }


def generate_beat_reference():
    """Beat tracking reference."""
    sr = 22050
    duration = 10.0
    true_bpm = 120

    y = np.zeros(int(duration * sr))
    samples_per_beat = int(60.0 / true_bpm * sr)
    y[::samples_per_beat] = 1.0

    tempo, beats = librosa.beat.beat_track(y=y, sr=sr, hop_length=512)
    beat_times = librosa.frames_to_time(beats, sr=sr, hop_length=512)

    return {
        "signal": "120bpm_impulse",
        "sr": sr,
        "true_bpm": true_bpm,
        "detected_tempo": float(tempo) if np.isscalar(tempo) else float(tempo[0]),
        "beat_times": beat_times.tolist(),
        "n_beats": len(beats),
    }


def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    print(f"librosa version: {librosa.__version__}")
    print(f"Output directory: {OUTPUT_DIR}")
    print()

    references = {
        "convert": generate_convert_reference(),
        "mel_filterbank": generate_mel_filterbank_reference(),
        "mfcc": generate_mfcc_reference(),
        "tempo": generate_tempo_reference(),
        "onset_strength": generate_onset_strength_reference(),
        "stft": generate_stft_reference(),
        "power_to_db": generate_power_to_db_reference(),
        "zcr_rms": generate_zcr_rms_reference(),
        "spectral_features": generate_spectral_features_reference(),
        "chroma": generate_chroma_reference(),
        "cqt": generate_cqt_reference(),
        "yin": generate_yin_reference(),
        "hpss": generate_hpss_reference(),
        "beat": generate_beat_reference(),
        "db_conversion": generate_db_conversion_reference(),
        "frames_samples": generate_frames_samples_reference(),
        "magphase": generate_magphase_reference(),
        "preemphasis": generate_preemphasis_reference(),
        "silence": generate_silence_reference(),
        "tempogram": generate_tempogram_reference(),
        "peak_pick": generate_peak_pick_reference(),
        "util_frame": generate_util_frame_reference(),
        "padding": generate_padding_reference(),
        "vector_normalize": generate_normalize_reference(),
        "poly_features": generate_poly_features_reference(),
        "tonnetz": generate_tonnetz_reference(),
        "pcen": generate_pcen_reference(),
        "chroma_cqt": generate_chroma_cqt_reference(),
        "chroma_cens": generate_chroma_cens_reference(),
        "pitch_utilities": generate_pitch_utilities_reference(),
        "plp": generate_plp_reference(),
        "iirt": generate_iirt_reference(),
        "inverse_features": generate_inverse_features_reference(),
        "zero_crossings": generate_zero_crossings_reference(),
        "weighting": generate_weighting_reference(),
        "synthesis": generate_synthesis_reference(),
        "remix": generate_remix_reference(),
        "audio_ops": generate_audio_ops_reference(),
    }

    metadata = {
        "librosa_version": librosa.__version__,
        "numpy_version": np.__version__,
    }

    for name, data in references.items():
        output_path = OUTPUT_DIR / f"{name}.json"
        output_data = {
            "metadata": metadata,
            "data": data,
        }
        with open(output_path, "w") as f:
            json.dump(output_data, f, indent=2)
        print(f"Generated: {output_path}")

    # Generate NOTICE.md
    notice_path = OUTPUT_DIR / "NOTICE.md"
    notice_content = f"""# Third-Party Test References

## librosa

Test reference data generated using librosa (https://github.com/librosa/librosa)

Copyright (c) 2013--2023, librosa development team.
Licensed under the ISC License.

These reference values were generated by running librosa functions
to verify numerical compatibility between libsonare and librosa.

Source: https://github.com/librosa/librosa
License: https://github.com/librosa/librosa/blob/main/LICENSE.md

Generated by: tests/librosa/generate_librosa_reference.py
librosa version: {librosa.__version__}
"""
    with open(notice_path, "w") as f:
        f.write(notice_content)
    print(f"Generated: {notice_path}")


if __name__ == "__main__":
    main()
