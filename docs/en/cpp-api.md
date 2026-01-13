# C++ API Reference

Complete API reference for libsonare C++ interface.

## Table of Contents

- [Namespaces](#namespaces)
- [Core Classes](#core-classes)
- [Feature Extraction](#feature-extraction)
- [Effects](#effects)
- [Analysis](#analysis)
- [Quick API](#quick-api)
- [C API](#c-api)
- [Utility Functions](#utility-functions)
- [Enumerations](#enumerations)
- [Structures](#structures)

> For architecture diagrams and data flow, see [Architecture](architecture.md).

---

## Namespaces

All libsonare functionality is contained within the `sonare` namespace.

```cpp
namespace sonare {
  // Core classes
  class Audio;
  class Spectrogram;

  // Feature extraction
  class MelSpectrogram;
  class Chroma;

  // Analysis
  class MusicAnalyzer;
  class BpmAnalyzer;
  class KeyAnalyzer;
  // ... etc

  namespace quick {
    // Simple function API
  }
}
```

---

## Core Classes

### Audio

Audio buffer with shared ownership and zero-copy slicing.

**Header:** `#include <sonare/core/audio.h>`

#### Factory Methods

```cpp
/// Creates Audio from raw sample buffer.
/// @param samples Pointer to float samples (will be copied)
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
static Audio Audio::from_buffer(const float* samples, size_t size, int sample_rate);

/// Creates Audio from vector (ownership transferred).
/// @param samples Vector of samples (moved)
/// @param sample_rate Sample rate in Hz
static Audio Audio::from_vector(std::vector<float> samples, int sample_rate);

/// Loads Audio from file (WAV, MP3).
/// @param path File path
/// @throws SonareException on file not found or decode error
static Audio Audio::from_file(const std::string& path);

/// Loads Audio from memory buffer.
/// @param data Pointer to audio file data
/// @param size Size in bytes
/// @throws SonareException on decode error
static Audio Audio::from_memory(const uint8_t* data, size_t size);
```

#### Properties

```cpp
const float* data() const;        // Pointer to sample data
size_t size() const;              // Number of samples
int sample_rate() const;          // Sample rate in Hz
float duration() const;           // Duration in seconds
int channels() const;             // Always 1 (mono)
bool empty() const;               // True if no samples
```

#### Operations

```cpp
/// Creates a slice (zero-copy, shared buffer).
/// @param start_time Start time in seconds
/// @param end_time End time in seconds (-1 = end of audio)
Audio slice(float start_time, float end_time = -1.0f) const;

/// Creates a slice by sample indices.
/// @param start_sample Start sample index
/// @param end_sample End sample index (-1 = end)
Audio slice_samples(size_t start_sample, size_t end_sample = -1) const;

/// Returns mono version (already mono, returns copy).
Audio to_mono() const;

/// Index operator for sample access.
float operator[](size_t index) const;

/// Iterator support.
const float* begin() const;
const float* end() const;
```

#### Example

```cpp
#include <sonare/sonare.h>

// Load from file
auto audio = sonare::Audio::from_file("song.mp3");
std::cout << "Duration: " << audio.duration() << "s\n";
std::cout << "Sample rate: " << audio.sample_rate() << " Hz\n";

// Zero-copy slicing
auto intro = audio.slice(0.0f, 30.0f);  // First 30 seconds
auto chorus = audio.slice(60.0f, 90.0f);  // 60-90 seconds

// Both share the same underlying buffer
assert(intro.data() == audio.data());  // Same pointer!
```

---

### Spectrogram

Short-Time Fourier Transform (STFT) of audio signal.

**Header:** `#include <sonare/core/spectrum.h>`

#### Configuration

```cpp
struct StftConfig {
  int n_fft = 2048;                      // FFT size
  int hop_length = 512;                  // Hop length between frames
  int win_length = 0;                    // Window length (0 = n_fft)
  WindowType window = WindowType::Hann;  // Window function
  bool center = true;                    // Pad signal to center frames

  int actual_win_length() const;         // Returns win_length or n_fft
};
```

#### Factory Methods

```cpp
/// Computes STFT of audio.
/// @param audio Input audio
/// @param config STFT configuration
/// @param progress_callback Optional progress callback (0.0 to 1.0)
static Spectrogram compute(
  const Audio& audio,
  const StftConfig& config = StftConfig(),
  SpectrogramProgressCallback progress_callback = nullptr
);

/// Creates from existing complex spectrum data.
static Spectrogram from_complex(
  const std::complex<float>* data,
  int n_bins, int n_frames,
  int n_fft, int hop_length, int sample_rate
);
```

#### Properties

```cpp
int n_bins() const;        // Frequency bins (n_fft/2 + 1)
int n_frames() const;      // Number of time frames
int n_fft() const;         // FFT size
int hop_length() const;    // Hop length
int sample_rate() const;   // Original sample rate
float duration() const;    // Duration in seconds
bool empty() const;        // True if empty
```

#### Data Access

```cpp
/// Complex spectrum view [n_bins x n_frames].
MatrixView<std::complex<float>> complex_view() const;

/// Raw complex data pointer.
const std::complex<float>* complex_data() const;

/// Magnitude spectrum (computed lazily, cached).
const std::vector<float>& magnitude() const;

/// Power spectrum (computed lazily, cached).
const std::vector<float>& power() const;

/// Magnitude in dB.
/// @param ref Reference value (default 1.0)
/// @param amin Minimum amplitude (default 1e-10)
std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;

/// Access complex value at (bin, frame).
const std::complex<float>& at(int bin, int frame) const;
```

#### Reconstruction

```cpp
/// Reconstructs audio via iSTFT.
/// @param length Target length in samples (0 = auto)
/// @param window Window function for synthesis
Audio to_audio(int length = 0, WindowType window = WindowType::Hann) const;
```

#### Example

```cpp
// Compute STFT
sonare::StftConfig config;
config.n_fft = 2048;
config.hop_length = 512;

auto spec = sonare::Spectrogram::compute(audio, config);

std::cout << "Spectrogram: " << spec.n_bins() << " bins x "
          << spec.n_frames() << " frames\n";

// Get power spectrum in dB
auto db = spec.to_db();

// Reconstruct audio
auto reconstructed = spec.to_audio();
```

---

### FFT

FFT wrapper using KissFFT.

**Header:** `#include <sonare/core/fft.h>`

```cpp
class FFT {
public:
  /// Constructs FFT processor.
  /// @param n_fft FFT size (should be power of 2)
  explicit FFT(int n_fft);

  ~FFT();

  // Non-copyable
  FFT(const FFT&) = delete;
  FFT& operator=(const FFT&) = delete;

  /// Forward real FFT.
  /// @param input Real input [n_fft]
  /// @param output Complex output [n_fft/2 + 1]
  void forward(const float* input, std::complex<float>* output);

  /// Inverse real FFT.
  /// @param input Complex input [n_fft/2 + 1]
  /// @param output Real output [n_fft]
  void inverse(const std::complex<float>* input, float* output);

  /// Returns FFT size.
  int size() const;
};
```

---

## Feature Extraction

### MelSpectrogram

Mel-frequency spectrogram and MFCC extraction.

**Header:** `#include <sonare/feature/mel_spectrogram.h>`

#### Configuration

```cpp
struct MelConfig {
  int n_mels = 128;       // Number of Mel bands
  float fmin = 0.0f;      // Minimum frequency
  float fmax = 0.0f;      // Maximum frequency (0 = sr/2)
  StftConfig stft;        // STFT configuration
};
```

#### Methods

```cpp
/// Computes Mel spectrogram from audio.
static MelSpectrogram compute(const Audio& audio, const MelConfig& config = MelConfig());

/// Computes from existing spectrogram.
static MelSpectrogram from_spectrogram(
  const Spectrogram& spec,
  int sample_rate,
  const MelFilterConfig& config = MelFilterConfig()
);

/// Returns Mel power spectrum [n_mels x n_frames].
MatrixView<float> power() const;

/// Returns Mel spectrum in dB.
std::vector<float> to_db(float ref = 1.0f, float amin = 1e-10f) const;

/// Computes MFCCs.
/// @param n_mfcc Number of coefficients (default 13)
/// @param lifter Liftering coefficient (0 = no liftering)
/// @return MFCC coefficients [n_mfcc x n_frames]
std::vector<float> mfcc(int n_mfcc = 13, float lifter = 0.0f) const;

/// Computes delta features.
/// @param data Input features
/// @param width Delta window width
static std::vector<float> delta(const std::vector<float>& data, int n_features, int n_frames, int width = 9);

int n_mels() const;
int n_frames() const;
```

#### Example

```cpp
sonare::MelConfig config;
config.n_mels = 128;
config.stft.n_fft = 2048;
config.stft.hop_length = 512;

auto mel = sonare::MelSpectrogram::compute(audio, config);

// Get MFCCs
auto mfcc = mel.mfcc(13);  // 13 coefficients

// Get delta MFCCs
auto delta = sonare::MelSpectrogram::delta(mfcc, 13, mel.n_frames());
```

---

### Chroma

Chromagram (pitch class energy distribution).

**Header:** `#include <sonare/feature/chroma.h>`

```cpp
struct ChromaConfig {
  int n_chroma = 12;         // Number of chroma bins (usually 12)
  float tuning = 0.0f;       // Tuning offset in cents
  StftConfig stft;           // STFT configuration
};

class Chroma {
public:
  static Chroma compute(const Audio& audio, const ChromaConfig& config = ChromaConfig());

  static Chroma from_spectrogram(
    const Spectrogram& spec,
    int sample_rate,
    const ChromaConfig& config = ChromaConfig()
  );

  /// Chroma features [12 x n_frames].
  MatrixView<float> features() const;

  /// Mean energy per pitch class.
  std::array<float, 12> mean_energy() const;

  /// Dominant pitch class per frame.
  std::vector<int> dominant_pitch_class() const;

  int n_frames() const;
};
```

---

### Spectral Features

Per-frame spectral features.

**Header:** `#include <sonare/feature/spectral.h>`

```cpp
/// Spectral centroid (center of mass) in Hz.
std::vector<float> spectral_centroid(const Spectrogram& spec, int sample_rate);

/// Spectral bandwidth in Hz.
std::vector<float> spectral_bandwidth(
  const Spectrogram& spec,
  int sample_rate,
  float p = 2.0f  // Order of the norm
);

/// Spectral rolloff frequency in Hz.
std::vector<float> spectral_rolloff(
  const Spectrogram& spec,
  int sample_rate,
  float percent = 0.85f  // Rolloff percentage
);

/// Spectral flatness (0 = tonal, 1 = noise-like).
std::vector<float> spectral_flatness(const Spectrogram& spec);

/// Spectral contrast per frequency band.
std::vector<float> spectral_contrast(
  const Spectrogram& spec,
  int sample_rate,
  int n_bands = 6,
  float fmin = 200.0f
);

/// Zero crossing rate.
std::vector<float> zero_crossing_rate(
  const Audio& audio,
  int frame_length = 2048,
  int hop_length = 512
);

/// RMS energy.
std::vector<float> rms_energy(
  const Audio& audio,
  int frame_length = 2048,
  int hop_length = 512
);
```

---

## Effects

### HPSS (Harmonic-Percussive Source Separation)

**Header:** `#include <sonare/effects/hpss.h>`

```cpp
struct HpssConfig {
  int kernel_size_harmonic = 31;    // Horizontal (time) kernel size
  int kernel_size_percussive = 31;  // Vertical (frequency) kernel size
  float power = 2.0f;               // Mask enhancement power
  float margin_harmonic = 1.0f;     // Harmonic margin
  float margin_percussive = 1.0f;   // Percussive margin
};

struct HpssSpectrogramResult {
  Spectrogram harmonic;
  Spectrogram percussive;
};

struct HpssAudioResult {
  Audio harmonic;
  Audio percussive;
};

/// HPSS on spectrogram.
HpssSpectrogramResult hpss(const Spectrogram& spec, const HpssConfig& config = HpssConfig());

/// HPSS on audio (convenience function).
HpssAudioResult hpss(
  const Audio& audio,
  const HpssConfig& config = HpssConfig(),
  const StftConfig& stft_config = StftConfig()
);

/// Extract harmonic component only.
Audio harmonic(const Audio& audio, const HpssConfig& config = HpssConfig());

/// Extract percussive component only.
Audio percussive(const Audio& audio, const HpssConfig& config = HpssConfig());
```

---

### Time Stretch

**Header:** `#include <sonare/effects/time_stretch.h>`

```cpp
struct TimeStretchConfig {
  int n_fft = 2048;
  int hop_length = 0;  // 0 = n_fft/4
};

/// Time-stretch audio without changing pitch.
/// @param audio Input audio
/// @param rate Stretch rate (0.5 = half speed, 2.0 = double speed)
/// @param config Configuration
/// @return Stretched audio
Audio time_stretch(
  const Audio& audio,
  float rate,
  const TimeStretchConfig& config = TimeStretchConfig()
);
```

---

### Pitch Shift

**Header:** `#include <sonare/effects/pitch_shift.h>`

```cpp
struct PitchShiftConfig {
  int n_fft = 2048;
  int hop_length = 0;  // 0 = n_fft/4
};

/// Shift pitch by semitones.
/// @param audio Input audio
/// @param semitones Number of semitones (positive = up, negative = down)
Audio pitch_shift(
  const Audio& audio,
  float semitones,
  const PitchShiftConfig& config = PitchShiftConfig()
);

/// Shift pitch by frequency ratio.
/// @param ratio Pitch ratio (2.0 = one octave up)
Audio pitch_shift_ratio(
  const Audio& audio,
  float ratio,
  const PitchShiftConfig& config = PitchShiftConfig()
);
```

---

### Normalize

**Header:** `#include <sonare/effects/normalize.h>`

```cpp
/// Peak normalization.
/// @param audio Input audio
/// @param target_db Target peak level in dB (default 0.0)
Audio normalize(const Audio& audio, float target_db = 0.0f);

/// RMS normalization.
Audio normalize_rms(const Audio& audio, float target_db = -20.0f);

/// Trim silence from start and end.
Audio trim(
  const Audio& audio,
  float threshold_db = -60.0f,
  int frame_length = 2048,
  int hop_length = 512
);

/// Detect silence boundaries.
std::pair<size_t, size_t> detect_silence_boundaries(
  const Audio& audio,
  float threshold_db = -60.0f
);

/// Get peak level in dB.
float peak_db(const Audio& audio);

/// Get RMS level in dB.
float rms_db(const Audio& audio);

/// Apply gain in dB.
Audio apply_gain(const Audio& audio, float gain_db);

/// Apply fade in.
Audio fade_in(const Audio& audio, float duration_sec);

/// Apply fade out.
Audio fade_out(const Audio& audio, float duration_sec);
```

---

## Analysis

### MusicAnalyzer

Unified music analysis facade with lazy initialization.

**Header:** `#include <sonare/analysis/music_analyzer.h>`

#### Configuration

```cpp
struct MusicAnalyzerConfig {
  int n_fft = 2048;
  int hop_length = 512;
  float bpm_min = 60.0f;
  float bpm_max = 200.0f;
  float start_bpm = 120.0f;
};
```

#### Constructor

```cpp
explicit MusicAnalyzer(
  const Audio& audio,
  const MusicAnalyzerConfig& config = MusicAnalyzerConfig()
);
```

#### Progress Callback

```cpp
using ProgressCallback = std::function<void(float progress, const char* stage)>;

void set_progress_callback(ProgressCallback callback);
```

Progress stages:
- `"bpm"` - BPM analysis
- `"key"` - Key detection
- `"beats"` - Beat tracking
- `"chords"` - Chord recognition
- `"sections"` - Section detection
- `"timbre"` - Timbre analysis
- `"dynamics"` - Dynamics analysis

#### Quick Access Methods

```cpp
float bpm();                      // Estimated BPM
Key key();                        // Detected key
std::vector<float> beat_times();  // Beat positions in seconds
std::vector<Chord> chords();      // Chord progression
std::string form();               // Song form string ("IABABCO")
```

#### Analyzer Access (Lazy Initialization)

```cpp
BpmAnalyzer& bpm_analyzer();
KeyAnalyzer& key_analyzer();
BeatAnalyzer& beat_analyzer();
ChordAnalyzer& chord_analyzer();
OnsetAnalyzer& onset_analyzer();
DynamicsAnalyzer& dynamics_analyzer();
RhythmAnalyzer& rhythm_analyzer();
TimbreAnalyzer& timbre_analyzer();
MelodyAnalyzer& melody_analyzer();
SectionAnalyzer& section_analyzer();
BoundaryDetector& boundary_detector();
```

#### Full Analysis

```cpp
AnalysisResult analyze();
```

#### Example

```cpp
auto audio = sonare::Audio::from_file("song.mp3");

sonare::MusicAnalyzerConfig config;
config.bpm_min = 80.0f;
config.bpm_max = 180.0f;

sonare::MusicAnalyzer analyzer(audio, config);

// Set progress callback
analyzer.set_progress_callback([](float progress, const char* stage) {
  std::cout << stage << ": " << (progress * 100) << "%\n";
});

// Get individual results
std::cout << "BPM: " << analyzer.bpm() << "\n";
std::cout << "Key: " << analyzer.key().to_string() << "\n";

// Or get full analysis
auto result = analyzer.analyze();

for (const auto& chord : result.chords) {
  std::cout << chord.to_string() << " [" << chord.start << "-" << chord.end << "s]\n";
}
```

---

### Individual Analyzers

Each analyzer can be used independently.

#### BpmAnalyzer

```cpp
struct BpmConfig {
  float bpm_min = 60.0f;
  float bpm_max = 200.0f;
  float start_bpm = 120.0f;
};

class BpmAnalyzer {
public:
  explicit BpmAnalyzer(const Audio& audio, const BpmConfig& config = BpmConfig());

  float bpm() const;                          // Primary BPM estimate
  float confidence() const;                   // Detection confidence (0-1)
  std::vector<float> bpm_candidates() const;  // Alternative BPM candidates
};
```

#### KeyAnalyzer

```cpp
struct KeyConfig {
  int hop_length = 512;
  bool use_hpss = true;       // Use harmonic component only
  float high_pass_hz = 80.0f; // High-pass filter cutoff
};

class KeyAnalyzer {
public:
  explicit KeyAnalyzer(const Audio& audio, const KeyConfig& config = KeyConfig());
  explicit KeyAnalyzer(const Chroma& chroma);

  Key key() const;
  std::vector<KeyCandidate> candidates(int top_n = 5) const;
};
```

#### BeatAnalyzer

```cpp
struct BeatConfig {
  float start_bpm = 120.0f;
  float tightness = 100.0f;  // Higher = stricter tempo
  bool trim = true;          // Remove beats outside audio
};

class BeatAnalyzer {
public:
  explicit BeatAnalyzer(const Audio& audio, const BeatConfig& config = BeatConfig());

  std::vector<Beat> beats() const;
  std::vector<float> beat_times() const;
  float bpm() const;
  TimeSignature time_signature() const;
};
```

#### ChordAnalyzer

```cpp
struct ChordConfig {
  float bpm = 0.0f;           // 0 = auto-detect
  float min_duration = 0.5f;  // Minimum chord duration in seconds
  float threshold = 0.3f;     // Minimum confidence threshold
};

class ChordAnalyzer {
public:
  explicit ChordAnalyzer(const Audio& audio, const ChordConfig& config = ChordConfig());
  explicit ChordAnalyzer(const Chroma& chroma, float sample_rate, const ChordConfig& config);

  std::vector<Chord> chords() const;
  std::string progression_pattern() const;  // "C - G - Am - F"
  std::vector<std::string> functional_analysis(const Key& key) const;  // Roman numerals
};
```

---

## Quick API

Simple stateless functions for common analysis tasks.

**Header:** `#include <sonare/quick.h>`

```cpp
namespace sonare::quick {

/// Detect BPM.
/// @param samples Pointer to mono float32 samples
/// @param size Number of samples
/// @param sample_rate Sample rate in Hz
/// @return Estimated BPM
float detect_bpm(const float* samples, size_t size, int sample_rate);

/// Detect musical key.
Key detect_key(const float* samples, size_t size, int sample_rate);

/// Detect onset times.
/// @return Vector of onset times in seconds
std::vector<float> detect_onsets(const float* samples, size_t size, int sample_rate);

/// Detect beat times.
/// @return Vector of beat times in seconds
std::vector<float> detect_beats(const float* samples, size_t size, int sample_rate);

/// Perform complete analysis.
AnalysisResult analyze(const float* samples, size_t size, int sample_rate);

}
```

---

## C API

C-compatible API for FFI integration.

**Header:** `#include <sonare_c.h>`

```c
// Error codes
typedef enum {
  SONARE_OK = 0,
  SONARE_ERROR_FILE_NOT_FOUND,
  SONARE_ERROR_INVALID_FORMAT,
  SONARE_ERROR_DECODE_FAILED,
  SONARE_ERROR_INVALID_PARAMETER,
  SONARE_ERROR_OUT_OF_MEMORY,
} SonareError;

// Opaque types
typedef struct SonareAudio SonareAudio;

// Audio functions
SonareError sonare_audio_from_buffer(
  const float* data, size_t len, int sample_rate, SonareAudio** out
);
SonareError sonare_audio_from_file(const char* path, SonareAudio** out);
void sonare_audio_free(SonareAudio* audio);

float sonare_audio_duration(const SonareAudio* audio);
int sonare_audio_sample_rate(const SonareAudio* audio);
size_t sonare_audio_size(const SonareAudio* audio);

// Analysis functions
SonareError sonare_detect_bpm(
  const SonareAudio* audio, float* out_bpm, float* out_confidence
);

SonareError sonare_detect_key(const SonareAudio* audio, SonareKey* out_key);

SonareError sonare_detect_beats(
  const SonareAudio* audio, float** out_times, size_t* out_count
);

SonareError sonare_detect_onsets(
  const SonareAudio* audio, float** out_times, size_t* out_count
);

SonareError sonare_analyze(const SonareAudio* audio, SonareAnalysisResult* out);

// Memory management
void sonare_free_floats(float* ptr);
void sonare_free_result(SonareAnalysisResult* result);

// Utilities
const char* sonare_error_message(SonareError error);
const char* sonare_version(void);
```

---

## Utility Functions

### Unit Conversion

**Header:** `#include <sonare/core/convert.h>`

```cpp
// Hz <-> Mel (Slaney formula, librosa default)
float hz_to_mel(float hz);
float mel_to_hz(float mel);

// Hz <-> Mel (HTK formula)
float hz_to_mel_htk(float hz);
float mel_to_hz_htk(float mel);

// Hz <-> MIDI note number (A4 = 440Hz = 69)
float hz_to_midi(float hz);
float midi_to_hz(float midi);

// Hz <-> Note name
std::string hz_to_note(float hz);     // e.g., "A4", "C#5"
float note_to_hz(const std::string& note);

// Time <-> Frames
float frames_to_time(int frames, int sample_rate, int hop_length);
int time_to_frames(float time, int sample_rate, int hop_length);

// Samples <-> Time
float samples_to_time(int samples, int sample_rate);
int time_to_samples(float time, int sample_rate);

// FFT bin <-> Hz
float bin_to_hz(int bin, int sample_rate, int n_fft);
int hz_to_bin(float hz, int sample_rate, int n_fft);
```

### Window Functions

**Header:** `#include <sonare/core/window.h>`

```cpp
enum class WindowType {
  Hann,
  Hamming,
  Blackman,
  Rectangular
};

std::vector<float> create_window(WindowType type, int length);
std::vector<float> hann_window(int length);
std::vector<float> hamming_window(int length);
std::vector<float> blackman_window(int length);
std::vector<float> rectangular_window(int length);
```

### Math Utilities

**Header:** `#include <sonare/util/math_utils.h>`

```cpp
template<typename T>
T clamp(T value, T min_val, T max_val);

template<typename T>
void normalize_l2(T* data, size_t size);

template<typename T>
size_t argmax(const T* data, size_t size);

template<typename T>
T mean(const T* data, size_t size);

template<typename T>
T variance(const T* data, size_t size);

float cosine_similarity(const float* a, const float* b, size_t size);
float pearson_correlation(const float* a, const float* b, size_t size);

template<typename T>
T median(T* data, size_t size);

template<typename T>
T percentile(T* data, size_t size, float p);
```

---

## Enumerations

```cpp
enum class PitchClass {
  C = 0, Cs = 1, D = 2, Ds = 3, E = 4, F = 5,
  Fs = 6, G = 7, Gs = 8, A = 9, As = 10, B = 11
};

enum class Mode {
  Major,
  Minor
};

enum class ChordQuality {
  Major,
  Minor,
  Diminished,
  Augmented,
  Dominant7,
  Major7,
  Minor7,
  Sus2,
  Sus4
};

enum class SectionType {
  Intro,
  Verse,
  PreChorus,
  Chorus,
  Bridge,
  Instrumental,
  Outro
};

enum class WindowType {
  Hann,
  Hamming,
  Blackman,
  Rectangular
};

enum class ErrorCode {
  Ok,
  FileNotFound,
  InvalidFormat,
  DecodeFailed,
  InvalidParameter,
  OutOfMemory
};
```

---

## Structures

### Key

```cpp
struct Key {
  PitchClass root;
  Mode mode;
  float confidence;  // 0.0 - 1.0

  std::string to_string() const;       // "C major", "A minor"
  std::string short_name() const;      // "C", "Am"
  int relative_key_root() const;       // Root of relative major/minor
  bool is_enharmonic(const Key& other) const;
};
```

### Beat

```cpp
struct Beat {
  float time;      // Time in seconds
  int frame;       // Frame index
  float strength;  // Beat strength (0.0 - 1.0)
};
```

### Chord

```cpp
struct Chord {
  PitchClass root;
  ChordQuality quality;
  float start;       // Start time in seconds
  float end;         // End time in seconds
  float confidence;

  std::string to_string() const;  // "C", "Am", "G7", "Bdim"
  float duration() const;
};
```

### Section

```cpp
struct Section {
  SectionType type;
  float start;
  float end;
  float energy_level;  // Relative energy (0.0 - 1.0)
  float confidence;

  std::string to_string() const;       // "Verse (0.0-15.2s)"
  std::string type_name() const;       // "Verse"
  float duration() const;
};
```

### TimeSignature

```cpp
struct TimeSignature {
  int numerator;    // Beats per measure (e.g., 4)
  int denominator;  // Beat unit (e.g., 4 for quarter note)
  float confidence;

  std::string to_string() const;  // "4/4"
};
```

### Timbre

```cpp
struct Timbre {
  float brightness;   // High-frequency content (0-1)
  float warmth;       // Low-frequency emphasis (0-1)
  float density;      // Spectral density (0-1)
  float roughness;    // Dissonance/inharmonicity (0-1)
  float complexity;   // Spectral complexity (0-1)
};
```

### Dynamics

```cpp
struct Dynamics {
  float dynamic_range_db;   // Peak to RMS range
  float loudness_range_db;  // Short-term loudness variation
  float crest_factor;       // Peak to RMS ratio
  bool is_compressed;       // True if heavily compressed
};
```

### RhythmFeatures

```cpp
struct RhythmFeatures {
  TimeSignature time_signature;
  float syncopation;           // Amount of syncopation (0-1)
  std::string groove_type;     // "straight", "shuffle", "swing"
  float pattern_regularity;    // Rhythmic regularity (0-1)
};
```

### AnalysisResult

```cpp
struct AnalysisResult {
  float bpm;
  float bpm_confidence;
  Key key;
  TimeSignature time_signature;
  std::vector<Beat> beats;
  std::vector<Chord> chords;
  std::vector<Section> sections;
  Timbre timbre;
  Dynamics dynamics;
  RhythmFeatures rhythm;
  std::string form;  // Song form, e.g., "IABABCO"
};
```

---

## Error Handling

libsonare uses exceptions for error handling.

```cpp
class SonareException : public std::exception {
public:
  explicit SonareException(ErrorCode code, const std::string& message);

  ErrorCode code() const;
  const char* what() const noexcept override;
};
```

### Example

```cpp
try {
  auto audio = sonare::Audio::from_file("nonexistent.mp3");
} catch (const sonare::SonareException& e) {
  std::cerr << "Error: " << e.what() << "\n";
  if (e.code() == sonare::ErrorCode::FileNotFound) {
    // Handle file not found
  }
}
```
