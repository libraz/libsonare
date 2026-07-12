#pragma once

/// @file insert_chain.h
/// @brief Shared insert-chain processing for buses and channel strips.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "rt/processor_base.h"

namespace sonare::mixing {

// Maximum sidechain width a graph-managed insert key can carry. The sidechain
// key stores borrowed pointers into the source buffers (not owned).
inline constexpr int kMaxSidechainChannels = 8;

// Graph-managed sidechain key for one insert slot, index-parallel to the insert
// vector. A default-constructed (managed == false) entry leaves a directly
// configured processor sidechain intact; managed == true with a zero width
// clears it.
struct InsertSidechain {
  std::array<const float*, kMaxSidechainChannels> channels{};
  int num_channels = 0;
  int num_samples = 0;
  bool managed = false;
};

/// @brief Runs one insert chain on the audio thread.
/// @details For each insert it forwards the slot's graph-managed sidechain key
///          (shifted forward by @p sidechain_offset samples), honours per-insert
///          bypass, and applies the StereoPairOnly front-pair view on a surround
///          buffer (@p num_channels > 2). RT-safe: no allocation. The caller
///          supplies @p shifted_scratch (capacity >= @p max_forward_rows) so the
///          shifted sidechain pointer table lives on the caller's stack.
/// @param inserts             The insert processors to run, in order.
/// @param stereo_pair_only    Index-parallel to @p inserts: 1 = front-pair-only.
/// @param sidechains          Sidechain keys indexed by @p first_insert_index +
///                            local; a shorter vector leaves later slots keyless.
/// @param first_insert_index  Offset into @p sidechains for this chain segment.
/// @param sidechain_offset    Sample offset applied to the key (for mid-block
///                            segmented processing).
/// @param max_forward_rows    Upper bound on sidechain rows forwarded to an
///                            insert (also the required @p shifted_scratch size).
inline void run_insert_chain(std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts,
                             const std::vector<uint8_t>& stereo_pair_only,
                             const std::vector<InsertSidechain>& sidechains, float* const* channels,
                             int num_channels, int num_samples, size_t first_insert_index,
                             int sidechain_offset, const float** shifted_scratch,
                             int max_forward_rows) {
  for (size_t local = 0; local < inserts.size(); ++local) {
    const size_t index = first_insert_index + local;
    const InsertSidechain* key = index < sidechains.size() ? &sidechains[index] : nullptr;
    if (key != nullptr && key->num_channels > 0 && key->num_samples > sidechain_offset) {
      // Clip the key length to whatever is available rather than discarding a
      // short block; dropping the key would make a sidechain compressor lose its
      // detector input for the block and click.
      const int rows = std::min(key->num_channels, max_forward_rows);
      const int remaining = key->num_samples - sidechain_offset;
      for (int ch = 0; ch < rows; ++ch) {
        shifted_scratch[ch] = key->channels[static_cast<size_t>(ch)] == nullptr
                                  ? nullptr
                                  : key->channels[static_cast<size_t>(ch)] + sidechain_offset;
      }
      inserts[local]->set_sidechain(shifted_scratch, rows, std::min(num_samples, remaining));
    } else if (key != nullptr && key->managed) {
      inserts[local]->clear_sidechain();
    } else {
      // Leave directly configured processor sidechains intact. Graph-managed
      // keys are marked through set_insert_sidechain().
    }
    if (inserts[local]->bypassed()) {
      continue;
    }
    // StereoPairOnly inserts see only the front L/R pair on a surround buffer:
    // the surround planes (2..N-1) pass through dry, and a width-sensitive insert
    // (e.g. eq.midSide, which aborts on a non-stereo width) gets the 2-plane view
    // it requires. At num_channels <= 2 this is the legacy full-buffer call.
    const bool spo = local < stereo_pair_only.size() && stereo_pair_only[local] != 0;
    const int insert_channels = (spo && num_channels > 2) ? 2 : num_channels;
    inserts[local]->process(channels, insert_channels, num_samples);
  }
}

}  // namespace sonare::mixing
