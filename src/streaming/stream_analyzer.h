#pragma once

/// @file stream_analyzer.h
/// @brief Streaming audio analyzer for real-time visualization.

#include <array>
#include <complex>
#include <deque>
#include <memory>
#include <vector>

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

  /// @brief Returns current statistics and progressive estimate.
  AnalyzerStats stats() const;

  /// @brief Returns configuration.
  const StreamConfig& config() const { return config_; }

  /// @brief Returns total frames processed.
  int frame_count() const { return frame_count_; }

  /// @brief Returns current time position (seconds).
  float current_time() const;

 private:
  StreamConfig config_;

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
  std::vector<float> chroma_buffer_;             // [12]

  // Progressive estimation accumulators
  std::vector<float> onset_accumulator_;
  std::array<float, 12> chroma_sum_;
  int chroma_frame_count_ = 0;
  float last_key_update_time_ = 0.0f;
  float last_bpm_update_time_ = 0.0f;
  ProgressiveEstimate current_estimate_;

  // Internal methods
  void process_internal(const float* samples, size_t n_samples);
  StreamFrame process_single_frame(const float* frame_start, size_t sample_offset);
  void compute_stft(const float* frame_start);
  void compute_mel();
  void compute_chroma();
  float compute_onset();
  void compute_spectral_features(StreamFrame& frame);
  void update_progressive_estimate(float current_time);
};

}  // namespace sonare
