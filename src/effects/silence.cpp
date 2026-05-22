/// @file silence.cpp
/// @brief Implementation of librosa-style trim() and split().

#include "effects/silence.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace sonare {

namespace {

/// @brief Compute centered, zero-padded RMS per frame.
/// @details Matches librosa's RMS with center=True: the signal is padded by
///          frame_length/2 with reflect padding, then framed. Here we use
///          constant zero padding which is sufficient for silence detection
///          (frames that fall fully into the pad will read as low energy,
///          which is what we want at the boundary).
std::vector<float> centered_rms(const float* x, std::size_t n, int frame_length, int hop_length) {
  const int half = frame_length / 2;
  std::vector<float> rms;
  if (n == 0) return rms;
  // Number of frames matches librosa: floor(n / hop_length) + 1 with center=True.
  const int n_frames = static_cast<int>(n) / hop_length + 1;
  rms.reserve(static_cast<std::size_t>(n_frames));
  for (int f = 0; f < n_frames; ++f) {
    const int center = f * hop_length;
    const int start = center - half;
    const int end = start + frame_length;  // exclusive
    double sum_sq = 0.0;
    for (int i = std::max(0, start); i < std::min(static_cast<int>(n), end); ++i) {
      const double v = x[i];
      sum_sq += v * v;
    }
    // librosa divides by frame_length (not just observed samples) for RMS.
    rms.push_back(static_cast<float>(std::sqrt(sum_sq / static_cast<double>(frame_length))));
  }
  return rms;
}

/// @brief Identify non-silent frame indices.
std::vector<bool> non_silent_frames(const std::vector<float>& rms, float top_db) {
  std::vector<bool> mask(rms.size(), false);
  if (rms.empty()) return mask;
  // Convert top_db to amplitude ratio: amp_threshold = peak / 10^(top_db/20).
  float peak = 0.0f;
  for (float r : rms) peak = std::max(peak, r);
  if (peak <= 0.0f) return mask;
  const float thr = peak * std::pow(10.0f, -top_db / 20.0f);
  for (std::size_t i = 0; i < rms.size(); ++i) {
    mask[i] = rms[i] > thr;
  }
  return mask;
}

}  // namespace

TrimResult trim(const float* x, std::size_t n, float top_db, int frame_length, int hop_length) {
  if (n > 0 && x == nullptr) {
    throw std::invalid_argument("trim: null input with non-zero length");
  }
  if (frame_length <= 0 || hop_length <= 0) {
    throw std::invalid_argument("trim: frame_length and hop_length must be > 0");
  }
  if (top_db <= 0.0f) {
    throw std::invalid_argument("trim: top_db must be > 0");
  }
  TrimResult result{{}, 0, 0};
  if (n == 0) return result;

  const auto rms = centered_rms(x, n, frame_length, hop_length);
  const auto mask = non_silent_frames(rms, top_db);

  int first = -1;
  int last = -1;
  for (int i = 0; i < static_cast<int>(mask.size()); ++i) {
    if (mask[i]) {
      if (first < 0) first = i;
      last = i;
    }
  }
  if (first < 0) {
    // All silent — return empty audio with zero range.
    return result;
  }
  // Convert frame indices to sample indices (centered frames).
  const int start_sample = std::max(0, first * hop_length);
  const int end_sample = std::min(static_cast<int>(n), (last + 1) * hop_length);
  result.start_sample = start_sample;
  result.end_sample = end_sample;
  result.audio.assign(x + start_sample, x + end_sample);
  return result;
}

TrimResult trim(const std::vector<float>& x, float top_db, int frame_length, int hop_length) {
  return trim(x.data(), x.size(), top_db, frame_length, hop_length);
}

std::vector<std::pair<int, int>> split(const float* x, std::size_t n, float top_db,
                                       int frame_length, int hop_length) {
  if (n > 0 && x == nullptr) {
    throw std::invalid_argument("split: null input with non-zero length");
  }
  if (frame_length <= 0 || hop_length <= 0) {
    throw std::invalid_argument("split: frame_length and hop_length must be > 0");
  }
  if (top_db <= 0.0f) {
    throw std::invalid_argument("split: top_db must be > 0");
  }
  std::vector<std::pair<int, int>> intervals;
  if (n == 0) return intervals;

  const auto rms = centered_rms(x, n, frame_length, hop_length);
  const auto mask = non_silent_frames(rms, top_db);

  // Scan contiguous true regions.
  int run_start = -1;
  for (int i = 0; i < static_cast<int>(mask.size()); ++i) {
    if (mask[i] && run_start < 0) {
      run_start = i;
    } else if (!mask[i] && run_start >= 0) {
      const int start_sample = std::max(0, run_start * hop_length);
      const int end_sample = std::min(static_cast<int>(n), i * hop_length);
      if (end_sample > start_sample) intervals.emplace_back(start_sample, end_sample);
      run_start = -1;
    }
  }
  if (run_start >= 0) {
    const int start_sample = std::max(0, run_start * hop_length);
    const int end_sample = static_cast<int>(n);
    if (end_sample > start_sample) intervals.emplace_back(start_sample, end_sample);
  }
  return intervals;
}

std::vector<std::pair<int, int>> split(const std::vector<float>& x, float top_db, int frame_length,
                                       int hop_length) {
  return split(x.data(), x.size(), top_db, frame_length, hop_length);
}

}  // namespace sonare
