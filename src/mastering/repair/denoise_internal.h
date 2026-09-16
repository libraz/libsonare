#pragma once

/// @file denoise_internal.h
/// @brief Pieces of the classical denoiser driven by something other than its
///        own entry points -- a test, or the streaming front end.
///
/// The denoiser walks its STFT one frame at a time so a whole-file mask never
/// exists. Everything in that walk is causal except the gain smoother, which
/// reads the frame after the one it writes; the smoother is declared here so a
/// test can compare it against a plane-shaped median without a second copy of
/// the gain functions around it. The mask stage above it is declared here so
/// the realtime front end applies the same gain math rather than a second
/// implementation of it; its members are defined in denoise_classical.cpp, next
/// to the file-local gain functions they call.

#include <complex>
#include <memory>
#include <vector>

#include "mastering/common/noise_tracker.h"
#include "mastering/repair/denoise_classical.h"

namespace sonare::mastering::repair::detail {

/// @brief Maps an estimator onto the NoiseTracker mode that implements it.
/// @details Exhaustive and without a `default`, so a new estimator cannot inherit
///   whichever mode an initializer happens to name. Quantile throws rather than
///   falling through: it is answered before this is reached, not by a tracker.
common::NoiseTracker::Mode tracker_mode_for(DenoiseNoiseEstimator estimator);

/// @brief One frame's two power spectra, each as its own consumer wants it.
/// @details The estimator and the reported floor read what Spectrogram::power()
///   holds, re^2 + im^2 in float; the gain recursion reads |z|^2 widened from the
///   magnitude. The two part company in the last bits and this module has always
///   been defined by that pair, so both are derived from the frame rather than
///   one from the other.
void frame_powers(const std::complex<float>* frame, int bins, float* power_f, double* power_d);

/// @brief Causal 3x3 median over a gain mask, one frame in and one out.
/// @details Holds the three raw-gain frames the median spans, which is O(bins).
///   A frame's median needs its successor, so the answer for frame t arrives
///   once t+1 has been pushed and the last frame is answered by @ref flush.
///   The window is truncated rather than extended at the first and last frame
///   and at the first and last bin, so the median there is taken over 4 or 6
///   values instead of 9.
class MedianGainSmoother {
 public:
  explicit MedianGainSmoother(int bins);

  /// @brief Feeds one frame of raw gains.
  /// @return The median frame for the previous push, or null on the first.
  const double* push(const double* raw_frame);

  /// @brief Answers for the last pushed frame, which has no successor.
  /// @return Null when nothing was ever pushed.
  const double* flush();

 private:
  const double* emit(int target, bool has_next);
  double* slot(int frame);

  int bins_;
  int pushed_ = 0;
  std::vector<std::vector<double>> raw_;
  std::vector<double> out_;
  std::vector<double> window_;
};

/// @brief The gain mask, one STFT frame in and one out.
/// @details State is O(bins): the decision-directed recursion's previous clean
///   power, plus the smoother's own three frames when gain smoothing is on.
///   Everything but that smoother is causal already -- the recursion carries one
///   frame, and spectral subtraction carries nothing -- so the one frame of
///   latency @ref latency reports is the smoother's.
class GainStage {
 public:
  GainStage(int bins, const DenoiseClassicalConfig& config);

  /// Frames between a pushed spectrum and the gain frame that answers for it.
  int latency() const;

  /// @brief Feeds one frame's |z|^2 and per-bin noise PSD.
  /// @return The finished gain frame for the frame @ref latency back, or null
  ///         while the smoother has yet to see a successor.
  const double* push(const double* power_frame, const double* noise_frame);

  /// @brief Answers for the last pushed frame, which has no successor.
  /// @return Null when nothing is pending, which is every non-smoothing pass.
  const double* flush();

 private:
  void compute_raw(const double* power_frame, const double* noise_frame, double* raw);
  const double* finish(const double* source);

  int bins_;
  DenoiseClassicalConfig config_;
  double floor_gain_;
  bool berouti_;
  bool log_spectral_;
  std::vector<double> prev_clean_power_;
  std::vector<double> raw_;
  std::vector<double> out_;
  std::unique_ptr<MedianGainSmoother> smoother_;
};

}  // namespace sonare::mastering::repair::detail
