# CLI Reference

Complete reference for the `sonare` command-line interface.

## Overview

The sonare CLI provides comprehensive music analysis, audio processing, and feature extraction capabilities from the command line.

```bash
sonare <command> [options] <audio_file> [-o output]
```

## Global Options

| Option | Description |
|--------|-------------|
| `--json` | Output results in JSON format |
| `--quiet`, `-q` | Suppress progress output |
| `--help`, `-h` | Show help for command |
| `-o`, `--output` | Output file path |
| `--n-fft <int>` | FFT size (default: 2048) |
| `--hop-length <int>` | Hop length (default: 512) |
| `--n-mels <int>` | Number of Mel bands (default: 128) |
| `--fmin <float>` | Minimum frequency in Hz |
| `--fmax <float>` | Maximum frequency in Hz |

---

## Analysis Commands

### analyze

Performs full music analysis including BPM, key, beats, chords, sections, timbre, dynamics, and rhythm.

```bash
sonare analyze music.mp3
sonare analyze music.mp3 --json
```

**Output:**
```
BPM: 120.5 (confidence: 0.95)
Key: C major (confidence: 0.85)
Time Signature: 4/4
Beats: 240
Sections: Intro (0-8s), Verse (8-32s), Chorus (32-48s)
Form: IABABCO
```

**JSON Output:**
```json
{
  "bpm": 120.5,
  "bpmConfidence": 0.95,
  "key": {
    "root": 0,
    "mode": 0,
    "confidence": 0.85,
    "name": "C major"
  },
  "timeSignature": {
    "numerator": 4,
    "denominator": 4,
    "confidence": 0.9
  },
  "beats": [...],
  "chords": [...],
  "sections": [...],
  "timbre": {...},
  "dynamics": {...},
  "rhythm": {...},
  "form": "IABABCO"
}
```

---

### bpm

Detect tempo (BPM) only.

```bash
sonare bpm music.mp3
sonare bpm music.wav --json
```

**Output:**
```
BPM: 128.0
```

---

### key

Detect musical key.

```bash
sonare key music.mp3
sonare key music.mp3 --json
```

**Output:**
```
Key: A minor (confidence: 0.82)
```

**JSON Output:**
```json
{"root": 9, "mode": 1, "confidence": 0.82, "name": "A minor"}
```

---

### beats

Detect beat times.

```bash
sonare beats music.mp3
sonare beats music.mp3 --json
```

**Output:**
```
Beat times (240 beats):
0.52, 1.02, 1.52, 2.02, 2.52, 3.02, 3.52, 4.02, 4.52, 5.02,
...
```

**JSON Output:**
```json
[0.52, 1.02, 1.52, 2.02, ...]
```

---

### onsets

Detect onset times (note attacks).

```bash
sonare onsets music.mp3
sonare onsets music.mp3 --json
```

---

### chords

Detect chord progression.

```bash
sonare chords music.mp3
sonare chords music.mp3 --min-duration 0.5 --threshold 0.3
sonare chords music.mp3 --triads-only
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--min-duration` | 0.3 | Minimum chord duration in seconds |
| `--threshold` | 0.5 | Minimum confidence threshold |
| `--triads-only` | false | Only detect triads (major/minor/dim/aug) |

**Output:**
```
Chord progression: C - G - Am - F
Duration: 180.5s, 48 chord changes

Time      Chord    Confidence
0.00s     C        0.85
4.02s     G        0.78
8.04s     Am       0.82
12.06s    F        0.80
```

---

### sections

Detect song structure (intro, verse, chorus, etc.).

```bash
sonare sections music.mp3
sonare sections music.mp3 --min-duration 4.0 --threshold 0.3
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--min-duration` | 4.0 | Minimum section duration in seconds |
| `--threshold` | 0.3 | Boundary detection threshold |

**Output:**
```
Form: IABABCO
Duration: 180.5s, 7 sections

Section   Type         Start    End      Energy
1         Intro        0.00s    8.52s    0.45
2         Verse        8.52s    32.10s   0.62
3         Chorus       32.10s   48.20s   0.85
```

---

### timbre

Analyze timbral characteristics.

```bash
sonare timbre music.mp3
sonare timbre music.mp3 --json
```

**Output:**
```
Timbre Analysis:
  Brightness:  0.65 (bright)
  Warmth:      0.42 (neutral)
  Density:     0.78 (rich)
  Roughness:   0.25 (smooth)
  Complexity:  0.55 (moderate)
```

---

### dynamics

Analyze dynamics and loudness.

```bash
sonare dynamics music.mp3
sonare dynamics music.mp3 --window-sec 0.4
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--window-sec` | 0.4 | Analysis window in seconds |

**Output:**
```
Dynamics Analysis:
  Peak Level:       -0.5 dB
  RMS Level:        -12.3 dB
  Dynamic Range:    15.2 dB
  Crest Factor:     11.8 dB
  Loudness Range:   8.5 LU
  Compression:      No (natural)
```

---

### rhythm

Analyze rhythm features.

```bash
sonare rhythm music.mp3
sonare rhythm music.mp3 --bpm-min 60 --bpm-max 200
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--start-bpm` | 120.0 | Initial BPM estimate |
| `--bpm-min` | 60.0 | Minimum BPM search range |
| `--bpm-max` | 200.0 | Maximum BPM search range |

**Output:**
```
Rhythm Analysis:
  Time Signature:    4/4 (confidence: 0.92)
  BPM:               128.0
  Groove Type:       straight
  Syncopation:       0.35 (moderate)
  Pattern Regularity: 0.85 (regular)
  Tempo Stability:   0.92 (stable)
```

---

### melody

Track melody and pitch contour.

```bash
sonare melody music.mp3
sonare melody music.mp3 --fmin 80 --fmax 1000 --threshold 0.1
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--fmin` | 80.0 | Minimum frequency (Hz) |
| `--fmax` | 1000.0 | Maximum frequency (Hz) |
| `--threshold` | 0.1 | Voicing threshold |

**Output:**
```
Melody Analysis:
  Has Melody:      Yes
  Pitch Range:     1.52 octaves
  Mean Frequency:  320.5 Hz
  Pitch Stability: 0.78
  Vibrato Rate:    5.2 Hz
  Pitch Points:    4520
```

---

### boundaries

Detect structural boundaries.

```bash
sonare boundaries music.mp3
sonare boundaries music.mp3 --threshold 0.3 --min-distance 2.0
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--threshold` | 0.3 | Detection threshold |
| `--kernel-size` | 64 | Checkerboard kernel size |
| `--min-distance` | 2.0 | Minimum distance between boundaries (seconds) |

**Output:**
```
Structural Boundaries (6 detected):
Time      Strength
8.52s     0.85
32.10s    0.92
48.20s    0.78
```

---

## Processing Commands

### pitch-shift

Shift pitch by semitones without changing tempo.

```bash
sonare pitch-shift --semitones 3 input.wav -o output.wav
sonare pitch-shift --semitones -5 input.mp3 -o lower.wav
```

**Options:**

| Option | Required | Description |
|--------|----------|-------------|
| `--semitones` | Yes | Number of semitones (positive = up, negative = down) |

---

### time-stretch

Change tempo without changing pitch.

```bash
sonare time-stretch --rate 0.5 input.wav -o slower.wav   # Half speed
sonare time-stretch --rate 2.0 input.wav -o faster.wav   # Double speed
sonare time-stretch --rate 1.25 input.mp3 -o output.wav  # 25% faster
```

**Options:**

| Option | Required | Description |
|--------|----------|-------------|
| `--rate` | Yes | Stretch rate (0.5 = half speed, 2.0 = double speed) |

---

### hpss

Harmonic-Percussive Source Separation.

```bash
sonare hpss input.wav -o separated           # Creates separated_harmonic.wav and separated_percussive.wav
sonare hpss input.wav -o output --with-residual  # Also creates output_residual.wav
sonare hpss input.wav -o harmonic.wav --harmonic-only
sonare hpss input.wav -o percussive.wav --percussive-only
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--kernel-harmonic` | 31 | Horizontal kernel size (time) |
| `--kernel-percussive` | 31 | Vertical kernel size (frequency) |
| `--hard-mask` | false | Use hard masks instead of soft masks |
| `--with-residual` | false | Also output residual component |
| `--harmonic-only` | false | Output only harmonic component |
| `--percussive-only` | false | Output only percussive component |

---

## Feature Commands

### mel

Compute Mel spectrogram statistics.

```bash
sonare mel music.mp3
sonare mel music.mp3 --n-mels 80 --fmin 50 --fmax 8000
```

**Output:**
```
Mel Spectrogram:
  Shape:       128 bands x 8520 frames
  Duration:    180.52s
  Sample Rate: 22050 Hz
  Stats:       min=0.0001, max=0.8520, mean=0.0452
```

---

### chroma

Compute chromagram (pitch class distribution).

```bash
sonare chroma music.mp3
sonare chroma music.mp3 --json
```

**Output:**
```
Chromagram:
  Shape:    12 bins x 8520 frames
  Duration: 180.52s

Mean Energy by Pitch Class:
  C : 0.125 *************
  C#: 0.045 *****
  D : 0.082 ********
  ...
```

---

### spectral

Compute spectral features (centroid, bandwidth, rolloff, flatness, ZCR, RMS).

```bash
sonare spectral music.mp3
sonare spectral music.mp3 --json
```

**Output:**
```
Spectral Features (8520 frames):
  Feature          Mean       Std        Min        Max
  centroid         2150.5     850.2      120.5      8500.0
  bandwidth        1850.2     520.8      50.2       4200.5
  rolloff          4520.8     1200.5     200.0      10000.0
  flatness         0.0250     0.0180     0.0010     0.1520
  zcr              0.0850     0.0420     0.0020     0.2500
  rms              0.0520     0.0280     0.0001     0.1850
```

---

### pitch

Track pitch using YIN or pYIN algorithm.

```bash
sonare pitch music.mp3
sonare pitch music.mp3 --algorithm yin --fmin 65 --fmax 2093
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--algorithm` | pyin | Pitch algorithm: "yin" or "pyin" |
| `--fmin` | 65.0 | Minimum frequency (Hz) |
| `--fmax` | 2093.0 | Maximum frequency (Hz) |
| `--threshold` | 0.3 | Confidence threshold |

**Output:**
```
Pitch Tracking (pyin):
  Frames:    8520
  Voiced:    6250 (73.4%)
  Median F0: 285.5 Hz
  Mean F0:   302.8 Hz
```

---

### onset-env

Compute onset strength envelope.

```bash
sonare onset-env music.mp3
sonare onset-env music.mp3 --json
```

**Output:**
```
Onset Strength Envelope:
  Frames:       8520
  Duration:     180.52s
  Peak Time:    45.28s
  Peak Strength: 0.952
  Mean:         0.125
```

---

### cqt

Compute Constant-Q Transform.

```bash
sonare cqt music.mp3
sonare cqt music.mp3 --fmin 32.7 --n-bins 84 --bins-per-octave 12
```

**Options:**

| Option | Default | Description |
|--------|---------|-------------|
| `--fmin` | 32.7 | Minimum frequency (Hz) - C1 default |
| `--n-bins` | 84 | Number of frequency bins |
| `--bins-per-octave` | 12 | Bins per octave (12 = semitone resolution) |

**Output:**
```
Constant-Q Transform:
  Shape:          84 bins x 8520 frames
  Frequency Range: 32.7 - 4186.0 Hz (7 octaves)
  Duration:       180.52s
```

---

## Utility Commands

### info

Display audio file information.

```bash
sonare info music.mp3
sonare info music.wav --json
```

**Output:**
```
Audio File: music.mp3
  Duration:    3:00.5 (180.52s)
  Sample Rate: 22050 Hz
  Samples:     3980000
  Peak Level:  -0.5 dB
  RMS Level:   -12.3 dB
```

---

### version

Display version information.

```bash
sonare version
sonare version --json
```

**Output:**
```
sonare-cli version 1.0.0
libsonare version 1.0.0
```

---

## Examples

### Basic Analysis Workflow

```bash
# Quick BPM and key check
sonare bpm song.mp3
sonare key song.mp3

# Full analysis with JSON output for scripting
sonare analyze song.mp3 --json > analysis.json

# Detect chord progression
sonare chords song.mp3 --min-duration 0.5
```

### Audio Processing Workflow

```bash
# Transpose up by 2 semitones
sonare pitch-shift --semitones 2 original.wav -o transposed.wav

# Slow down for practice (80% speed)
sonare time-stretch --rate 0.8 song.wav -o practice.wav

# Extract drums and melody separately
sonare hpss song.wav -o separated
# Creates: separated_harmonic.wav, separated_percussive.wav
```

### Feature Extraction for ML

```bash
# Extract features for machine learning
sonare mel song.mp3 --json > mel_features.json
sonare spectral song.mp3 --json > spectral_features.json
sonare chroma song.mp3 --json > chroma_features.json
```

### Batch Processing

```bash
# Analyze all MP3 files in directory
for f in *.mp3; do
  echo "Processing: $f"
  sonare analyze "$f" --json > "${f%.mp3}.json"
done

# Extract BPM from all files
for f in *.wav; do
  bpm=$(sonare bpm "$f" --json | jq -r '.bpm')
  echo "$f: $bpm BPM"
done
```

---

## Exit Codes

| Code | Description |
|------|-------------|
| 0 | Success |
| 1 | Error (invalid arguments, file not found, processing error) |

---

## Supported Audio Formats

| Format | Extension | Notes |
|--------|-----------|-------|
| WAV | `.wav` | Uncompressed PCM |
| MP3 | `.mp3` | Decoded using minimp3 |

---

## Performance Tips

1. **Large files**: For files over 10 minutes, consider slicing first:
   ```bash
   # Analyze only first 60 seconds (using ffmpeg)
   ffmpeg -i long_song.mp3 -t 60 sample.wav
   sonare analyze sample.wav
   ```

2. **Batch processing**: Use `--quiet` to reduce output overhead:
   ```bash
   sonare analyze song.mp3 --quiet --json > result.json
   ```

3. **FFT size**: Smaller FFT size (`--n-fft 1024`) is faster but less frequency resolution.

4. **Hop length**: Larger hop length (`--hop-length 1024`) is faster but less time resolution.
