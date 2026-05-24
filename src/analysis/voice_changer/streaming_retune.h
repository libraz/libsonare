#pragma once

/// @file streaming_retune.h
/// @brief Real-time grain overlap-add (SOLA-style) streaming pitch shifter.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonare::analysis::voice_changer {

struct StreamingRetuneConfig {
  float semitones = 0.0f;
  float mix = 1.0f;
};

/// @brief Low-latency streaming pitch shifter with cross-block continuity.
/// @details Uses fixed-size Hann-windowed grains, overlap-added at the analysis
///          hop. Each grain is resampled from a history ring buffer with a
///          fractional read increment equal to the pitch ratio, so positive
///          semitones read source faster and raise pitch. All state (ring
///          buffer, write head, OLA accumulators, drain/synthesis positions)
///          persists across process_block calls. Allocation happens only in
///          prepare().
class StreamingRetune {
 public:
  explicit StreamingRetune(StreamingRetuneConfig config = {});

  void prepare(double sample_rate, int max_block_size);
  void reset();
  void set_config(const StreamingRetuneConfig& config);
  const StreamingRetuneConfig& config() const noexcept { return config_; }

  void process_block(const float* input, float* output, int num_samples);

 private:
  void update_ratio() noexcept;
  void emit_grain() noexcept;
  float read_ring_linear(double position) const noexcept;

  StreamingRetuneConfig config_{};
  double sample_rate_ = 0.0;
  int max_block_size_ = 0;

  int grain_size_ = 0;        ///< Grain length in samples.
  int hop_a_ = 0;             ///< Analysis/synthesis hop (grain_size / 4).
  std::size_t ring_cap_ = 0;  ///< History ring capacity (4 * grain_size).
  std::size_t accum_cap_ = 0;  ///< OLA accumulator capacity (2 * grain_size).

  double pitch_ratio_ = 1.0;

  std::vector<float> window_;     ///< Precomputed Hann window (grain_size).
  std::vector<float> ring_buf_;   ///< History ring buffer (ring_cap).
  std::vector<float> synth_acc_;  ///< Circular OLA signal accumulator.
  std::vector<float> norm_acc_;   ///< Circular OLA window^2 accumulator.

  std::uint64_t write_head_ = 0;  ///< Total samples written to ring.
  int input_phase_ = 0;           ///< Counts samples until next grain emit.
  std::size_t drain_pos_ = 0;     ///< Front of OLA accumulator (output tap).
  std::size_t synth_pos_ = 0;     ///< Current grain write position in OLA.
};

}  // namespace sonare::analysis::voice_changer
