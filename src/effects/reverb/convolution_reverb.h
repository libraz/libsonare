#pragma once

/// @file convolution_reverb.h
/// @brief Non-RT IR-loadable FFT partitioned convolution reverb.

#include <cstdint>
#include <memory>
#include <vector>

#include "rt/partitioned_convolver.h"
#include "rt/processor_base.h"

namespace sonare::effects::reverb {

/// @brief Parameters for the algorithmic default impulse response.
///
/// When no explicit IR is loaded via load_ir(), prepare() synthesizes a short
/// exponentially-decaying noise IR from these fields so the convolution reverb
/// produces an actual room-like tail from scalar params, matching its sibling
/// algorithmic reverbs (effects.reverb.fdn / .velvet). Supplying an explicit IR
/// via load_ir() overrides this synthesis entirely.
struct ConvolutionReverbConfig {
  /// Approximate RT60 tail length in seconds (matched to FDN/velvet, where
  /// decaySec maps to the ~T60 reverberation time). Clamped to a sane range.
  float decay_sec = 1.5f;
  /// Pre-delay before the synthesized tail begins, in milliseconds.
  float pre_delay_ms = 0.0f;
  /// Dry/wet mix. 1.0 = fully wet (convolution only); 0.0 = dry passthrough.
  float dry_wet = 0.35f;
  /// Deterministic seed for the decaying-noise IR (so output is reproducible).
  std::uint32_t seed = 0x5151ABCDu;
};

class ConvolutionReverb : public rt::ProcessorBase {
 public:
  ConvolutionReverb() = default;
  explicit ConvolutionReverb(ConvolutionReverbConfig config) : config_(config) {
    dry_wet_ = config.dry_wet;
  }

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  void load_ir(const float* impulse_response, int num_samples);
  void load_ir(const std::vector<float>& impulse_response);

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = dry_wet (clamped to [0, 1] in process())
  bool set_parameter(unsigned int param_id, float value) override;

  /// Latency equals the partitioned-convolution block size: input is buffered
  /// until a full partition is available before being processed.
  int latency_samples() const noexcept override { return partition_size_; }
  int ir_size() const noexcept { return static_cast<int>(ir_.size()); }

 protected:
  void suppress_default_ir_synthesis() noexcept { explicit_ir_ = true; }

 private:
  void rebuild_convolvers();
  // Synthesize a decaying-noise IR from config_ at the prepared sample rate.
  // Used only when no explicit IR was supplied via load_ir().
  void synthesize_default_ir(double sample_rate);

  ConvolutionReverbConfig config_{};
  // True once load_ir() supplies caller IR samples; suppresses default synthesis.
  bool explicit_ir_ = false;
  std::vector<float> ir_;
  int partition_size_ = 0;
  // Dry/wet mix. 1.0 = fully wet (convolution only); 0.0 = dry passthrough.
  // A default-constructed (no-config) ConvolutionReverb is fully wet so a direct
  // user who load_ir()s their own IR gets the pure convolution; the insert/scene
  // path supplies its own mix via ConvolutionReverbConfig::dry_wet (0.35).
  float dry_wet_ = 1.0f;

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
