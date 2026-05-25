#pragma once

/// @file partitioned_convolver.h
/// @brief Uniform partitioned FIR convolution using overlap-save FFT blocks.

#include <complex>
#include <memory>
#include <vector>

#include "core/fft.h"

namespace sonare::rt {

struct PartitionedConvolverConfig {
  /// Processing block size and FIR partition size in samples.
  int partition_size = 256;
};

class PartitionedConvolver {
 public:
  explicit PartitionedConvolver(PartitionedConvolverConfig config = {});

  void set_impulse_response(const float* impulse_response, int num_samples);
  void set_impulse_response(const std::vector<float>& impulse_response);
  void reset();

  /// Processes exactly partition_size() samples.
  /// @note Real-time safe: never throws and never allocates. If either pointer
  ///   is null the output block is zero-filled (when non-null) and the call
  ///   returns without touching internal state.
  void process_block(const float* input, float* output) noexcept;

  int partition_size() const noexcept { return config_.partition_size; }
  int fft_size() const noexcept { return fft_size_; }
  int num_partitions() const noexcept { return static_cast<int>(ir_partitions_.size()); }
  bool empty() const noexcept { return ir_partitions_.empty(); }

 private:
  void validate_config() const;
  void rebuild_fft();

  PartitionedConvolverConfig config_{};
  int fft_size_ = 0;
  std::unique_ptr<sonare::FFT> fft_;

  std::vector<std::vector<std::complex<float>>> ir_partitions_;

  // Uniform-partitioned overlap-save (UPOLS) state. A ring of past input
  // spectra is multiply-accumulated against the IR partition spectra in the
  // frequency domain, requiring only a single inverse FFT per block.
  std::vector<std::vector<std::complex<float>>> input_spectrum_ring_;
  int ring_pos_ = 0;

  std::vector<float> ola_buffer_;
  std::vector<float> fft_input_;
  std::vector<float> fft_output_;
  std::vector<std::complex<float>> current_spectrum_;
  std::vector<std::complex<float>> accum_spectrum_;
};

}  // namespace sonare::rt
