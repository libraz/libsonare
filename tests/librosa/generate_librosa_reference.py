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

    for name, y in [("440Hz_tone", y_tone), ("white_noise", y_noise)]:
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
