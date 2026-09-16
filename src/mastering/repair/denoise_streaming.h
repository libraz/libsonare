#pragma once

/// @file denoise_streaming.h
/// @brief Block-based front end for the classical denoiser.
///
/// The offline denoiser sees the whole signal; this one sees one host block at
/// a time and reaches the same gain math through detail::GainStage. What it
/// cannot inherit is the offline analysis geometry: framing is uncentred,
/// because a stream has no centre to pad, and the noise estimator must be
/// recursive, which is why Quantile is refused rather than substituted.
///
/// The uncentred framing is what the minimum-tracking estimators see first. The
/// offline path's centred padding makes frame 0 mostly zeros, so its floor seeds
/// low; a stream opened mid-programme seeds it at programme level and holds it
/// for the half second the minimum window spans, over-suppressing until then.
/// Prepending that much noise-only audio reproduces the offline result exactly.
/// Spp tracks no minimum and is unaffected.

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

#include "core/fft.h"
#include "mastering/common/noise_tracker.h"
#include "mastering/repair/denoise_classical.h"
#include "mastering/repair/denoise_internal.h"
#include "rt/processor_base.h"

namespace sonare::mastering::repair {

/// @brief Realtime classical denoiser: STFT analysis, one channel-linked gain
///        mask, overlap-add synthesis.
/// @details The mask is built from the power summed over the block's channels
///   and applied unchanged to every one of them, as the offline linked path
///   does, so the processing cannot move an interchannel level or phase
///   difference. That makes the channel count part of the analysis rather than
///   a per-channel loop bound: it is fixed at the first process() after a reset
///   and a later block that disagrees is refused by name.
///
///   process() is in-place and reports the block it was handed delayed by
///   @ref latency_samples; until the pipeline has filled that delay is written
///   as zeros. Everything is sized in prepare(), and process() allocates
///   nothing. reset() rebuilds the gain stage, so it allocates and belongs on
///   the control thread beside prepare().
class StreamingDenoise : public rt::ProcessorBase {
 public:
  /// @brief Binds the processor to @p config.
  /// @throws SonareException(InvalidParameter) for anything
  ///   @ref validate_config rejects, and for
  ///   `noise_estimator == DenoiseNoiseEstimator::Quantile`, which ranks the
  ///   frames of a whole signal and so cannot exist in a stream.
  explicit StreamingDenoise(const DenoiseClassicalConfig& config);

  /// @brief Sizes for the realtime channel maximum.
  void prepare(double sample_rate, int max_block_size) override;

  /// @brief Sizes per-channel scratch for at most @p max_channels channels.
  /// @throws SonareException(InvalidParameter) for a non-positive rate, a
  ///   negative block size, or a channel count outside [1, 64].
  void prepare(double sample_rate, int max_block_size, int max_channels) override;

  /// @brief Denoises one block in place.
  /// @throws SonareException(InvalidState) when the processor is not prepared;
  ///   SonareException(InvalidParameter) for a null buffer, a block wider or
  ///   longer than prepare() was told to expect, or a channel count differing
  ///   from the one this run started with.
  void process(float* const* channels, int num_channels, int num_samples) override;

  /// @brief Returns every ring, the noise tracker and the gain stage to their
  ///        post-construction state. Allocates; control thread only.
  void reset() override;

  /// @brief Samples between an input sample and the output sample answering for
  ///        it, `n_fft - 1` plus one hop whenever the mask stage lags a frame.
  int latency_samples() const noexcept override;

  /// @brief What is still inside the pipeline when the input goes silent, which
  ///        is the delay itself: nothing here decays past it.
  int tail_samples() const noexcept override;

  /// @brief The configuration this processor was constructed with, as validated.
  const DenoiseClassicalConfig& config() const noexcept { return config_; }

 private:
  /// Builds one frame from the input rings, masks it and overlap-adds it.
  void analyze_frame();

  /// @brief Returns every value-carrying cell to its post-reset state when a
  ///        non-finite value has reached one. Called once per process() that
  ///        took a frame, and not at all by one that took none.
  /// @details Timing state is deliberately untouched -- the queue's fill, the
  ///   smoother's frame count and the ring cursors decide how many samples this
  ///   block owes, so returning them would make the block underrun the delay it
  ///   already reported. The input ring goes with the rest because the sample
  ///   that caused the discard is still in it and the tracker would reseed from
  ///   it on the next frame.
  /// @return true when anything was discarded.
  bool discard_non_finite_state() noexcept;

  /// Overlap-adds one masked frame at @p start and finalizes one hop of output.
  void emit_frame(const double* gains, const std::complex<float>* source);

  /// Moves every synthesis-ring sample below absolute index @p end into the
  /// delay queue, normalized by the accumulated analysis*synthesis window sum.
  void finalize_before(std::size_t end);

  DenoiseClassicalConfig config_;
  int n_fft_ = 0;
  int hop_length_ = 0;
  int n_bins_ = 0;
  int mask_latency_frames_ = 0;
  double sample_rate_ = 48000.0;
  int max_block_size_ = 0;
  int max_channels_ = 0;
  int active_channels_ = 0;
  bool prepared_ = false;

  std::vector<float> analysis_window_;
  std::vector<float> synthesis_window_;
  std::vector<float> window_product_;

  // Analysis input: one n_fft ring per channel, written sample by sample. A
  // frame is taken the moment the ring holds exactly the n_fft samples it spans.
  std::vector<float> input_ring_;
  int input_write_ = 0;
  int samples_to_next_frame_ = 0;

  // Per-frame scratch, all sized in prepare().
  std::vector<float> frame_;
  std::vector<std::complex<float>> spectra_;
  std::vector<std::complex<float>> held_spectra_;
  std::vector<std::complex<float>> masked_;
  std::vector<float> power_f_;
  std::vector<double> power_d_;
  std::vector<float> channel_power_f_;
  std::vector<double> channel_power_d_;
  std::vector<float> tracker_input_;
  std::vector<double> noise_frame_;

  // Overlap-add: one n_fft ring per channel plus the window sum they share,
  // since the mask is one real number per cell and every channel carries it.
  std::vector<float> synthesis_ring_;
  std::vector<float> window_sum_ring_;
  std::size_t ring_base_ = 0;
  std::size_t next_frame_start_ = 0;

  // Finished samples waiting to be handed back, one ring per channel. Primed
  // with latency_samples() zeros so a block always finds a block's worth here.
  std::vector<float> output_queue_;
  int queue_capacity_ = 0;
  int queue_read_ = 0;
  int queue_size_ = 0;

  std::unique_ptr<detail::GainStage> stage_;
  std::unique_ptr<common::NoiseTracker> tracker_;
  FFT fft_;
};

}  // namespace sonare::mastering::repair
