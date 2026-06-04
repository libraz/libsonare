#pragma once

/// @file voice_random.h
/// @brief Seeded, deterministic per-voice variation ("analog" detune / drift /
///        pan scatter) for the voice toolkit.
///
/// Determinism contract (§0 of the instrument build plan): no RNG, no wall
/// clock. All variation derives from a (voice_index, note, age) seed through a
/// fixed integer hash, so the same project bounces bit-identically within one
/// build while distinct voices still decorrelate.

#include <cstdint>

namespace sonare::midi::synth {

/// SplitMix64-style avalanche of a 64-bit seed (Steele/Lea/Flood finalizer).
inline uint64_t voice_hash(uint64_t seed) noexcept {
  uint64_t z = seed + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/// Combines the canonical per-voice identifiers into one seed.
inline uint64_t voice_seed(uint32_t voice_index, uint32_t note, uint64_t age) noexcept {
  return voice_hash((static_cast<uint64_t>(voice_index) << 40) ^
                    (static_cast<uint64_t>(note) << 32) ^ age);
}

/// Uniform float in [0, 1) from a seed.
inline float voice_random_unipolar(uint64_t seed) noexcept {
  // Top 24 bits -> [0,1) keeps the mapping exact in float.
  return static_cast<float>(voice_hash(seed) >> 40) * (1.0f / 16777216.0f);
}

/// Uniform float in [-1, 1) from a seed.
inline float voice_random_bipolar(uint64_t seed) noexcept {
  return 2.0f * voice_random_unipolar(seed) - 1.0f;
}

/// A small deterministic stream for voices that need several variation values
/// (per-oscillator detune, drift phase, pan...). Counter-based: value(k) is
/// independent of how many values were drawn before it.
class VoiceRandomSequence {
 public:
  VoiceRandomSequence() = default;
  explicit VoiceRandomSequence(uint64_t seed) noexcept : seed_(seed) {}

  void reseed(uint32_t voice_index, uint32_t note, uint64_t age) noexcept {
    seed_ = voice_seed(voice_index, note, age);
    counter_ = 0;
  }

  float next_unipolar() noexcept { return voice_random_unipolar(seed_ ^ counter_++); }
  float next_bipolar() noexcept { return voice_random_bipolar(seed_ ^ counter_++); }

  /// Random access without disturbing the stream position.
  float unipolar_at(uint64_t index) const noexcept { return voice_random_unipolar(seed_ ^ index); }
  float bipolar_at(uint64_t index) const noexcept { return voice_random_bipolar(seed_ ^ index); }

 private:
  uint64_t seed_ = 0;
  uint64_t counter_ = 0;
};

}  // namespace sonare::midi::synth
