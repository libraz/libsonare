#pragma once

/// @file resource_limits.h
/// @brief Shared limits for caller-controlled offline resource expansion.

#include <cstddef>

#include "util/numeric_validation.h"

namespace sonare::resource {

/// Maximum number of float samples accepted by one offline audio operation.
inline constexpr std::size_t kMaxOfflineAudioSamples = 500'000'000;
/// Upper bound for caller-controlled iterative phase reconstruction.
inline constexpr int kMaxGriffinLimIterations = 256;

/// Maximum MIDI expansion performed by one project export operation.
inline constexpr std::size_t kMaxProjectMidiExportEvents = 1'000'000;
inline constexpr std::size_t kMaxMidiExportLoopIterations = 1'000'000;

/// Default file-size ceiling used by the general audio-file loaders.
inline constexpr std::size_t kMaxAudioFileBytes = 500u * 1024u * 1024u;

/// SoundFont parsing budgets. The peak estimate counts the caller-owned input
/// bytes plus the decoded float sample pool, which is the dominant expansion.
struct Sf2ResourceLimits {
  std::size_t max_file_bytes;
  std::size_t max_sample_points;
  std::size_t max_peak_bytes;
  std::size_t max_table_records;
};

inline constexpr Sf2ResourceLimits kDefaultSf2ResourceLimits{
    256u * 1024u * 1024u,  // input SF2 bytes
    64u * 1024u * 1024u,   // decoded sample points (256 MiB as float)
    512u * 1024u * 1024u,  // input + decoded float sample pool
    65'536u,               // one pdta table (indices are uint16_t)
};

inline bool sf2_file_fits(std::size_t file_bytes,
                          const Sf2ResourceLimits& limits = kDefaultSf2ResourceLimits) noexcept {
  return file_bytes <= limits.max_file_bytes;
}

/// Checks the dominant SF2 expansion without allocating. This helper is also
/// used for exact boundary tests with small custom budgets.
inline bool sf2_sample_expansion_fits(
    std::size_t file_bytes, std::size_t sample_points,
    const Sf2ResourceLimits& limits = kDefaultSf2ResourceLimits) noexcept {
  if (!sf2_file_fits(file_bytes, limits) || sample_points > limits.max_sample_points) {
    return false;
  }
  std::size_t decoded_bytes = 0;
  if (!numeric::checked_size_product(sample_points, sizeof(float), limits.max_peak_bytes,
                                     &decoded_bytes)) {
    return false;
  }
  std::size_t peak_bytes = 0;
  return numeric::checked_add(file_bytes, decoded_bytes, &peak_bytes) &&
         peak_bytes <= limits.max_peak_bytes;
}

inline bool sf2_table_records_fit(
    std::size_t current_records, std::size_t additional_records,
    const Sf2ResourceLimits& limits = kDefaultSf2ResourceLimits) noexcept {
  std::size_t total = 0;
  return numeric::checked_add(current_records, additional_records, &total) &&
         total <= limits.max_table_records;
}

}  // namespace sonare::resource
