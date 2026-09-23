#pragma once

/// @file source_residual.h
/// @brief SourceResidualSplitter — attributes a shared bus-wide DSP residual
///        (mix minus dry) to per-source render targets, proportioned by each
///        source's recent dry energy.
///
/// NativeSynth and Sf2Player both run bus-wide processing (body resonators,
/// bus drive, master EQ, reverb/chorus returns, the DC blocker) that mixes
/// every source's dry signal before it can be un-mixed. Rather than dumping
/// that whole residual on the default (slot 0) target, this splitter shares
/// it across the sources that produced it, weighted by how much dry energy
/// each one contributed. The weight carries memory (exponential decay,
/// caller-chosen tau) so a body/reverb tail outlives the note that excited
/// it and still lands on that note's own lane rather than falling back to
/// slot 0 the instant the dry voice stops.
///
/// Energy is accumulated from the same values the caller passes to its own
/// add_output() for a dry voice contribution -- never by re-reading a target
/// buffer -- so it does not depend on the engine having zero-cleared targets
/// and stays correct when several voices (or, for LayeredInstrument, several
/// child instruments) write the same target.
///
/// RT contract: no allocation, ever. Capacity is a compile-time bound with
/// headroom over any known MidiInstrumentSourceOutput count.

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "midi/instrument.h"

namespace sonare::midi {

/// Weight-table capacity for SourceResidualSplitter. The engine's current
/// MidiInstrumentSourceOutput bound is TrackMixerRuntime::kMaxTrackLanes + 1
/// (33); this stays generous above that without the midi layer depending on
/// an engine header.
inline constexpr size_t kMaxResidualSources = 64;

/// Fixed chunk length residual attribution runs at, matching Sf2Player's
/// internal bus-graph chunk (kChunkFrames) so both instruments smooth on the
/// same cadence.
inline constexpr int kResidualChunk = 256;

/// Below this, a learned weight is treated as silence rather than carried as
/// a denormal through further exponential decay.
inline constexpr float kResidualWeightFloor = 1e-30f;

/// Per-instrument residual splitter. Not thread-safe -- one instance lives
/// inside one instrument, touched only from that instrument's own render call.
class SourceResidualSplitter {
 public:
  /// CONTROL thread: sets the smoothing time constant and the sample rate the
  /// caller will render at. Call once from prepare().
  void configure(double sample_rate, float tau_seconds) noexcept {
    sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
    tau_seconds_ = tau_seconds > 0.0f ? tau_seconds : 0.5f;
  }

  /// Clears every learned weight and in-flight chunk energy. Call from
  /// prepare() and reset() -- a weight learned before must never bias where a
  /// later, unrelated render's residual lands.
  void reset() noexcept {
    slot_count_ = 0;
    track_ids_.fill(0);
    weights_.fill(0.0f);
    chunk_energy_.fill(0.0f);
  }

  /// AUDIO thread: folds one sample's dry contribution for @p source_track_id
  /// into the in-flight chunk's energy. Call this alongside every add_output()
  /// that lands attributable (dry) voice audio on a source target, passing the
  /// exact same values -- never from the shared bus-wide processing itself.
  void accumulate(uint32_t source_track_id, float l, float r) noexcept {
    const size_t slot = slot_for(source_track_id);
    chunk_energy_[slot] += l * l + r * r;
  }

  /// AUDIO thread: folds the in-flight chunk's energy into each source's
  /// smoothed weight, then splits @p residual_l / @p residual_r (n samples,
  /// n <= kResidualChunk, indexed 0..n-1) across @p outputs in proportion to
  /// the result, adding to target sample @p output_offset + i through the
  /// caller's own add_output so mono-fold and extra-channel fan-out match
  /// every other target write exactly. @p output_offset lets the residual
  /// buffer stay chunk-relative while the target buffers span a whole block
  /// (Sf2Player's own chunk offset; NativeSynth's running chunk start).
  /// Resets the chunk energy accumulator for the next chunk.
  ///
  /// Two exceptions to the proportional split, both required for a
  /// single-source render to reproduce a slot-0-only render bit-for-bit:
  /// no source has contributed energy this block (weights all zero) or
  /// exactly one has -- either way the residual goes to its one live target
  /// unmultiplied rather than through a *1.0 that a slot-0-only render never
  /// performs.
  template <class AddOutputFn>
  void flush(const MidiInstrumentSourceOutput* outputs, size_t output_count, int n,
             const float* residual_l, const float* residual_r, int output_offset,
             AddOutputFn&& add_output) noexcept {
    const double decay_d =
        std::exp(-static_cast<double>(n) / (static_cast<double>(tau_seconds_) * sample_rate_));
    const float decay = static_cast<float>(decay_d);
    float sum_w = 0.0f;
    int nonzero_count = 0;
    size_t nonzero_slot = 0;
    for (size_t s = 0; s < slot_count_; ++s) {
      weights_[s] = weights_[s] * decay + chunk_energy_[s];
      chunk_energy_[s] = 0.0f;
      if (weights_[s] < kResidualWeightFloor) weights_[s] = 0.0f;
    }
    // A source with no target in this block renders its dry voices onto the
    // default target, so its share of the residual belongs there too.
    for (size_t s = 0; s < slot_count_; ++s) {
      if (weights_[s] > 0.0f && track_ids_[s] != 0 &&
          !present(track_ids_[s], outputs, output_count)) {
        const float moved = weights_[s];
        weights_[s] = 0.0f;
        weights_[slot_for(0)] += moved;
      }
    }
    for (size_t s = 0; s < slot_count_; ++s) {
      if (weights_[s] > 0.0f) {
        sum_w += weights_[s];
        nonzero_slot = s;
        ++nonzero_count;
      }
    }

    if (nonzero_count <= 1) {
      float* const* target = nonzero_count == 1
                                 ? target_for(track_ids_[nonzero_slot], outputs, output_count)
                                 : outputs[0].channels;
      for (int i = 0; i < n; ++i) {
        add_output(target, output_offset + i, residual_l[i], residual_r[i]);
      }
      return;
    }
    for (size_t s = 0; s < slot_count_; ++s) {
      if (weights_[s] <= 0.0f) continue;
      const float share = weights_[s] / sum_w;
      float* const* target = target_for(track_ids_[s], outputs, output_count);
      for (int i = 0; i < n; ++i) {
        add_output(target, output_offset + i, residual_l[i] * share, residual_r[i] * share);
      }
    }
  }

 private:
  size_t slot_for(uint32_t source_track_id) noexcept {
    for (size_t i = 0; i < slot_count_; ++i) {
      if (track_ids_[i] == source_track_id) return i;
    }
    // Reuse a slot whose source has fully decayed, so a long session cycling
    // through track ids never exhausts the table.
    for (size_t i = 0; i < slot_count_; ++i) {
      if (weights_[i] == 0.0f && chunk_energy_[i] == 0.0f) {
        track_ids_[i] = source_track_id;
        return i;
      }
    }
    if (slot_count_ < kMaxResidualSources) {
      track_ids_[slot_count_] = source_track_id;
      return slot_count_++;
    }
    // Capacity exhausted: kMaxResidualSources carries headroom over every
    // known MidiInstrumentSourceOutput bound, so this folds into the last
    // slot rather than index out of range.
    return kMaxResidualSources - 1;
  }

  static bool present(uint32_t source_track_id, const MidiInstrumentSourceOutput* outputs,
                      size_t output_count) noexcept {
    // Index 0 is checked too: it is always present (outputs[0].source_track_id
    // is contractually 0), so a slot learned for the default target must not
    // be zeroed here the way an actually-removed lane is.
    for (size_t i = 0; i < output_count; ++i) {
      if (outputs[i].source_track_id == source_track_id) return true;
    }
    return false;
  }

  static float* const* target_for(uint32_t source_track_id,
                                  const MidiInstrumentSourceOutput* outputs,
                                  size_t output_count) noexcept {
    for (size_t i = 1; i < output_count; ++i) {
      if (outputs[i].source_track_id == source_track_id && outputs[i].channels != nullptr) {
        return outputs[i].channels;
      }
    }
    return outputs[0].channels;
  }

  std::array<uint32_t, kMaxResidualSources> track_ids_{};
  std::array<float, kMaxResidualSources> weights_{};
  std::array<float, kMaxResidualSources> chunk_energy_{};
  size_t slot_count_ = 0;
  double sample_rate_ = 48000.0;
  float tau_seconds_ = 0.5f;
};

}  // namespace sonare::midi
