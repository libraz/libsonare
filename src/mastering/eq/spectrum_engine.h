#pragma once

/// @file spectrum_engine.h
/// @brief Realtime-safe EQ spectrum snapshots and helper mapping.

#include <array>
#include <cstddef>
#include <cstdint>

#include "mastering/eq/eq_band.h"

namespace sonare::mastering::eq {

static constexpr size_t kSpectrumStreamCapacity = 256;
static constexpr size_t kSpectrumProfileBands = 16;

struct SpectrumPoint {
  float left = 0.0f;
  float right = 0.0f;
};

struct EqualizerSpectrumSnapshot {
  std::array<SpectrumPoint, kSpectrumStreamCapacity> pre{};
  std::array<SpectrumPoint, kSpectrumStreamCapacity> post{};
  size_t pre_count = 0;
  size_t post_count = 0;
  std::array<float, 24> band_gain_db{};
  std::array<float, kSpectrumProfileBands> profile_db{};
  uint64_t seq = 0;
};

struct SpectrumGrabResult {
  size_t index = 0;
  bool use_existing = false;
};

SpectrumGrabResult spectrum_grab_band(float frequency_hz, const EqBand* bands, size_t num_bands,
                                      size_t max_bands = 24) noexcept;

}  // namespace sonare::mastering::eq
