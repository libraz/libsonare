/// @file silence.cpp
/// @brief Implementation of librosa-style trim() and split().

#include "effects/silence.h"

#include <algorithm>
#include <cmath>

#include "util/db.h"
#include "util/exception.h"

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
  const float thr = peak * db_to_linear(-top_db);
  for (std::size_t i = 0; i < rms.size(); ++i) {
    mask[i] = rms[i] > thr;
  }
  return mask;
}

}  // namespace

TrimResult trim(const float* x, std::size_t n, float top_db, int frame_length, int hop_length) {
  // Empty input is rejected rather than answered with {{}, 0, 0}: that value is
  // the all-silent result, so accepting an empty buffer would make "nothing was
  // passed in" and "nothing in the signal rose above the threshold"
  // indistinguishable to the caller.
  if (n == 0) {
    throw SonareException(ErrorCode::InvalidParameter, "trim: input must not be empty");
  }
  if (x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "trim: null input with non-zero length");
  }
  if (frame_length <= 0 || hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "trim: frame_length and hop_length must be > 0");
  }
  if (top_db <= 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "trim: top_db must be > 0");
  }
  TrimResult result{{}, 0, 0};
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

SplitReport split_with_report(const float* x, std::size_t n, float top_db, int frame_length,
                              int hop_length) {
  // Rejected for the same reason as trim(): an empty interval list is already
  // the all-silent result, so an empty input must not share it.
  if (n == 0) {
    throw SonareException(ErrorCode::InvalidParameter, "split: input must not be empty");
  }
  if (x == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter, "split: null input with non-zero length");
  }
  if (frame_length <= 0 || hop_length <= 0) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "split: frame_length and hop_length must be > 0");
  }
  if (top_db <= 0.0f) {
    throw SonareException(ErrorCode::InvalidParameter, "split: top_db must be > 0");
  }
  std::vector<std::pair<int, int>> intervals;
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
  // Measured from the same frames the mask was built from, so the figure and the
  // intervals can never describe different passes. The quietest frame against the
  // peak IS the threshold the detector compares to, read back as a dB depth: it
  // is the largest top_db at which this signal still has a frame under the line.
  SplitReport report;
  report.intervals = std::move(intervals);
  if (!rms.empty()) {
    const float peak = *std::max_element(rms.begin(), rms.end());
    if (peak > 0.0f) {
      const float quietest = *std::min_element(rms.begin(), rms.end());
      report.silence_ceiling_db = -linear_to_db(quietest / peak);
    }
  }
  return report;
}

std::vector<std::pair<int, int>> split(const float* x, std::size_t n, float top_db,
                                       int frame_length, int hop_length) {
  return split_with_report(x, n, top_db, frame_length, hop_length).intervals;
}

CommonSplitReport split_common_with_report(const float* const* signals, const std::size_t* lengths,
                                           std::size_t signal_count, float top_db, int frame_length,
                                           int hop_length) {
  if (signal_count == 0 || signals == nullptr || lengths == nullptr) {
    throw SonareException(ErrorCode::InvalidParameter,
                          "split_common: at least one signal is required");
  }
  CommonSplitReport out;
  std::vector<std::pair<int, int>> collected;
  for (std::size_t index = 0; index < signal_count; ++index) {
    SplitReport one =
        split_with_report(signals[index], lengths[index], top_db, frame_length, hop_length);
    const int count = static_cast<int>(one.intervals.size());
    if (index == 0) {
      out.silence_ceiling_db = one.silence_ceiling_db;
      out.max_signal_intervals = count;
      out.min_signal_intervals = count;
    } else {
      out.silence_ceiling_db = std::min(out.silence_ceiling_db, one.silence_ceiling_db);
      out.max_signal_intervals = std::max(out.max_signal_intervals, count);
      out.min_signal_intervals = std::min(out.min_signal_intervals, count);
    }
    collected.insert(collected.end(), one.intervals.begin(), one.intervals.end());
  }
  std::sort(collected.begin(), collected.end());
  for (const auto& range : collected) {
    // Touching counts as overlapping: two takes whose intervals meet exactly
    // leave no silent sample between them, so a cut there would be mid-phrase.
    if (!out.intervals.empty() && range.first <= out.intervals.back().second) {
      out.intervals.back().second = std::max(out.intervals.back().second, range.second);
      continue;
    }
    out.intervals.push_back(range);
  }
  return out;
}

std::vector<std::pair<int, int>> split(const std::vector<float>& x, float top_db, int frame_length,
                                       int hop_length) {
  return split(x.data(), x.size(), top_db, frame_length, hop_length);
}

}  // namespace sonare
