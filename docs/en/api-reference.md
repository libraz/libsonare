# API Reference

## Core Types

### Audio

Audio buffer class with zero-copy slicing support.

```cpp
class Audio {
public:
  // Factory methods
  static Audio from_buffer(const float* data, size_t length, int sample_rate);
  static Audio from_vector(std::vector<float> data, int sample_rate);
  static Audio from_file(const std::string& path);  // Native only
  static Audio from_memory(const uint8_t* data, size_t length);

  // Properties
  const float* data() const;
  size_t size() const;
  int sample_rate() const;
  float duration() const;
  int channels() const;  // Always 1 (mono)

  // Operations
  Audio slice(float start_sec, float end_sec) const;  // Zero-copy
  Audio slice_samples(size_t start, size_t end) const;
  Audio to_mono() const;
};
```

### Key

Musical key representation.

```cpp
struct Key {
  PitchClass root;      // C, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B
  Mode mode;            // Major, Minor
  float confidence;     // 0.0 - 1.0

  std::string to_string() const;  // e.g., "C major", "A minor"
};
```

### Beat

Beat information.

```cpp
struct Beat {
  float time;       // Time in seconds
  int frame;        // Frame index
  float strength;   // Beat strength (0.0 - 1.0)
};
```

### Chord

Chord information.

```cpp
struct Chord {
  PitchClass root;
  ChordQuality quality;  // Major, Minor, Diminished, Augmented, etc.
  float start;           // Start time in seconds
  float end;             // End time in seconds
  float confidence;

  std::string to_string() const;  // e.g., "C", "Am", "G7"
};
```

### Section

Structural section information.

```cpp
struct Section {
  SectionType type;    // Intro, Verse, Chorus, Bridge, etc.
  float start;         // Start time in seconds
  float end;           // End time in seconds
  float energy_level;  // Relative energy (0.0 - 1.0)
  float confidence;

  std::string to_string() const;  // e.g., "Verse (0.0-15.2s)"
};
```

### AnalysisResult

Complete analysis result.

```cpp
struct AnalysisResult {
  float bpm;
  Key key;
  TimeSignature time_signature;
  std::vector<Beat> beats;
  std::vector<Chord> chords;
  std::vector<Section> sections;
  Timbre timbre;
  Dynamics dynamics;
};
```

## Quick API

Simple functions for common analysis tasks.

```cpp
namespace sonare::quick {
  // BPM detection (±2 BPM accuracy)
  float detect_bpm(const float* samples, size_t length, int sample_rate);

  // Key detection with confidence
  Key detect_key(const float* samples, size_t length, int sample_rate);

  // Beat times in seconds
  std::vector<float> detect_beats(const float* samples, size_t length, int sample_rate);

  // Onset times in seconds
  std::vector<float> detect_onsets(const float* samples, size_t length, int sample_rate);

  // Full analysis
  AnalysisResult analyze(const float* samples, size_t length, int sample_rate);
}
```

## MusicAnalyzer

Facade class for comprehensive music analysis with lazy initialization.

```cpp
class MusicAnalyzer {
public:
  explicit MusicAnalyzer(const Audio& audio);

  // Individual analyzers (lazy initialized)
  BpmAnalyzer& bpm_analyzer();
  KeyAnalyzer& key_analyzer();
  OnsetAnalyzer& onset_analyzer();
  BeatAnalyzer& beat_analyzer();
  ChordAnalyzer& chord_analyzer();
  SectionAnalyzer& section_analyzer();
  RhythmAnalyzer& rhythm_analyzer();
  TimbreAnalyzer& timbre_analyzer();
  MelodyAnalyzer& melody_analyzer();
  DynamicsAnalyzer& dynamics_analyzer();

  // Quick access methods
  float bpm() const;
  Key key() const;
  std::vector<Beat> beats() const;
  std::vector<Chord> chords() const;
  std::vector<Section> sections() const;
  TimeSignature time_signature() const;
  Timbre timbre() const;
  Dynamics dynamics() const;

  // Full analysis
  AnalysisResult analyze() const;
};
```

## Feature Extraction

### MelSpectrogram

```cpp
struct MelConfig {
  int n_mels = 128;
  float fmin = 0.0f;
  float fmax = 0.0f;  // 0 = sr/2
  StftConfig stft;
};

class MelSpectrogram {
public:
  static MelSpectrogram compute(const Audio& audio, const MelConfig& config = {});

  MatrixView<float> power() const;
  std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;
  std::vector<float> mfcc(int n_mfcc = 13, float lifter = 0.0f) const;

  int n_mels() const;
  int n_frames() const;
};
```

### Chroma

```cpp
class Chroma {
public:
  static Chroma compute(const Audio& audio, const ChromaConfig& config = {});

  MatrixView<float> features() const;  // [12 x n_frames]
  std::array<float, 12> mean_energy() const;
  std::vector<int> dominant_pitch_class() const;

  int n_frames() const;
};
```

### Spectral Features

```cpp
// Per-frame spectral centroid (Hz)
std::vector<float> spectral_centroid(const Spectrogram& spec, int sr);

// Per-frame spectral bandwidth (Hz)
std::vector<float> spectral_bandwidth(const Spectrogram& spec, int sr);

// Per-frame spectral rolloff (Hz)
std::vector<float> spectral_rolloff(const Spectrogram& spec, int sr, float percent = 0.85f);

// Per-frame spectral flatness
std::vector<float> spectral_flatness(const Spectrogram& spec);

// Per-frame spectral contrast
std::vector<float> spectral_contrast(const Spectrogram& spec, int sr, int n_bands = 6);
```

## Effects

### HPSS (Harmonic-Percussive Source Separation)

```cpp
struct HpssConfig {
  int kernel_size_harmonic = 31;
  int kernel_size_percussive = 31;
  float power = 2.0f;
};

struct HpssAudioResult {
  Audio harmonic;
  Audio percussive;
};

HpssAudioResult hpss(const Audio& audio, const HpssConfig& config = {});
Audio harmonic(const Audio& audio, const HpssConfig& config = {});
Audio percussive(const Audio& audio, const HpssConfig& config = {});
```

### Time Stretch

```cpp
// Stretch audio by rate (0.5 = half speed, 2.0 = double speed)
Audio time_stretch(const Audio& audio, float rate, const TimeStretchConfig& config = {});
```

### Pitch Shift

```cpp
// Shift pitch by semitones (positive = higher, negative = lower)
Audio pitch_shift(const Audio& audio, float semitones, const PitchShiftConfig& config = {});

// Shift pitch by ratio (2.0 = one octave up)
Audio pitch_shift_ratio(const Audio& audio, float ratio, const PitchShiftConfig& config = {});
```

### Normalize

```cpp
// Peak normalization to target dB
Audio normalize(const Audio& audio, float target_db = 0.0f);

// RMS normalization
Audio normalize_rms(const Audio& audio, float target_db = -20.0f);

// Trim silence from start and end
Audio trim(const Audio& audio, float threshold_db = -60.0f);
```

## C API

```c
// Audio loading
SonareError sonare_audio_from_buffer(const float* data, size_t len, int sr, SonareAudio** out);
SonareError sonare_audio_from_file(const char* path, SonareAudio** out);
void sonare_audio_free(SonareAudio* audio);

// Analysis
SonareError sonare_detect_bpm(const SonareAudio* audio, float* out_bpm, float* out_confidence);
SonareError sonare_detect_key(const SonareAudio* audio, SonareKey* out_key);
SonareError sonare_detect_beats(const SonareAudio* audio, float** out_times, size_t* out_count);
SonareError sonare_analyze(const SonareAudio* audio, SonareAnalysisResult* out);

// Memory management
void sonare_free_floats(float* ptr);
void sonare_free_result(SonareAnalysisResult* result);

// Utility
const char* sonare_error_message(SonareError error);
const char* sonare_version(void);
```

## Unit Conversion

```cpp
// Hz ↔ Mel
float hz_to_mel(float hz);       // Slaney formula
float mel_to_hz(float mel);
float hz_to_mel_htk(float hz);   // HTK formula
float mel_to_hz_htk(float mel);

// Hz ↔ MIDI note number
float hz_to_midi(float hz);      // A4 = 440Hz = 69
float midi_to_hz(float midi);

// Hz ↔ Note name
std::string hz_to_note(float hz);    // e.g., "A4", "C#5"
float note_to_hz(const std::string& note);

// Time ↔ Frames
float frames_to_time(int frames, int sr, int hop_length);
int time_to_frames(float time, int sr, int hop_length);

// Samples ↔ Time
float samples_to_time(int samples, int sr);
int time_to_samples(float time, int sr);

// Bin ↔ Hz
float bin_to_hz(int bin, int sr, int n_fft);
int hz_to_bin(float hz, int sr, int n_fft);
```

## Enums

```cpp
enum class PitchClass {
  C = 0, Cs, D, Ds, E, F, Fs, G, Gs, A, As, B
};

enum class Mode {
  Major, Minor
};

enum class ChordQuality {
  Major, Minor, Diminished, Augmented,
  Dominant7, Major7, Minor7, Sus2, Sus4
};

enum class SectionType {
  Intro, Verse, PreChorus, Chorus, Bridge, Instrumental, Outro
};

enum class WindowType {
  Hann, Hamming, Blackman, Rectangular
};
```
