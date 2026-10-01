#pragma once

/// @file classic_unit.h
/// @brief One GS insertion-effect unit drawn by the classic graph engine at 32 kHz.
///
/// The host signal is resampled to the models' 32 kHz, drawn through one type's graph,
/// and resampled back, both ways with minimum-phase r8brain filters (20 % transition
/// band, 96 dB). The resamplers emit variable counts per call, so the host side keeps an
/// output FIFO primed in `prepare()` with the deepest shortfall the resampler pair can
/// reach; every block then returns exactly `num_samples`. The round trip is not reported
/// as latency: the classic realization leaves it uncompensated.
///
/// The 20 byte slots are realtime parameters `byte0`..`byte19` (id = slot); a write only
/// re-indexes the expanded LUTs. `prepare()` allocates; `process()` and `set_parameter()`
/// do not.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "midi/synth/gs_classic/graph_engine.h"
#include "midi/synth/gs_classic/model_format.h"
#include "rt/processor_base.h"

namespace r8b {
class CDSPResampler;
}  // namespace r8b

namespace sonare::midi::synth::gs_classic {

class GsClassicUnit : public rt::ProcessorBase {
 public:
  /// `models` (with its LUTs expanded) and `type` must outlive the unit.
  GsClassicUnit(const GsClassicModelSet& models, const GsClassicType& type);
  ~GsClassicUnit() override;
  GsClassicUnit(const GsClassicUnit&) = delete;
  GsClassicUnit& operator=(const GsClassicUnit&) = delete;

  void prepare(double sample_rate, int max_block_size) override;
  void process(float* const* channels, int num_channels, int num_samples) override;
  void reset() override;

  bool set_parameter_impl(unsigned int param_id, float value) override;
  bool parameter_is_realtime_safe(unsigned int param_id) const noexcept override;
  std::vector<rt::ParamDescriptor> parameter_descriptors() const override;

  /// Host rendering allowance for recursive classic effects, in host samples.
  /// This is a fixed ten-second policy cap for non-Thru units, not a measured
  /// decay bound. It remains allocation-free and stable across reset().
  int tail_samples() const noexcept override { return tail_samples_; }

  /// The byte in one of the 20 slots.
  uint8_t byte(std::size_t slot) const noexcept { return graph_.byte(slot); }
  /// Host samples the output FIFO was primed with.
  std::size_t fifo_prefill() const noexcept { return prefill_; }
  /// Blocks in which the output FIFO ran short and was padded with silence.
  uint32_t fifo_underruns() const noexcept { return underruns_; }

 private:
  void prime() noexcept;

  const GsClassicModelSet* models_;
  const GsClassicType* type_;
  GsClassicGraph graph_;
  bool prepared_ = false;
  int tail_samples_ = 0;
  std::size_t max_block_ = 0;
  std::unique_ptr<r8b::CDSPResampler> up_[2];
  std::unique_ptr<r8b::CDSPResampler> down_[2];
  std::vector<double> host_in_[2];
  std::vector<double> drawn_[2];
  std::vector<double> fifo_[2];
  std::size_t fifo_read_ = 0;
  std::size_t fifo_size_ = 0;
  std::size_t prefill_ = 0;
  uint32_t underruns_ = 0;
};

}  // namespace sonare::midi::synth::gs_classic
