#pragma once

/// @file convolution_reverb.h
/// @brief Non-RT IR-loadable FFT partitioned convolution reverb.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "rt/partitioned_convolver.h"
#include "rt/processor_base.h"
#include "rt/stage_gate.h"
#include "rt/tail_budget.h"

namespace sonare::effects::reverb {

/// @brief Parameters for the algorithmic default impulse response.
///
/// When no explicit IR is loaded via load_ir(), prepare() synthesizes a short
/// exponentially-decaying noise IR from these fields so the convolution reverb
/// produces an actual room-like tail from scalar params, matching its sibling
/// algorithmic reverbs (effects.reverb.fdn / .velvet). Supplying an explicit IR
/// via load_ir() overrides this synthesis entirely.
struct ConvolutionReverbConfig {
  /// Upper bound on the synthesized IR length so prepare() stays cheap and the
  /// convolver never allocates an unbounded partition set. Requests above this
  /// resolve to the ceiling. Shared with the insert factory so an out-of-range
  /// {decaySec} is clamped at construction rather than silently at prepare().
  static constexpr float kMaxDecaySeconds = 12.0f;
  /// Approximate RT60 tail length in seconds (matched to FDN/velvet, where
  /// decaySec maps to the ~T60 reverberation time). Clamped to
  /// [0, kMaxDecaySeconds].
  float decay_sec = 1.5f;
  /// Pre-delay before the synthesized tail begins, in milliseconds.
  float pre_delay_ms = 0.0f;
  /// Dry/wet mix. 1.0 = fully wet (convolution only); 0.0 = dry passthrough.
  float dry_wet = 0.35f;
  /// Deterministic seed for the decaying-noise IR (so output is reproducible).
  std::uint32_t seed = 0x5151ABCDu;
};

/// @brief FFT partitioned convolution reverb.
///
/// Every channel is convolved with the same impulse response — the one loaded
/// via load_ir(), or the synthesized decaying-noise IR otherwise. This is
/// by design: a convolution reverb reproduces the supplied IR faithfully, so a
/// mono IR yields an inter-channel-correlated (mono) tail. Stereo width must
/// come from supplying a genuinely decorrelated multichannel IR; this processor
/// does not synthesize per-channel decorrelation (unlike an algorithmic reverb,
/// where decorrelation is a property of the algorithm rather than the input).
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

  /// @brief Load an impulse response scaled to unit energy.
  ///
  /// The level contract of `dryWet`: one parameter value must mean the same
  /// audible mix depth on every engine that mixes a convolution in. The
  /// synthesized default IR is normalized to unit energy for exactly that
  /// reason, so an IR whose amplitude carries a physical scale — a
  /// geometry-derived RIR is attenuated by 1/(4*pi*d), around 20 dB down at a
  /// few metres — must be normalized the same way before it is mixed,
  /// or the same `dryWet` reads far quieter than on the sibling reverbs. Offline
  /// RIR consumers that need the physical scale keep it by using load_ir().
  ///
  /// Throws ErrorCode::InvalidParameter on an invalid buffer (as load_ir does)
  /// and on an IR with no energy at all, which has no unit-energy form and would
  /// render as digital silence rather than as an audible room.
  void load_ir_unit_energy(const float* impulse_response, int num_samples);

  /// @brief Load a mono IR plus a left/right IR pair.
  ///
  /// The mono IR is normalized exactly as load_ir_unit_energy() does. The pair is scaled by
  /// one common factor so (E_left + E_right) / 2 == 1. process() with one channel runs the
  /// mono IR; with two or more channels the first two run left/right. Each path keeps its own
  /// state, so changing the channel count mid-stream switches path and cuts the tail.
  /// load_ir() / load_ir_unit_energy() discard the pair. Throws ErrorCode::InvalidParameter
  /// when any of the three is empty or silent.
  void load_ir_set_unit_energy(const std::vector<float>& mono, const std::vector<float>& left,
                               const std::vector<float>& right);

  // Automatable parameters (RT-safe, no allocation, no state reset):
  //   0 = dry_wet (clamped to [0, 1] in process())
  bool set_parameter_impl(unsigned int param_id, float value) override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// Latency equals the partitioned-convolution block size: input is buffered
  /// until a full partition is available before being processed. An empty IR
  /// makes process() a true passthrough (no buffering), so it reports no latency.
  int latency_samples() const noexcept override {
    return (ir_.empty() || partition_size_ <= 0) ? 0 : partition_size_;
  }
  /// After the input goes silent the convolver keeps emitting the impulse
  /// response, so the decay tail equals the loaded/synthesized IR length. A
  /// dry-only configuration has processing latency but no audible decay tail.
  int tail_samples() const noexcept override {
    rt::TailBudget tail;
    if (std::clamp(dry_wet_, 0.0f, 1.0f) > 0.0f) tail.delay(static_cast<double>(max_ir_length()));
    return tail.samples();
  }
  int ir_size() const noexcept { return static_cast<int>(max_ir_length()); }

  /// Skips implicit noise-IR construction in prepare() when a caller will
  /// synchronously provide an explicit IR with load_ir().
  void suppress_default_ir_synthesis() noexcept { explicit_ir_ = true; }

 private:
  void rebuild_convolvers();
  std::size_t max_ir_length() const noexcept {
    return std::max({ir_.size(), ir_left_.size(), ir_right_.size()});
  }
  // Run one channel through its convolver and staging buffers.
  void process_channel(rt::PartitionedConvolver& convolver, std::vector<float>& in_block,
                       std::vector<float>& out_block, int& fill, float* data, int num_samples,
                       float dry, float wet);
  // Synthesize a decaying-noise IR from config_ at the prepared sample rate.
  // Used only when no explicit IR was supplied via load_ir().
  void synthesize_default_ir(double sample_rate);
  // Validate the caller's buffer and copy it into ir_. Shared by both loaders so
  // they reject the same inputs and the convolvers are rebuilt exactly once.
  void store_ir(const float* impulse_response, int num_samples);
  // Scale ir_ so its samples sum to unit energy. Returns false for an IR with no
  // energy (nothing to normalize toward).
  bool normalize_ir_unit_energy();

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

  // Left/right IR pair and its dedicated convolvers/buffers (empty when no pair is loaded).
  std::vector<float> ir_left_;
  std::vector<float> ir_right_;
  std::vector<std::unique_ptr<rt::PartitionedConvolver>> pair_convolvers_;
  std::vector<std::vector<float>> pair_input_;
  std::vector<std::vector<float>> pair_output_;
  std::vector<int> pair_fill_;
  rt::StageGate stage_;
};

}  // namespace sonare::effects::reverb
