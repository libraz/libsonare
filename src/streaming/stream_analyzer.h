#pragma once

/// @file stream_analyzer.h
/// @brief Streaming audio analyzer for real-time visualization.

#include <array>
#include <complex>
#include <deque>
#include <memory>
#include <vector>

#include "analysis/chord_templates.h"
#include "streaming/stream_config.h"
#include "streaming/stream_frame.h"

namespace sonare {

// Forward declarations
class FFT;

/// @brief Streaming audio analyzer for real-time visualization.
/// @details Processes audio in chunks, maintaining overlap state between calls.
/// Produces StreamFrame objects with timestamp and features.
///
/// Timestamps represent "stream time" (input sample position), not necessarily
/// AudioContext.currentTime. See documentation for synchronization guidance.
///
/// Usage:
/// @code
///   StreamAnalyzer analyzer(config);
///
///   // In AudioWorklet.process():
///   analyzer.process(samples, n_samples);
///
///   // In main thread (via postMessage):
///   auto frames = analyzer.read_frames(10);
///   for (const auto& frame : frames) {
///     visualize(frame);
///   }
/// @endcode
class StreamAnalyzer {
 public:
  /// @brief Constructs analyzer with configuration.
  /// @param config Stream configuration
  explicit StreamAnalyzer(const StreamConfig& config);

  ~StreamAnalyzer();

  // Non-copyable, movable
  StreamAnalyzer(const StreamAnalyzer&) = delete;
  StreamAnalyzer& operator=(const StreamAnalyzer&) = delete;
  StreamAnalyzer(StreamAnalyzer&&) noexcept;
  StreamAnalyzer& operator=(StreamAnalyzer&&) noexcept;

  /// @brief Processes audio chunk (internal offset tracking).
  /// @param samples Input samples
  /// @param n_samples Number of samples
  /// @details Internally tracks cumulative sample count for timestamp calculation.
  void process(const float* samples, size_t n_samples);

  /// @brief Processes audio chunk (external offset synchronization).
  /// @param samples Input samples
  /// @param n_samples Number of samples
  /// @param sample_offset Cumulative sample count at start of this chunk
  /// @details Use this overload when you need precise synchronization with
  ///          an external timeline (e.g., AudioContext).
  void process(const float* samples, size_t n_samples, size_t sample_offset);

  /// @brief Returns number of frames available to read.
  size_t available_frames() const;

  /// @brief Reads processed frames from internal buffer.
  /// @param max_frames Maximum number of frames to read
  /// @return Vector of frames (up to max_frames, may be empty)
  /// @details Frames are consumed from internal buffer after reading.
  std::vector<StreamFrame> read_frames(size_t max_frames);

  /// @brief Reads processed frames into SOA buffer.
  /// @param max_frames Maximum number of frames to read
  /// @param buffer Output buffer (cleared and filled)
  /// @details More efficient for WASM/postMessage transfer.
  void read_frames_soa(size_t max_frames, FrameBuffer& buffer);

  /// @brief Reads processed frames into quantized 8-bit buffer.
  /// @param max_frames Maximum number of frames to read
  /// @param buffer Output buffer (cleared and filled)
  /// @param qconfig Quantization configuration
  /// @details Reduces bandwidth by 4x compared to Float32.
  void read_frames_quantized_u8(size_t max_frames, QuantizedFrameBufferU8& buffer,
                                 const QuantizeConfig& qconfig = QuantizeConfig());

  /// @brief Reads processed frames into quantized 16-bit buffer.
  /// @param max_frames Maximum number of frames to read
  /// @param buffer Output buffer (cleared and filled)
  /// @param qconfig Quantization configuration
  /// @details Reduces bandwidth by 2x compared to Float32, higher precision than U8.
  void read_frames_quantized_i16(size_t max_frames, QuantizedFrameBufferI16& buffer,
                                  const QuantizeConfig& qconfig = QuantizeConfig());

  /// @brief Resets analyzer state for new stream.
  /// @param base_sample_offset Starting sample offset (default 0)
  void reset(size_t base_sample_offset = 0);

  /// @brief Sets expected total duration of the audio.
  /// @param duration_seconds Total duration in seconds
  /// @details Used to calculate optimal pattern lock timing.
  ///          If not set, a default threshold is used.
  void set_expected_duration(float duration_seconds);

  /// @brief Sets normalization gain for loud audio.
  /// @param gain Gain factor to apply to input samples (e.g., 0.5 for -6dB)
  /// @details Use this to normalize loud/compressed audio before analysis.
  ///          Typical usage: compute peak or RMS from AudioBuffer, then set
  ///          gain = target_level / measured_level.
  void set_normalization_gain(float gain);

  /// @brief Sets tuning reference frequency and recreates chroma filterbank.
  /// @param ref_hz Reference frequency for A4 (default 440 Hz)
  /// @details Use this when the audio has non-standard tuning.
  ///          For example, if the audio is 1 semitone sharp (A4 = 466.16 Hz),
  ///          pass ref_hz = 466.16f to correct the chroma analysis.
  ///          Should be called before processing audio (after reset if needed).
  void set_tuning_ref_hz(float ref_hz);

  /// @brief Returns current statistics and progressive estimate.
  AnalyzerStats stats();

  /// @brief Returns configuration.
  const StreamConfig& config() const { return config_; }

  /// @brief Returns total frames processed.
  int frame_count() const { return frame_count_; }

  /// @brief Returns current time position (seconds).
  float current_time() const;

 private:
  StreamConfig config_;

  // Internal sample rate for analysis (downsample if input is higher)
  // Using 44100 Hz internally ensures consistent results regardless of input sample rate
  static constexpr int kInternalSampleRate = 44100;
  static constexpr int kMaxDirectSampleRate = 44100;  // Resample anything above 44100 Hz
  int internal_sample_rate_;  // Actual rate used for analysis
  bool needs_resampling_ = false;
  float resample_ratio_ = 1.0f;
  std::vector<float> resample_buffer_;  // Buffer for resampled audio

  // Normalization
  float normalization_gain_ = 1.0f;  // Gain factor for loud audio

  // Cumulative state
  size_t cumulative_samples_ = 0;
  int frame_count_ = 0;
  int emitted_frame_count_ = 0;  // For emit_every_n_frames

  // Overlap buffer (stores last n_fft - hop_length samples)
  std::vector<float> overlap_buffer_;

  // Output ring buffer
  std::deque<StreamFrame> output_buffer_;

  // FFT processor (reusable)
  std::unique_ptr<FFT> fft_;

  // Cached window function
  std::vector<float> window_;

  // Pre-computed filterbanks
  std::vector<float> mel_filterbank_;     // [n_mels x n_bins]
  std::vector<float> chroma_filterbank_;  // [12 x n_bins]

  // Chord templates for chord detection
  std::vector<ChordTemplate> chord_templates_;

  // Frequency array for spectral features
  std::vector<float> frequencies_;

  // Previous frame state (for onset detection)
  std::vector<float> prev_mel_log_;
  bool has_prev_frame_ = false;

  // Working buffers (reused to avoid allocation)
  std::vector<float> frame_buffer_;              // [n_fft]
  std::vector<std::complex<float>> spectrum_;    // [n_bins]
  std::vector<float> magnitude_;                 // [n_bins]
  std::vector<float> power_;                     // [n_bins]
  std::vector<float> mel_buffer_;                // [n_mels]
  std::vector<float> mel_log_;                   // [n_mels]
  std::vector<float> chroma_buffer_;             // [12] - L2 normalized
  std::array<float, 12> chroma_raw_;             // [12] - raw (unnormalized) for accumulation

  // Progressive estimation accumulators
  std::vector<float> onset_accumulator_;
  std::array<float, 12> chroma_sum_;
  int chroma_frame_count_ = 0;
  float last_key_update_time_ = 0.0f;
  float last_bpm_update_time_ = 0.0f;
  float last_chord_analysis_time_ = 0.0f;
  ProgressiveEstimate current_estimate_;

  // Accumulated chroma frames for batch-style chord analysis
  // Stored as [12 * n_frames] (row-major: chroma bins × frames)
  std::vector<float> accumulated_chroma_;

  // Chord progression tracking
  int prev_chord_root_ = -1;
  int prev_chord_quality_ = -1;
  float chord_stable_time_ = 0.0f;        ///< Time chord has been stable
  static constexpr float kChordMinDuration = 0.3f;  ///< Min duration to register change
  static constexpr int kChordSmoothingFrames = 12;  ///< Number of frames to smooth (~0.25s at default settings)
  static constexpr float kChordConfidenceThreshold = 0.5f;  ///< Min correlation for chord detection
  std::deque<std::array<float, 12>> chroma_history_;  ///< History for chord smoothing

  // Bar-synchronized chord tracking (requires stable BPM)
  static constexpr float kBpmConfidenceThreshold = 0.3f;  ///< Min BPM confidence for bar sync
  static constexpr int kBeatsPerBar = 4;  ///< Beats per bar (4/4 time signature)
  bool bar_tracking_active_ = false;      ///< True when BPM is stable enough
  float bar_duration_ = 0.0f;             ///< Duration of one bar in seconds
  int current_bar_index_ = -1;            ///< Current bar index (0-based)
  float bar_start_time_ = 0.0f;           ///< Start time of current bar
  std::array<float, 12> bar_chroma_sum_;  ///< Accumulated chroma within current bar
  int bar_chroma_count_ = 0;              ///< Number of frames accumulated in current bar

  // Chord voting within bar (alternative to chroma averaging)
  std::array<int, 48> bar_chord_votes_;   ///< Vote counts per chord (12 roots × 4 qualities)
  int bar_vote_count_ = 0;                ///< Total votes in current bar

  // Pattern locking (once detected with high confidence, don't change)
  bool pattern_locked_ = false;           ///< True if pattern is locked
  float expected_duration_ = 0.0f;        ///< Expected total duration (0 = unknown)

  // Full chroma history for retroactive bar chord detection
  static constexpr size_t kMaxChromaHistoryFrames = 3000;  ///< ~35s at default settings
  std::vector<std::array<float, 12>> full_chroma_history_;  ///< All chroma vectors

  // Known chord progression patterns (degree, quality pairs)
  // degree: 0=I, 2=II, 4=III, 5=IV, 7=V, 9=VI, 11=VII
  struct ProgressionPattern {
    std::string name;
    std::vector<std::pair<int, int>> chords;  ///< (degree, quality) pairs
  };
  static const std::vector<ProgressionPattern>& get_known_patterns();

  // Internal methods
  void compute_retroactive_bar_chords();
  void compute_voted_pattern(int pattern_length = 4);
  void correct_voted_pattern_by_known_patterns();
  void detect_progression_pattern();
  void process_internal(const float* samples, size_t n_samples);
  StreamFrame process_single_frame(const float* frame_start, size_t sample_offset);
  void compute_stft(const float* frame_start);
  void compute_mel();
  void compute_chroma();
  float compute_onset();
  void compute_spectral_features(StreamFrame& frame);
  void update_progressive_estimate(float current_time);
  void update_bar_chord_tracking(float current_time);
};

}  // namespace sonare
