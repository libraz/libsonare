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
    """MFCC reference using synthetic signal."""
    # Generate test signal: 440Hz tone
    sr = 22050
    duration = 1.0
    y = librosa.tone(440.0, sr=sr, duration=duration)

    refs = []
    for n_mfcc in [13, 20]:
        for n_mels in [64, 128]:
            mfcc = librosa.feature.mfcc(
                y=y, sr=sr, n_mfcc=n_mfcc, n_mels=n_mels
            )
            refs.append({
                "signal": "440Hz_tone",
                "sr": sr,
                "n_mfcc": n_mfcc,
                "n_mels": n_mels,
                "shape": list(mfcc.shape),
                "mean": mfcc.mean(axis=1).tolist(),
                "std": mfcc.std(axis=1).tolist(),
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
    """Onset strength envelope reference."""
    sr = 22050
    duration = 1.0

    # Generate test signal with clear onsets
    y = np.zeros(int(duration * sr))
    # Add impulses at 0.2s intervals
    for t in [0.2, 0.4, 0.6, 0.8]:
        idx = int(t * sr)
        y[idx:idx+100] = np.hanning(100)

    refs = []
    for hop_length in [256, 512]:
        onset_env = librosa.onset.onset_strength(
            y=y, sr=sr, hop_length=hop_length
        )
        refs.append({
            "signal": "impulse_train",
            "sr": sr,
            "hop_length": hop_length,
            "shape": list(onset_env.shape),
            "max": float(onset_env.max()),
            "mean": float(onset_env.mean()),
            "nonzero_count": int(np.count_nonzero(onset_env > 0.1)),
        })

    return refs


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
