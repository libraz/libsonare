#pragma once

/// @file convolution_reverb.h
/// @brief Non-RT IR-loadable FFT partitioned convolution reverb.

#include <memory>
#include <vector>

#include "rt/partitioned_convolver.h"
#include "rt/processor_base.h"

namespace sonare::effects::reverb {

class ConvolutionReverb : public rt::ProcessorBase {
 public:
  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void load_ir(const float* impulse_response, int num_samples);
  void load_ir(const std::vector<float>& impulse_response);

  /// Latency equals the partitioned-convolution block size: input is buffered
  /// until a full partition is available before being processed.
  int latency_samples() const noexcept override { return partition_size_; }
  int ir_size() const noexcept { return static_cast<int>(ir_.size()); }

 private:
  void rebuild_convolvers();

  std::vector<float> ir_;
  int partition_size_ = 0;

  // One convolver per channel; the library targets mono/stereo only.
  std::vector<std::unique_ptr<rt::PartitionedConvolver>> convolvers_;

  // Per-channel input accumulation and processed-output staging buffers, each
  // sized to one partition. Filled in prepare()/load_ir() so process() never
  // allocates on the audio thread.
  std::vector<std::vector<float>> block_input_;
  std::vector<std::vector<float>> block_output_;
  std::vector<int> fill_count_;
};

}  // namespace sonare::effects::reverb
