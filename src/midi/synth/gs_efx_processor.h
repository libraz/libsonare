#pragma once

/// @file gs_efx_processor.h
/// @brief A GS EFX unit usable as a normal realtime audio processor.
///
/// The MIDI player and this facade use the same realised unit graph.  The
/// facade keeps the GS wire bytes as its construction state and exposes only
/// the parameter bytes as realtime automation; changing the type or the
/// realization requires constructing a new processor.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "midi/synth/gs_efx_graph.h"
#include "util/constants.h"

namespace sonare::midi::synth {

/// Construction policy for missing modern stages.  Sf2Player keeps the stage
/// position when its injected factory cannot build a processor; a standalone
/// audio insert must reject that partial graph instead of silently passing dry.
enum class GsEfxBuildPolicy : uint8_t {
  kRequireCompleteGraph,
  kAllowUnavailableStage,
};

/// One GS EFX unit over a regular planar audio buffer.
///
/// The processor accepts one or two channels.  Mono is processed as a stereo
/// signal with the same input on both legs and folded back to mono.  More than
/// two channels are rejected; callers that need a multichannel graph should
/// instantiate one unit per stereo pair.
class GsEfxProcessor final : public rt::ProcessorBase {
 public:
  /// Construct from a fully resolved GS state.  All 20 bytes are applied while
  /// constructing the stage JSON and before prepare(); realtime setters are
  /// only used for subsequent supported byte updates.
  GsEfxProcessor(const GsEfx& state, GsEfxRealization realization, GsEfxStageFactory factory,
                 GsEfxBuildPolicy policy = GsEfxBuildPolicy::kRequireCompleteGraph);

  /// Construct a type from its measured power-on defaults.
  explicit GsEfxProcessor(uint16_t type, GsEfxRealization realization = GsEfxRealization::kModern,
                          GsEfxStageFactory factory = {},
                          GsEfxBuildPolicy policy = GsEfxBuildPolicy::kRequireCompleteGraph);

  ~GsEfxProcessor() override = default;
  GsEfxProcessor(const GsEfxProcessor&) = delete;
  GsEfxProcessor& operator=(const GsEfxProcessor&) = delete;

  uint16_t type() const noexcept { return state_.type; }
  GsEfxRealization realization() const noexcept { return realization_; }
  const GsEfx& state() const noexcept { return state_; }

  void prepare(double sample_rate, int max_block_size) override;
  void prepare(double sample_rate, int max_block_size, int max_channels) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;
  int latency_samples() const noexcept override;
  int latency_samples_q8() const noexcept override;
  int tail_samples() const noexcept override;

  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;
  /// The EFX type: each byte position means something different per type.
  uint64_t parameter_layout_variant() const noexcept override { return state_.type; }

 protected:
  bool set_parameter_impl(unsigned int param_id, float value) override;

 private:
  struct ParamDestination {
    uint8_t stage_index = 0xFF;
    uint32_t param_id = 0;
    GsEfxBindingRow row{};
  };
  struct EnableDestination {
    GsEfxEnable rule{};
    GsEfxEnableStageIndices stage_indices = kGsEfxUnmappedStageIndices;
  };

  void build_parameter_plan();
  void apply_enable_state(bool reset_fade = false) noexcept;
  bool validate_channels(int num_channels) const noexcept;

  GsEfx state_{};
  GsEfxRealization realization_ = GsEfxRealization::kModern;
  GsEfxBuildPolicy policy_ = GsEfxBuildPolicy::kRequireCompleteGraph;
  GsEfxStageFactory factory_{};
  Sf2EfxUnitRt unit_{};
  std::array<std::array<ParamDestination, 8>, 20> destinations_{};
  std::array<uint8_t, 20> destination_count_{};
  std::array<EnableDestination, 32> enables_{};
  std::array<bool, 20> enable_slots_{};
  uint8_t enable_count_ = 0;
  double sample_rate_ = sonare::constants::kDefaultDawSampleRate;
  int max_block_size_ = 0;
  int max_channels_ = 2;
  std::vector<float> mono_right_;
  bool prepared_ = false;
};

}  // namespace sonare::midi::synth
