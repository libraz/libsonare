#pragma once

/// @file sample_reader.h
/// @brief Position stepping, loop wrapping and linear interpolation for
///        pitched sample playback.
///
/// Holds a read-only view of a sample pool plus the region being played. The
/// pool and its lifetime belong to whoever supplied it (an Sf2File's decoded
/// pool, a SampleBank), so a reader is only valid while that owner is.
///
/// A looping region is required to satisfy start <= loop_start < loop_end <=
/// end; the read then never leaves the region, which is why neither tap is
/// range-checked per sample. Whoever fills a SampleRegion enforces it.
///
/// RT contract: allocation-free and header-only.

#include <cmath>
#include <cstdint>

namespace sonare::midi::synth {

/// One playable span inside a sample pool, in pool indices.
struct SampleRegion {
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t loop_start = 0;
  uint32_t loop_end = 0;
  /// SF2 sampleModes: 0 = no loop, 1 = continuous, 3 = loop while key down.
  int loop_mode = 0;
};

/// Reads a SampleRegion at a fractional position advanced by the caller.
class SampleReader {
 public:
  /// @p offset is added to the region start (an attack skip), in samples.
  void start(const float* pool, const SampleRegion& region, double offset = 0.0) noexcept {
    pool_ = pool;
    region_ = region;
    pos_ = static_cast<double>(region.start) + offset;
  }

  bool valid() const noexcept { return pool_ != nullptr; }
  double position() const noexcept { return pos_; }
  void set_position(double pos) noexcept { pos_ = pos; }
  void advance(double increment) noexcept { pos_ += increment; }

  /// True while the region's loop is the one being read.
  bool looping(bool key_down) const noexcept {
    return region_.loop_mode == 1 || (region_.loop_mode == 3 && key_down);
  }

  /// Wraps a looping read back into its loop, and reports whether the region
  /// still has audio to give. A non-finite position fails the same way a
  /// one-shot past its end does, so a diverged increment cannot index the pool.
  bool wrap(bool looping) noexcept {
    if (!looping) return pos_ < static_cast<double>(region_.end);
    if (!std::isfinite(pos_)) return false;
    const double loop_start = static_cast<double>(region_.loop_start);
    const double loop_len = static_cast<double>(region_.loop_end) - loop_start;
    // Also catches an increment larger than the loop.
    if (pos_ >= static_cast<double>(region_.loop_end) && loop_len > 0.0) {
      pos_ = loop_start + std::fmod(pos_ - loop_start, loop_len);
    }
    return true;
  }

  /// Linear interpolation; the second tap wraps across the loop seam.
  float read(bool looping) const noexcept {
    const uint32_t i0 = static_cast<uint32_t>(pos_);
    const float mu = static_cast<float>(pos_ - static_cast<double>(i0));
    uint32_t i1 = i0 + 1;
    if (looping && i1 >= region_.loop_end) i1 = region_.loop_start;
    if (i1 >= region_.end) i1 = region_.end > 0 ? region_.end - 1 : 0;
    const float y0 = pool_[i0];
    const float y1 = pool_[i1];
    return y0 + mu * (y1 - y0);
  }

 private:
  const float* pool_ = nullptr;
  SampleRegion region_{};
  double pos_ = 0.0;
};

}  // namespace sonare::midi::synth
