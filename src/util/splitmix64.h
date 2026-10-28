#pragma once

/// @file splitmix64.h
/// @brief SplitMix64, the library's portable deterministic PRNG.
///
/// Every value is a pure function of the seed and the draw index, specified down
/// to the bit, so a stream reproduces across compilers, standard libraries and
/// languages (the Python CLI's WAV writer recomputes the same dither). Standard
/// `<random>` distributions are implementation-defined and cannot promise that.

#include <cstdint>

namespace sonare {

/// Weyl increment SplitMix64 adds to its state before every draw.
inline constexpr std::uint64_t kSplitMix64Gamma = 0x9E3779B97F4A7C15ull;

/// @brief SplitMix64's output mix of one state value.
constexpr std::uint64_t splitmix64_mix(std::uint64_t z) noexcept {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/// @brief Draw @p index (0-based) of the stream seeded with @p seed, without
///        stepping through the draws before it.
constexpr std::uint64_t splitmix64_at(std::uint64_t seed, std::uint64_t index) noexcept {
  return splitmix64_mix(seed + (index + 1) * kSplitMix64Gamma);
}

/// @brief Sequential SplitMix64 stream; draw k equals `splitmix64_at(seed, k)`.
class SplitMix64 {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += kSplitMix64Gamma;
    return splitmix64_mix(state_);
  }

  /// Uniform double in [0, 1) from the top 53 bits.
  double uniform() noexcept {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }

 private:
  std::uint64_t state_;
};

}  // namespace sonare
