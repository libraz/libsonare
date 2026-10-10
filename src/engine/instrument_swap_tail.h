#pragma once

/// @file instrument_swap_tail.h
/// @brief Per-destination fade-out tails that keep a hosted instrument's output
///        continuous when the instrument on a destination is swapped or cleared.
///
/// The engine renders the outgoing instrument for a short fade window on the
/// CONTROL thread at swap time, applies a gain ramp that ends at exactly zero,
/// and stores the result here. The AUDIO thread only sums the remaining frames
/// into the destination's instrument output and advances the cursor, so the
/// outgoing instrument is never touched once the swap returns. A tail rendered
/// through the source-aware path holds one plane per source track id; a
/// destination-path tail holds a single plane.
///
/// Each entry also records whether its bound instrument has rendered on the
/// engine since it was bound. Only such an instrument is pre-rendered, so a
/// bind followed by a clear with no audio in between never advances it.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/instrument_rack.h"

namespace sonare::engine {

/// Fixed table of per-destination swap tails. Storage is grown on the CONTROL
/// thread only; the AUDIO thread reads, sums and advances.
class InstrumentSwapTails {
 public:
  /// Bound destinations plus as many cleared destinations still fading out.
  static constexpr size_t kSlots = 2 * InstrumentRack::kMaxInstruments;

  struct Entry {
    uint32_t destination_id = 0;
    /// An instrument is bound to the destination.
    bool bound = false;
    /// AUDIO thread: the bound instrument rendered since it was bound.
    bool rendered_since_bind = false;
    /// AUDIO thread: the branch and channel count of that last render.
    bool last_render_source_aware = false;
    int last_render_channels = 0;

    bool tail_source_aware = false;
    int tail_channels = 0;
    int tail_frames = 0;
    int tail_cursor = 0;
    /// Source track id of each plane (source-aware tails only).
    std::vector<uint32_t> tail_track_ids;
    /// Planar samples laid out as [plane][channel][frame].
    std::vector<float> tail_samples;

    bool tail_pending() const noexcept { return tail_cursor < tail_frames; }
    bool in_use() const noexcept { return bound || tail_pending(); }
    size_t tail_planes() const noexcept {
      return tail_source_aware ? tail_track_ids.size() : static_cast<size_t>(1);
    }
    /// Plane @p plane, channel @p channel, starting at the current cursor.
    const float* tail_at(size_t plane, int channel) const noexcept {
      const size_t frames = static_cast<size_t>(tail_frames);
      return tail_samples.data() +
             (plane * static_cast<size_t>(tail_channels) + static_cast<size_t>(channel)) * frames +
             static_cast<size_t>(tail_cursor);
    }
    void drop_tail() noexcept { tail_cursor = tail_frames; }
  };

  /// Entry in use for @p destination_id, or nullptr.
  Entry* find(uint32_t destination_id) noexcept {
    for (Entry& entry : entries_) {
      if (entry.in_use() && entry.destination_id == destination_id) return &entry;
    }
    return nullptr;
  }

  /// CONTROL thread: an instrument was bound to @p destination_id. Claims an
  /// entry, evicting a cleared destination's tail only when every slot is taken.
  void on_bound(uint32_t destination_id) noexcept {
    Entry* entry = claim(destination_id);
    if (entry == nullptr) return;
    entry->bound = true;
    entry->rendered_since_bind = false;
  }

  /// CONTROL thread: the destination's instrument was cleared.
  void on_unbound(uint32_t destination_id) noexcept {
    if (Entry* entry = find(destination_id)) {
      entry->bound = false;
      entry->rendered_since_bind = false;
    }
  }

  /// AUDIO thread: @p entry's bound instrument rendered a block.
  static void mark_rendered(Entry* entry, bool source_aware, int channels) noexcept {
    if (entry == nullptr) return;
    entry->rendered_since_bind = true;
    entry->last_render_source_aware = source_aware;
    entry->last_render_channels = channels;
  }

  /// True when any destination still has tail frames to play.
  bool any_pending() const noexcept {
    for (const Entry& entry : entries_) {
      if (entry.tail_pending()) return true;
    }
    return false;
  }

  /// Invoke `fn(destination_id)` for every unbound destination with a pending tail.
  template <typename Fn>
  void for_each_unbound_pending(Fn&& fn) {
    for (Entry& entry : entries_) {
      if (!entry.bound && entry.tail_pending()) fn(entry.destination_id);
    }
  }

  /// AUDIO thread: adds the tail plane @p plane into @p out at the cursor.
  static void add_plane(const Entry& entry, size_t plane, float* const* out, int channels,
                        int num_frames) noexcept {
    const int frames = std::min(num_frames, entry.tail_frames - entry.tail_cursor);
    const int shared = std::min(channels, entry.tail_channels);
    for (int ch = 0; ch < shared; ++ch) {
      float* dst = out[static_cast<size_t>(ch)];
      if (dst == nullptr) continue;
      const float* src = entry.tail_at(plane, ch);
      for (int i = 0; i < frames; ++i) dst[i] += src[i];
    }
  }

  /// AUDIO thread: moves the cursor past @p num_frames played frames.
  static void advance(Entry& entry, int num_frames) noexcept {
    entry.tail_cursor = std::min(entry.tail_frames, entry.tail_cursor + std::max(num_frames, 0));
  }

  /// Drops every pending tail; storage keeps its capacity. RT-safe.
  void discard_tails() noexcept {
    for (Entry& entry : entries_) entry.drop_tail();
  }

  /// Ends the output stream: drops every tail and forgets every render, so a
  /// later swap has nothing to continue and never renders the outgoing instrument.
  void end_stream() noexcept {
    for (Entry& entry : entries_) {
      entry.drop_tail();
      entry.rendered_since_bind = false;
    }
  }

  /// CONTROL thread: drops every tail and gives its storage back.
  void release_storage() noexcept {
    for (Entry& entry : entries_) {
      entry.drop_tail();
      std::vector<float>().swap(entry.tail_samples);
      std::vector<uint32_t>().swap(entry.tail_track_ids);
    }
  }

  /// Samples held in tail storage.
  size_t reserved_samples() const noexcept {
    size_t samples = 0;
    for (const Entry& entry : entries_) samples += entry.tail_samples.capacity();
    return samples;
  }

 private:
  Entry* claim(uint32_t destination_id) noexcept {
    if (Entry* entry = find(destination_id)) return entry;
    Entry* chosen = nullptr;
    for (Entry& entry : entries_) {
      if (!entry.in_use()) {
        chosen = &entry;
        break;
      }
    }
    if (chosen == nullptr) {
      for (Entry& entry : entries_) {
        if (!entry.bound) {
          chosen = &entry;
          break;
        }
      }
    }
    if (chosen == nullptr) return nullptr;
    chosen->drop_tail();
    chosen->destination_id = destination_id;
    chosen->bound = false;
    chosen->rendered_since_bind = false;
    chosen->last_render_source_aware = false;
    chosen->last_render_channels = 0;
    return chosen;
  }

  std::array<Entry, kSlots> entries_{};
};

}  // namespace sonare::engine
