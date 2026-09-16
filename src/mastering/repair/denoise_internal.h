#pragma once

/// @file denoise_internal.h
/// @brief Pieces of the classical denoiser that are driven directly by a test.
///
/// The denoiser walks its STFT one frame at a time so a whole-file mask never
/// exists. Everything in that walk is causal except the gain smoother, which
/// reads the frame after the one it writes; the smoother is declared here so a
/// test can compare it against a plane-shaped median without a second copy of
/// the gain functions around it.

#include <vector>

namespace sonare::mastering::repair::detail {

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

}  // namespace sonare::mastering::repair::detail
