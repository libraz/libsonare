#pragma once

/// @file insert_chain.h
/// @brief Shared insert-chain processing for buses and channel strips.

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "mixing/alignment_delay.h"
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

/// @brief Points one per-insert alignment bank slot at @p delay_samples_q8 over
///        @p prepared_channels planes, allocating only when it has work to do.
/// @details A slot asked for a zero delay is left unprepared: it then owns no
///          delay-line storage and both @c process() and @c prime() are no-ops
///          on it, which matters because most inserts are latency-free. A slot
///          that already carries a delay is still re-pointed at zero, since a
///          chain whose insert order shifts can hand a previously latent slot to
///          a latency-free insert. Control thread only (may allocate).
inline void configure_insert_alignment_delay(AlignmentDelay& delay, int prepared_channels,
                                             int delay_samples_q8) {
  if (delay_samples_q8 <= 0 && delay.delay_samples_q8() == 0) {
    return;
  }
  delay.set_prepared_channels(prepared_channels);
  delay.set_delay_samples_q8(delay_samples_q8);
}

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
/// @param stereo_pair_alignment_delays Optional index-parallel state used by a
///                            surround bus to delay untouched planes after a
///                            latent StereoPairOnly insert. Each state must be
///                            prepared for planes 2..N-1 by the caller.
/// @param bypass_alignment_delays Optional index-parallel state that keeps a
///                            bypassed latent insert's contribution to the
///                            chain's reported latency real (soft bypass). Each
///                            state must be prepared for every plane the caller
///                            can pass and carry the insert's own Q8 latency.
/// @note Both delay banks are indexed by the insert's position within @p inserts,
///       not by @p first_insert_index + that position. A caller that spreads one
///       addressing space across several chain segments offsets the pointer itself.
inline void run_insert_chain(std::vector<std::unique_ptr<rt::ProcessorBase>>& inserts,
                             const std::vector<uint8_t>& stereo_pair_only,
                             const std::vector<InsertSidechain>& sidechains, float* const* channels,
                             int num_channels, int num_samples, size_t first_insert_index,
                             int sidechain_offset, const float** shifted_scratch,
                             int max_forward_rows, int detector_excluded_channel = -1,
                             AlignmentDelay* stereo_pair_alignment_delays = nullptr,
                             AlignmentDelay* bypass_alignment_delays = nullptr) {
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
    const bool latent = inserts[local]->latency_samples_q8() > 0;
    const bool spo = local < stereo_pair_only.size() && stereo_pair_only[local] != 0;
    if (inserts[local]->bypassed()) {
      // Soft bypass: the chain keeps reporting this insert's latency, because
      // PDC is recomputed on the control thread and bypass is toggled by an
      // atomic the audio thread reads per block -- a bypass that silently
      // shortened the chain would slide this signal several milliseconds
      // forward against the rest of the mix for as long as the stale PDC
      // stood. Run the insert's own latency as a plain delay over every plane
      // instead, so the block delay stays exactly what latency_samples_q8()
      // advertises across the toggle. The state is owned by the caller and
      // preallocated, keeping this path RT safe. A latency-free insert (the
      // common case) costs nothing.
      if (latent && stereo_pair_alignment_delays != nullptr && spo && num_channels > 2) {
        // Keep the surround aligner fed with what it would have seen, so
        // un-bypassing does not open its planes with a delay line of silence.
        // Ordered before the substitute delay so it reads the same undelayed
        // planes the active path hands it.
        stereo_pair_alignment_delays[local].prime(channels + 2, num_channels - 2, num_samples);
      }
      if (latent && bypass_alignment_delays != nullptr) {
        bypass_alignment_delays[local].process(channels, num_channels, num_samples);
      }
      continue;
    }
    if (latent && bypass_alignment_delays != nullptr) {
      // Same in the other direction: the substitute delay is fed the insert's
      // input on every active block, so engaging bypass switches to a warm
      // delay line and stays continuous instead of dropping out for the length
      // of the compensation. Read-only, and ordered ahead of the insert because
      // the insert overwrites this buffer in place.
      bypass_alignment_delays[local].prime(channels, num_channels, num_samples);
    }
    // StereoPairOnly inserts see only the front L/R pair on a surround buffer:
    // the surround planes (2..N-1) pass through dry, and a width-sensitive insert
    // (e.g. eq.midSide, which aborts on a non-stereo width) gets the 2-plane view
    // it requires. At num_channels <= 2 this is the legacy full-buffer call.
    const int insert_channels = (spo && num_channels > 2) ? 2 : num_channels;
    // A surround bus owns the only layout-aware detector context in the mixer.
    // A StereoPairOnly insert gets only L/R, so its LFE exclusion is necessarily
    // disabled by ProcessorBase's range check.
    inserts[local]->set_detector_excluded_channel(detector_excluded_channel);
    inserts[local]->process(channels, insert_channels, num_samples);

    // The insert has just delayed its L/R output, while planes 2..N-1 were not
    // presented to it. Delay those untouched planes by the same reported Q8
    // latency so a surround bed remains phase/time aligned. The state is owned
    // by the caller and preallocated during prepare(), keeping this path RT
    // safe. A bypassed insert never reaches here; it is compensated uniformly
    // across every plane by the bypass bank instead (the early continue above).
    if (spo && num_channels > 2 && stereo_pair_alignment_delays != nullptr && latent) {
      stereo_pair_alignment_delays[local].process(channels + 2, num_channels - 2, num_samples);
    }
  }
}

}  // namespace sonare::mixing
