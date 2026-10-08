#pragma once

/// @file dereverb_streaming.h
/// @brief The subtraction-mask dereverberator as a realtime insert.
///
/// The mask of a frame reads only that frame and the one the late-reverb lag places behind it,
/// so a stream reaches the offline result: frames sit on the offline centred grid (the first
/// starts half a window before sample zero, over zeros), every frame takes part from the first,
/// and the output is the offline output delayed by `n_fft - 1` samples. WPE fits its predictor
/// over the whole signal and has no streaming form, so it is refused by name.

#include <complex>
#include <cstddef>
#include <vector>

#include "core/fft.h"
#include "mastering/repair/dereverb_classical.h"
#include "rt/processor_base.h"

namespace sonare::mastering::repair {

/// @brief Realtime classical dereverberator with one mask linked across the block's channels.
/// @details The mask is built from the power summed over the channels and applied unchanged to
///   each, as the offline linked path does, so the channel count is fixed at the first process()
///   after a reset and a later block that disagrees is refused. Everything is sized in prepare();
///   process() is in-place and allocates nothing.
class StreamingDereverb : public rt::ProcessorBase {
 public:
  /// @throws SonareException(InvalidParameter) for anything @ref validate_config rejects, and for
  ///   `wpe_enabled`, whose predictor is fitted over the whole signal.
  explicit StreamingDereverb(const DereverbClassicalConfig& config);

  void prepare(double sample_rate, int max_block_size) override;

  /// @throws SonareException(InvalidParameter) for a non-positive rate, a negative block size,
  ///   or a channel count outside [1, 64].
  void prepare(double sample_rate, int max_block_size, int max_channels) override;

  /// @throws SonareException(InvalidState) when not prepared; SonareException(InvalidParameter)
  ///   for a null buffer, a block wider or longer than prepare() allows, or a channel count that
  ///   differs from the one this run started with.
  void process(float* const* channels, int num_channels, int num_samples) override;

  /// @brief Returns every ring and the late-power history to their post-prepare state.
  void reset() override;

  /// @brief `n_fft - 1`: the last frame covering a sample needs the samples after it.
  int latency_samples() const noexcept override;

  /// @brief The delay itself: nothing here decays past it.
  int tail_samples() const noexcept override;

  const DereverbClassicalConfig& config() const noexcept { return config_; }

 private:
  void analyze_frame();
  void finalize_before(std::size_t end);

  DereverbClassicalConfig config_;
  int n_fft_ = 0;
  int hop_length_ = 0;
  int n_bins_ = 0;
  int prefix_ = 0;
  int delay_frames_ = 1;
  double decay_ = 0.0;
  int max_block_size_ = 0;
  int max_channels_ = 0;
  int active_channels_ = 0;
  bool prepared_ = false;

  std::vector<float> analysis_window_;
  std::vector<float> synthesis_window_;
  std::vector<float> window_product_;

  std::vector<float> input_ring_;
  int input_write_ = 0;
  int samples_to_next_frame_ = 0;

  std::vector<float> frame_;
  std::vector<std::complex<float>> spectra_;
  std::vector<std::complex<float>> masked_;
  std::vector<float> power_;
  std::vector<double> gains_;

  // Summed power of the last delay_frames_ frames, oldest first at power_history_read_.
  std::vector<float> power_history_;
  int power_history_read_ = 0;
  std::size_t frames_taken_ = 0;

  std::vector<float> synthesis_ring_;
  std::vector<float> window_sum_ring_;
  std::size_t ring_base_ = 0;
  std::size_t next_frame_start_ = 0;

  std::vector<float> output_queue_;
  int queue_capacity_ = 0;
  int queue_read_ = 0;
  int queue_size_ = 0;

  FFT fft_;
};

}  // namespace sonare::mastering::repair
