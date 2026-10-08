#pragma once

/// @file decrackle_streaming.h
/// @brief The median decrackle as a realtime insert.
///
/// The median mode decides each sample from itself and its two neighbours, so a stream reaches
/// the offline result one sample late: the output at sample n is the offline output at n - 1.
/// The wavelet mode shrinks a transform of the whole signal and has no streaming form, so it is
/// refused by name rather than replaced.

#include <cstddef>
#include <vector>

#include "mastering/repair/decrackle.h"
#include "rt/processor_base.h"

namespace sonare::mastering::repair {

/// @brief Realtime median decrackle, each channel on its own.
/// @details Everything is sized in prepare(); process() is in-place and allocates nothing.
class StreamingDecrackle : public rt::ProcessorBase {
 public:
  /// @throws SonareException(InvalidParameter) for anything @ref validate_config rejects, and for
  ///   DecrackleMode::WaveletShrinkage, which transforms the whole signal.
  explicit StreamingDecrackle(const DecrackleConfig& config);

  void prepare(double sample_rate, int max_block_size) override;

  /// @throws SonareException(InvalidParameter) for a non-positive rate, a negative block size,
  ///   or a channel count outside [1, 64].
  void prepare(double sample_rate, int max_block_size, int max_channels) override;

  /// @throws SonareException(InvalidState) when not prepared; SonareException(InvalidParameter)
  ///   for a null buffer or more channels than prepare() was told to expect.
  void process(float* const* channels, int num_channels, int num_samples) override;

  void reset() override;

  /// @brief One sample: a sample's verdict needs the sample after it.
  int latency_samples() const noexcept override { return 1; }

  const DecrackleConfig& config() const noexcept { return config_; }

 private:
  DecrackleConfig config_;
  int max_channels_ = 0;
  bool prepared_ = false;
  std::size_t samples_seen_ = 0;
  std::vector<float> before_;  ///< Per channel, the input two samples back.
  std::vector<float> held_;    ///< Per channel, the input one sample back, awaiting its verdict.
};

}  // namespace sonare::mastering::repair
