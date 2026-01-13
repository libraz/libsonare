# Architecture

This document describes the internal architecture of libsonare, including module structure, data flow, and design decisions.

## Module Overview

```mermaid
graph TB
    subgraph "API Layer"
        WASM["WASM Bindings<br/>(Embind)"]
        CAPI["C API<br/>(sonare_c.h)"]
        QUICK["Quick API<br/>(quick.h)"]
        UNIFIED["Unified Header<br/>(sonare.h)"]
    end

    subgraph "Analysis Layer"
        MUSIC["MusicAnalyzer"]
        BPM["BpmAnalyzer"]
        KEY["KeyAnalyzer"]
        BEAT["BeatAnalyzer"]
        CHORD["ChordAnalyzer"]
        SECTION["SectionAnalyzer"]
        TIMBRE["TimbreAnalyzer"]
        DYNAMICS["DynamicsAnalyzer"]
    end

    subgraph "Effects Layer"
        HPSS["HPSS"]
        TIMESTRETCH["Time Stretch"]
        PITCHSHIFT["Pitch Shift"]
        NORMALIZE["Normalize"]
    end

    subgraph "Feature Layer"
        MEL["MelSpectrogram"]
        CHROMA["Chroma"]
        CQT["CQT"]
        SPECTRAL["Spectral Features"]
        ONSET["Onset Detection"]
        PITCH["Pitch Tracking"]
    end

    subgraph "Core Layer"
        AUDIO["Audio"]
        SPECTRUM["Spectrogram<br/>(STFT/iSTFT)"]
        FFT["FFT<br/>(KissFFT)"]
        WINDOW["Window Functions"]
        CONVERT["Unit Conversion"]
        RESAMPLE["Resampling<br/>(r8brain)"]
        AUDIO_IO["Audio I/O<br/>(dr_libs, minimp3)"]
    end

    subgraph "Utility Layer"
        TYPES["Types"]
        MATH["Math Utils"]
        EXCEPTION["Exception"]
    end

    subgraph "Filterbanks"
        MELFILTER["Mel Filterbank"]
        CHROMAFILTER["Chroma Filterbank"]
        DCT["DCT"]
        IIR["IIR Filters"]
    end

    WASM --> QUICK
    CAPI --> QUICK
    UNIFIED --> MUSIC
    QUICK --> MUSIC

    MUSIC --> BPM
    MUSIC --> KEY
    MUSIC --> BEAT
    MUSIC --> CHORD
    MUSIC --> SECTION
    MUSIC --> TIMBRE
    MUSIC --> DYNAMICS

    BPM --> ONSET
    KEY --> CHROMA
    BEAT --> ONSET
    CHORD --> CHROMA
    SECTION --> MEL
    TIMBRE --> MEL
    DYNAMICS --> AUDIO

    HPSS --> SPECTRUM
    TIMESTRETCH --> SPECTRUM
    PITCHSHIFT --> TIMESTRETCH

    MEL --> SPECTRUM
    MEL --> MELFILTER
    CHROMA --> SPECTRUM
    CHROMA --> CHROMAFILTER
    CQT --> FFT
    SPECTRAL --> SPECTRUM
    ONSET --> MEL
    PITCH --> AUDIO

    SPECTRUM --> FFT
    SPECTRUM --> WINDOW
    AUDIO --> AUDIO_IO
    AUDIO --> RESAMPLE

    FFT --> TYPES
    WINDOW --> TYPES
    CONVERT --> MATH
    MELFILTER --> CONVERT
    CHROMAFILTER --> CONVERT

    classDef api fill:#e1f5fe
    classDef analysis fill:#f3e5f5
    classDef effects fill:#fff3e0
    classDef feature fill:#e8f5e9
    classDef core fill:#fce4ec
    classDef util fill:#f5f5f5
    classDef filter fill:#fff8e1

    class WASM,CAPI,QUICK,UNIFIED api
    class MUSIC,BPM,KEY,BEAT,CHORD,SECTION,TIMBRE,DYNAMICS analysis
    class HPSS,TIMESTRETCH,PITCHSHIFT,NORMALIZE effects
    class MEL,CHROMA,CQT,SPECTRAL,ONSET,PITCH feature
    class AUDIO,SPECTRUM,FFT,WINDOW,CONVERT,RESAMPLE,AUDIO_IO core
    class TYPES,MATH,EXCEPTION util
    class MELFILTER,CHROMAFILTER,DCT,IIR filter
```

---

## Directory Structure

```
src/
├── util/               # Level 0: Basic utilities
│   ├── types.h         # MatrixView, ErrorCode, enums
│   ├── exception.h     # SonareException
│   └── math_utils.h    # mean, variance, argmax, etc.
│
├── core/               # Level 1-3: Core DSP
│   ├── convert.h       # Hz/Mel/MIDI conversion
│   ├── window.h        # Hann, Hamming, Blackman
│   ├── fft.h           # KissFFT wrapper
│   ├── spectrum.h      # STFT/iSTFT
│   ├── audio.h         # Audio buffer
│   ├── audio_io.h      # WAV/MP3 loading
│   └── resample.h      # r8brain resampling
│
├── filters/            # Level 4: Filterbanks
│   ├── mel.h           # Mel filterbank
│   ├── chroma.h        # Chroma filterbank
│   ├── dct.h           # DCT for MFCC
│   └── iir.h           # IIR filters
│
├── feature/            # Level 4: Feature extraction
│   ├── mel_spectrogram.h
│   ├── chroma.h
│   ├── cqt.h
│   ├── vqt.h
│   ├── spectral.h
│   ├── onset.h
│   └── pitch.h
│
├── effects/            # Level 5: Audio effects
│   ├── hpss.h          # Harmonic-percussive separation
│   ├── time_stretch.h  # Phase vocoder time stretch
│   ├── pitch_shift.h   # Pitch shifting
│   ├── phase_vocoder.h # Phase vocoder core
│   └── normalize.h     # Normalization, trim
│
├── analysis/           # Level 6: Music analysis
│   ├── music_analyzer.h    # Facade
│   ├── bpm_analyzer.h
│   ├── key_analyzer.h
│   ├── beat_analyzer.h
│   ├── chord_analyzer.h
│   ├── section_analyzer.h
│   ├── timbre_analyzer.h
│   ├── dynamics_analyzer.h
│   ├── rhythm_analyzer.h
│   ├── melody_analyzer.h
│   ├── onset_analyzer.h
│   └── boundary_detector.h
│
├── quick.h             # Simple function API
├── sonare.h            # Unified include header
├── sonare_c.h          # C API header
└── wasm/
    └── bindings.cpp    # Embind bindings
```

---

## Dependency Levels

| Level | Modules | Dependencies |
|-------|---------|--------------|
| 0 | util/ | None (header-only except math_utils) |
| 1 | core/convert, core/window | util/ |
| 2 | core/fft | util/, KissFFT |
| 3 | core/spectrum, core/audio | core/fft, core/window |
| 4 | filters/, feature/ | core/ |
| 5 | effects/ | core/, feature/ |
| 6 | analysis/ | feature/, effects/ |

---

## Data Flow

### Audio Analysis Pipeline

```mermaid
flowchart LR
    subgraph Input
        FILE[Audio File<br/>WAV/MP3]
        BUFFER[Raw Buffer<br/>float*]
    end

    subgraph Core
        AUDIO[Audio]
        STFT[STFT]
        SPEC[Spectrogram]
    end

    subgraph Features
        MEL[Mel Spectrogram]
        CHROMA[Chromagram]
        ONSET[Onset Strength]
    end

    subgraph Analysis
        BPM[BPM Detection]
        KEY[Key Detection]
        BEAT[Beat Tracking]
        CHORD[Chord Recognition]
    end

    subgraph Output
        RESULT[AnalysisResult]
    end

    FILE --> AUDIO
    BUFFER --> AUDIO
    AUDIO --> STFT
    STFT --> SPEC
    SPEC --> MEL
    SPEC --> CHROMA
    MEL --> ONSET
    ONSET --> BPM
    ONSET --> BEAT
    CHROMA --> KEY
    CHROMA --> CHORD
    BPM --> RESULT
    KEY --> RESULT
    BEAT --> RESULT
    CHORD --> RESULT
```

### Audio Effects Pipeline

```mermaid
flowchart LR
    subgraph Input
        AUDIO[Audio]
    end

    subgraph Transform
        STFT[STFT]
        SPEC[Complex<br/>Spectrogram]
    end

    subgraph Effects
        HPSS[HPSS]
        PV[Phase Vocoder]
    end

    subgraph Processing
        HARM[Harmonic Mask]
        PERC[Percussive Mask]
        STRETCH[Time Stretch]
        SHIFT[Pitch Shift]
    end

    subgraph Reconstruct
        ISTFT[iSTFT]
        RESAMPLE[Resample]
    end

    subgraph Output
        OUT[Processed Audio]
    end

    AUDIO --> STFT
    STFT --> SPEC
    SPEC --> HPSS
    SPEC --> PV
    HPSS --> HARM
    HPSS --> PERC
    PV --> STRETCH
    STRETCH --> RESAMPLE
    RESAMPLE --> SHIFT
    HARM --> ISTFT
    PERC --> ISTFT
    SHIFT --> ISTFT
    ISTFT --> OUT
```

---

## Class Design

### MusicAnalyzer (Facade Pattern)

```mermaid
classDiagram
    class MusicAnalyzer {
        -Audio audio_
        -MusicAnalyzerConfig config_
        -unique_ptr~BpmAnalyzer~ bpm_analyzer_
        -unique_ptr~KeyAnalyzer~ key_analyzer_
        -unique_ptr~BeatAnalyzer~ beat_analyzer_
        -unique_ptr~ChordAnalyzer~ chord_analyzer_
        -unique_ptr~SectionAnalyzer~ section_analyzer_
        +MusicAnalyzer(audio, config)
        +bpm() float
        +key() Key
        +beat_times() vector~float~
        +chords() vector~Chord~
        +analyze() AnalysisResult
        +set_progress_callback(callback)
    }

    class BpmAnalyzer {
        -OnsetAnalyzer onset_
        -float bpm_
        -float confidence_
        +BpmAnalyzer(audio, config)
        +bpm() float
        +confidence() float
        +bpm_candidates() vector~float~
    }

    class KeyAnalyzer {
        -Chroma chroma_
        -Key key_
        +KeyAnalyzer(audio, config)
        +key() Key
        +candidates(top_n) vector~KeyCandidate~
    }

    class BeatAnalyzer {
        -OnsetAnalyzer onset_
        -vector~Beat~ beats_
        +BeatAnalyzer(audio, config)
        +beats() vector~Beat~
        +beat_times() vector~float~
        +time_signature() TimeSignature
    }

    class ChordAnalyzer {
        -Chroma chroma_
        -vector~Chord~ chords_
        +ChordAnalyzer(audio, config)
        +chords() vector~Chord~
        +progression_pattern() string
    }

    class SectionAnalyzer {
        -BoundaryDetector detector_
        -vector~Section~ sections_
        +SectionAnalyzer(audio, config)
        +sections() vector~Section~
        +form() string
    }

    MusicAnalyzer --> BpmAnalyzer : lazy creates
    MusicAnalyzer --> KeyAnalyzer : lazy creates
    MusicAnalyzer --> BeatAnalyzer : lazy creates
    MusicAnalyzer --> ChordAnalyzer : lazy creates
    MusicAnalyzer --> SectionAnalyzer : lazy creates
```

### Audio Buffer (Shared Ownership)

```mermaid
classDiagram
    class Audio {
        -shared_ptr~AudioData~ data_
        -size_t offset_
        -size_t size_
        -int sample_rate_
        +from_file(path) Audio
        +from_buffer(ptr, size, sr) Audio
        +from_vector(vec, sr) Audio
        +slice(start, end) Audio
        +data() const float*
        +size() size_t
        +duration() float
    }

    class AudioData {
        -vector~float~ samples_
        +samples() const float*
        +size() size_t
    }

    Audio --> AudioData : shared_ptr
    Audio --> Audio : slice (zero-copy)
```

---

## Key Design Decisions

### 1. Lazy Initialization

MusicAnalyzer uses lazy initialization for individual analyzers. This allows:
- Only compute what's needed
- Share intermediate results between analyzers
- Reduce memory when using subset of features

```cpp
// Only BPM is computed
float bpm = analyzer.bpm();

// Key detection triggers chroma computation
Key key = analyzer.key();

// Full analysis computes everything
AnalysisResult result = analyzer.analyze();
```

### 2. Zero-Copy Audio Slicing

Audio uses `shared_ptr` with offset/size for zero-copy slicing:

```cpp
auto full = Audio::from_file("song.mp3");  // 10 MB

// Both share same underlying buffer
auto intro = full.slice(0, 30);     // 0-30 sec, zero-copy
auto chorus = full.slice(60, 90);   // 60-90 sec, zero-copy
```

### 3. WASM Compatibility

Core modules avoid:
- File I/O (handled by Audio I/O layer)
- Threading (single-threaded execution)
- Dynamic loading
- System-specific APIs

All external dependencies are either:
- Header-only (Eigen3)
- Statically linked (KissFFT, dr_libs, minimp3, r8brain)

### 4. librosa Compatibility

Default parameters match librosa for easy migration:

| Parameter | Default | librosa Default |
|-----------|---------|-----------------|
| sample_rate | 22050 | 22050 |
| n_fft | 2048 | 2048 |
| hop_length | 512 | 512 |
| n_mels | 128 | 128 |
| fmin | 0 | 0 |
| fmax | sr/2 | sr/2 |

Mel scale uses Slaney formula (librosa default).

---

## Third-Party Libraries

| Library | Location | Purpose | License |
|---------|----------|---------|---------|
| KissFFT | third_party/kissfft/ | FFT | BSD-3-Clause |
| Eigen3 | System/FetchContent | Matrix ops | MPL-2.0 |
| dr_libs | third_party/dr_libs/ | WAV decode | Public Domain |
| minimp3 | third_party/minimp3/ | MP3 decode | CC0-1.0 |
| r8brain | third_party/r8brain/ | Resampling | MIT |
| Catch2 | FetchContent | Testing | BSL-1.0 |

---

## Performance Considerations

### Memory Layout

- Spectrograms stored as column-major (frequency x time)
- Compatible with Eigen's default layout
- Efficient for frequency-wise operations

### Caching

- Spectrogram caches magnitude/power on first access
- MelSpectrogram reuses filterbank matrices
- Chroma reuses chroma filterbank

### Parallelization

- Single-threaded for WASM compatibility
- Frame-level parallelism possible for native builds (future)
