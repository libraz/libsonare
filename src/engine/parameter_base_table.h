#pragma once

/// @file parameter_base_table.h
/// @brief Fixed-capacity audio-thread table of the last manually-set value
///        per parameter id, used to restore a target once the automation
///        lane driving it empties (RealtimeEngine's F7 base-value
///        mechanism). Header-only: no source list entry needed.

#include <cassert>
#include <cstdint>
#include <vector>

namespace sonare::engine {

/// Open-addressing (linear probe, no tombstones -- entries are only ever
/// overwritten, never removed) `uint32_t id -> float` table.
///
/// Capacity is fixed by @ref prepare and never grows afterward, so
/// @ref record and @ref lookup never allocate: both are RT-safe for the
/// audio thread once prepared on the control thread.
class ParameterBaseTable {
 public:
  /// Sizes the table. @p slot_count must be a power of two (linear probing
  /// masks the index) and @p max_entries should sit at a load factor of
  /// roughly 0.5 or below to keep probe chains short. Allocates; call from
  /// the control thread before the audio thread starts calling record() /
  /// lookup(). Safe to call again (e.g. on re-prepare): resets every entry.
  void prepare(uint32_t slot_count, uint32_t max_entries) {
    assert((slot_count & (slot_count - 1)) == 0 && "slot_count must be a power of two");
    assert(max_entries <= slot_count && "max_entries must not exceed slot_count");
    slots_.assign(slot_count, Slot{});
    mask_ = slot_count > 0 ? slot_count - 1 : 0;
    max_entries_ = max_entries;
    count_ = 0;
  }

  /// Records/updates @p id's value. Returns false, recording nothing, when
  /// @p id is the reserved invalid id (0), the table has not been prepared,
  /// or @p id is new and the table is already at its entry cap -- the caller
  /// is expected to surface that last case as telemetry. RT-safe, no alloc.
  bool record(uint32_t id, float value) noexcept {
    if (id == 0 || slots_.empty()) return false;
    uint32_t index = id & mask_;
    for (uint32_t probe = 0; probe < slots_.size(); ++probe) {
      Slot& slot = slots_[index];
      if (slot.occupied && slot.id == id) {
        slot.value = value;
        return true;
      }
      if (!slot.occupied) {
        if (count_ >= max_entries_) return false;
        slot.occupied = true;
        slot.id = id;
        slot.value = value;
        ++count_;
        return true;
      }
      index = (index + 1) & mask_;
    }
    // Every slot probed without an empty one or a match: the entry cap (at
    // or below the slot count's load factor) should always leave room well
    // before this, so this is unreachable in practice, but still reported
    // rather than silently dropped.
    return false;
  }

  /// Looks up @p id's recorded value. Returns false if @p id was never
  /// recorded (or the table has not been prepared). RT-safe, no alloc.
  bool lookup(uint32_t id, float* out_value) const noexcept {
    if (id == 0 || slots_.empty()) return false;
    uint32_t index = id & mask_;
    for (uint32_t probe = 0; probe < slots_.size(); ++probe) {
      const Slot& slot = slots_[index];
      // No tombstones (record() never removes an entry), so the first
      // unoccupied slot on the probe sequence proves id is absent.
      if (!slot.occupied) return false;
      if (slot.id == id) {
        *out_value = slot.value;
        return true;
      }
      index = (index + 1) & mask_;
    }
    return false;
  }

  /// Number of distinct ids currently recorded.
  uint32_t entry_count() const noexcept { return count_; }

 private:
  struct Slot {
    bool occupied = false;
    uint32_t id = 0;
    float value = 0.0f;
  };
  std::vector<Slot> slots_{};
  uint32_t mask_ = 0;
  uint32_t max_entries_ = 0;
  uint32_t count_ = 0;
};

}  // namespace sonare::engine
