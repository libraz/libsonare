#pragma once

/// @file parallel_paths.h
/// @brief Latency alignment for signal paths that are summed back together.
///
/// A processor that splits its input into parallel paths (dry and wet, a
/// harmonic generator beside its source, crossover bands) declares each path's
/// own latency here in Q8 samples. Every path is delayed up to the longest one
/// before the sum, and that longest latency is the figure the processor reports
/// to plugin-delay compensation, so the sum and the report cannot disagree.
/// A fractional remainder is matched by linear interpolation; at the half sample
/// an ADAA1 stage adds, that is the two-tap average ADAA1 itself applies to a
/// signal in its linear region.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace sonare::rt {

class ParallelPaths {
 public:
  /// @brief Declares each path's own latency in Q8 samples (256 == one sample).
  /// @details Control thread: reallocates the compensation lines of every
  ///   channel already prepared and clears their history.
  ///   Declaring the latencies already held changes nothing.
  void set_path_latencies_q8(std::vector<int> latencies_q8) {
    for (int& latency : latencies_q8) latency = std::max(0, latency);
    if (latencies_q8 == latencies_q8_) return;
    latencies_q8_ = std::move(latencies_q8);
    latency_q8_ = 0;
    for (const int latency : latencies_q8_) latency_q8_ = std::max(latency_q8_, latency);
    const size_t channels = channels_;
    channels_ = 0;
    lines_.clear();
    ensure_channels(channels);
  }

  /// @brief Allocates compensation lines for at least @p num_channels channels.
  /// @details Control thread. Existing channels keep their history.
  void ensure_channels(size_t num_channels) {
    if (num_channels <= channels_) return;
    lines_.resize(num_channels * latencies_q8_.size());
    for (size_t ch = channels_; ch < num_channels; ++ch) {
      for (size_t path = 0; path < latencies_q8_.size(); ++path) {
        Line& line = lines_[ch * latencies_q8_.size() + path];
        const int compensation = latency_q8_ - latencies_q8_[path];
        line.whole = static_cast<size_t>(compensation >> 8);
        line.fraction = static_cast<float>(compensation & 0xff) / 256.0f;
        line.ring.assign(line.whole + (line.fraction > 0.0f ? 1 : 0), 0.0f);
        line.write = 0;
      }
    }
    channels_ = num_channels;
  }

  void reset() noexcept {
    for (Line& line : lines_) {
      std::fill(line.ring.begin(), line.ring.end(), 0.0f);
      line.write = 0;
    }
  }

  /// @brief Whether every held compensation sample is finite.
  bool finite() const noexcept {
    for (const Line& line : lines_) {
      for (const float sample : line.ring) {
        if (!std::isfinite(sample)) return false;
      }
    }
    return true;
  }

  size_t num_paths() const noexcept { return latencies_q8_.size(); }
  size_t num_channels() const noexcept { return channels_; }
  int path_latency_q8(size_t path) const noexcept { return latencies_q8_[path]; }

  /// @brief The common latency every path reaches the sum with, in Q8 samples.
  int latency_samples_q8() const noexcept { return latency_q8_; }
  int latency_samples() const noexcept { return latency_q8_ >> 8; }

  /// @brief Delays one sample of @p path on @p channel to the common latency.
  /// @details RT-safe; @p channel must be below num_channels().
  float align(size_t path, size_t channel, float x) noexcept {
    Line& line = lines_[channel * latencies_q8_.size() + path];
    const size_t size = line.ring.size();
    if (size == 0) return x;
    // ring holds x[n-size .. n-1]; ring[write] is the oldest.
    const float oldest = line.ring[line.write];
    float y = oldest;
    if (line.fraction > 0.0f) {
      const float nearer = line.whole == 0 ? x : line.ring[(line.write + size - line.whole) % size];
      y = (1.0f - line.fraction) * nearer + line.fraction * oldest;
    }
    line.ring[line.write] = x;
    line.write = (line.write + 1) % size;
    return y;
  }

  /// @brief Aligns @p num_samples samples of @p path on @p channel in place.
  void align_block(size_t path, size_t channel, float* samples, int num_samples) noexcept {
    if (lines_[channel * latencies_q8_.size() + path].ring.empty()) return;
    for (int i = 0; i < num_samples; ++i) samples[i] = align(path, channel, samples[i]);
  }

 private:
  struct Line {
    std::vector<float> ring;
    size_t write = 0;
    size_t whole = 0;
    float fraction = 0.0f;
  };

  std::vector<int> latencies_q8_;
  int latency_q8_ = 0;
  size_t channels_ = 0;
  std::vector<Line> lines_;  // channel-major: [channel * num_paths + path]
};

}  // namespace sonare::rt
