#pragma once

/// @file dehum_streaming.h
/// @brief The dehummer as a realtime insert.
///
/// The fixed notch cascade is causal and reaches the offline result with no delay. The adaptive
/// pass estimates each `frame_size` block's fundamental before filtering it, so the insert holds
/// one block and reaches the offline result `frame_size` samples late. The fixed subtract mode
/// starts its cancellers at zero: the offline pass seeds them from a projection over the first
/// second, which a stream does not have yet, so the two agree once the cancellers converge.
/// Each channel is processed on its own, as the mono offline pass processes it.

#include <memory>

#include "mastering/repair/dehum.h"
#include "rt/processor_base.h"

namespace sonare::mastering::repair {

/// @brief Realtime dehummer, each channel tracked and filtered on its own.
/// @details Everything is sized in prepare(); process() is in-place and allocates nothing.
class StreamingDehum : public rt::ProcessorBase {
 public:
  /// @throws SonareException(InvalidParameter) for anything @ref validate_config rejects.
  explicit StreamingDehum(const DehumConfig& config);
  ~StreamingDehum() override;

  void prepare(double sample_rate, int max_block_size) override;

  /// @throws SonareException(InvalidParameter) for a non-positive rate, a negative block size,
  ///   or a channel count outside [1, 64].
  void prepare(double sample_rate, int max_block_size, int max_channels) override;

  /// @throws SonareException(InvalidState) when not prepared; SonareException(InvalidParameter)
  ///   for a null buffer or a block wider or longer than prepare() allows.
  void process(float* const* channels, int num_channels, int num_samples) override;

  void reset() override;

  /// @brief `frame_size` when adaptive, otherwise 0.
  int latency_samples() const noexcept override;

  const DehumConfig& config() const noexcept { return config_; }

 private:
  struct State;

  DehumConfig config_;
  std::unique_ptr<State> state_;
};

}  // namespace sonare::mastering::repair
