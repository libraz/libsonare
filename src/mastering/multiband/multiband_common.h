#pragma once

/// @file multiband_common.h
/// @brief Band-split plumbing shared by the multiband processors.
///
/// Every multiband processor prepares a Crossover plus caller-owned scratch,
/// validates the same block shape in process(), sums its members' non-finite
/// discard counts around a block, and pads shallower bands to the deepest band
/// delay. These helpers hold those steps; the per-band processing stays at each
/// call site.

#include <cstddef>
#include <cstdint>
#include <string>

#include "mastering/multiband/crossover.h"
#include "rt/parallel_paths.h"
#include "rt/processor_base.h"
#include "util/exception.h"

namespace sonare::mastering::multiband {

/// @brief Validates a multiband process() call against the prepared bounds.
/// @param name Class name used in the exception messages.
/// @return false when the block is empty and the caller has nothing to do.
/// @throws SonareException on an unprepared processor, a negative size, a block
///         beyond the prepared channel or sample bound, or a null buffer.
inline bool validate_multiband_block(bool prepared, const char* name, int max_channels,
                                     int max_block_size, float* const* channels, int num_channels,
                                     int num_samples) {
  rt::ProcessorBase::ensure_prepared(prepared, name);
  if (!rt::ProcessorBase::validate_block_size(num_channels, num_samples)) {
    return false;
  }
  if (num_channels > max_channels) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("num_channels exceeds prepared ") + name + " capacity");
  }
  if (num_samples > max_block_size) {
    throw SonareException(ErrorCode::InvalidParameter,
                          std::string("num_samples exceeds prepared ") + name + " block size");
  }
  rt::ProcessorBase::validate_channel_buffers(channels, num_channels);
  return true;
}

/// @brief Prepares @p crossover and sizes @p scratch to the prepared bounds, so
///        process() splits without allocating and rejects an oversized block
///        instead of growing the scratch on the audio thread.
inline void prepare_band_split(Crossover& crossover, CrossoverScratch& scratch, double sample_rate,
                               int max_block_size, int max_channels) {
  crossover.prepare(sample_rate, max_block_size, max_channels);
  crossover.prepare_scratch(scratch, max_channels, max_block_size);
}

/// @brief Applies a new split config; the crossover rebuilds (and zeroes) itself
///        only for a changed split, and the scratch follows its band count.
inline void apply_band_split_config(Crossover& crossover, CrossoverScratch& scratch,
                                    const CrossoverConfig& config, bool prepared, int max_channels,
                                    int max_block_size) {
  crossover.set_config(config);
  if (prepared) crossover.prepare_scratch(scratch, max_channels, max_block_size);
}

/// @brief Sum of the crossover's and every member's non-finite discard count.
///        Read once on either side of a block: a change detects that some member
///        moved without standing in for the caller's own count, which is one per
///        process() call.
/// @param member_count Callable mapping one member to its discard count.
template <typename Members, typename MemberCount>
uint64_t member_discard_total(const Crossover& crossover, const Members& members,
                              MemberCount member_count) noexcept {
  uint64_t total = crossover.non_finite_discard_count();
  for (const auto& member : members) total += member_count(member);
  return total;
}

/// @brief Pads one band's channels to the deepest band delay so the sum
///        reconstructs the crossover.
inline void align_band(rt::ParallelPaths& band_paths, size_t band,
                       const std::vector<float*>& band_channels, int num_channels,
                       int num_samples) noexcept {
  for (int ch = 0; ch < num_channels; ++ch) {
    band_paths.align_block(band, static_cast<size_t>(ch), band_channels[static_cast<size_t>(ch)],
                           num_samples);
  }
}

}  // namespace sonare::mastering::multiband
