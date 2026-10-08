#pragma once

/// @file insert_automation_targets.h
/// @brief Grow-only table behind the strip field of a reserved insert-automation id.
///
/// Every insert id names one entry: the strip (track, bus or master) by identity,
/// the insert slot, and the parameter layout of the processor in that slot (its
/// dynamic type plus ProcessorBase::parameter_layout_variant()). A track lane's
/// fader and pan, and a bus's fader, share the table as one slot-less entry per
/// track or bus. Entries are minted when the mixer is configured, in a canonical
/// order (each lane's fader/pan entry then its slots, each bus's fader entry then
/// its slots, then master, slots in index order), so two engines that receive
/// the same configuration sequence number them identically; resolving an id only
/// looks one up. An entry is retired when its track or bus leaves the mixer or
/// its slot comes to hold a processor of another layout, and it is never reused,
/// so an id outlives a lane or bus reorder but never reaches another strip or
/// another processor. Entries are written on the control thread and published by
/// a release store of the count, the instrument destination table's pattern; the
/// audio thread acquires the count and indexes an entry directly. Storage grows
/// in fixed chunks so an engine with few inserts does not carry the whole 13-bit
/// selector namespace.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <typeinfo>

#include "engine/insert_automation_id.h"
#include "rt/processor_base.h"

namespace sonare::engine {

/// Which strip family an insert-automation entry addresses. kTrackLane (a track
/// lane's fader and pan) and kBusLane (a bus's fader) are not insert slots: their
/// insert index is 0, their processor layout empty, and the id's param field a
/// TrackMixerRuntime::ParamId.
enum class InsertStripKind : uint8_t { kTrack, kBus, kMaster, kTrackLane, kBusLane };

/// True for the strip families whose entry names the strip's own controls, not a slot.
constexpr bool insert_strip_slotless(InsertStripKind strip) noexcept {
  return strip == InsertStripKind::kTrackLane || strip == InsertStripKind::kBusLane;
}

/// What gives a processor's param ids their meaning. Empty for an empty slot.
struct InsertProcessorLayout {
  const std::type_info* type = nullptr;
  uint64_t variant = 0;

  bool empty() const noexcept { return type == nullptr; }
  bool operator==(const InsertProcessorLayout& other) const noexcept {
    if (type == nullptr || other.type == nullptr) return type == other.type;
    return *type == *other.type && variant == other.variant;
  }
  bool operator!=(const InsertProcessorLayout& other) const noexcept { return !(*this == other); }
};

/// Layout of @p processor, or an empty layout for an empty slot. Allocation free.
inline InsertProcessorLayout insert_processor_layout(const rt::ProcessorBase* processor) noexcept {
  if (processor == nullptr) return {};
  return {&typeid(*processor), processor->parameter_layout_variant()};
}

/// The (strip, insert slot, processor layout) an insert id addresses.
struct InsertAutomationTarget {
  InsertStripKind strip = InsertStripKind::kTrack;
  /// Track id or bus id; 0 for the master strip.
  uint32_t owner_id = 0;
  unsigned int insert_index = 0;
  /// Layout of the processor the slot held when the entry was minted.
  InsertProcessorLayout processor{};

  bool same_owner(InsertStripKind kind, uint32_t owner) const noexcept {
    return strip == kind && owner_id == owner;
  }
  bool same_slot(const InsertAutomationTarget& other) const noexcept {
    return same_owner(other.strip, other.owner_id) && insert_index == other.insert_index;
  }
};

class InsertAutomationTargetTable {
 public:
  /// One entry per value of the id's 13-bit strip field.
  static constexpr size_t kCapacity = static_cast<size_t>(kInsertStripMask) + 1u;
  static constexpr size_t kChunkSize = 256;

  /// CONTROL thread. Entries that can still be minted.
  size_t remaining() const noexcept { return kCapacity - count_.load(std::memory_order_relaxed); }

  /// CONTROL thread. Selector of the live entry equal to @p target, or -1.
  int64_t find(const InsertAutomationTarget& target) const noexcept {
    const size_t count = count_.load(std::memory_order_relaxed);
    for (size_t selector = 0; selector < count; ++selector) {
      const Entry& entry = entry_at(selector);
      if (entry.live.load(std::memory_order_relaxed) && entry.target.same_slot(target) &&
          entry.target.processor == target.processor) {
        return static_cast<int64_t>(selector);
      }
    }
    return -1;
  }

  /// CONTROL thread. Returns the live entry for @p target, minting one when none
  /// exists. False when the table is full or a chunk cannot be allocated.
  bool ensure(const InsertAutomationTarget& target) noexcept {
    if (target.processor.empty() != insert_strip_slotless(target.strip)) return false;
    if (find(target) >= 0) return true;
    const size_t count = count_.load(std::memory_order_relaxed);
    if (count >= kCapacity) return false;
    std::unique_ptr<Chunk>& chunk = chunks_[count / kChunkSize];
    if (chunk == nullptr) {
      chunk.reset(new (std::nothrow) Chunk());
      if (chunk == nullptr) return false;
    }
    Entry& entry = chunk->entries[count % kChunkSize];
    entry.target = target;
    entry.live.store(true, std::memory_order_relaxed);
    // Release: the entry and its chunk must be visible before the count reaches it.
    count_.store(count + 1, std::memory_order_release);
    return true;
  }

  /// Any thread. The live entry behind @p selector, or nullptr for an unminted
  /// or retired one.
  const InsertAutomationTarget* live(uint32_t selector) const noexcept {
    if (selector >= count_.load(std::memory_order_acquire)) return nullptr;
    const Entry& entry = entry_at(selector);
    return entry.live.load(std::memory_order_acquire) ? &entry.target : nullptr;
  }

  /// CONTROL thread. The entry behind @p selector whether live or retired, for
  /// purges that must also reach retired ids.
  const InsertAutomationTarget* minted(uint32_t selector) const noexcept {
    if (selector >= count_.load(std::memory_order_acquire)) return nullptr;
    return &entry_at(selector).target;
  }

  /// CONTROL thread. Retires every live entry for which @p predicate(target) is
  /// true. Returns the number retired.
  template <typename Predicate>
  size_t retire_if(Predicate&& predicate) noexcept {
    size_t retired = 0;
    const size_t count = count_.load(std::memory_order_relaxed);
    for (size_t selector = 0; selector < count; ++selector) {
      Entry& entry = entry_at(selector);
      if (!entry.live.load(std::memory_order_relaxed) || !predicate(entry.target)) continue;
      entry.live.store(false, std::memory_order_release);
      ++retired;
    }
    return retired;
  }

  /// CONTROL thread. True when any entry, live or retired, satisfies @p predicate.
  template <typename Predicate>
  bool any_minted(Predicate&& predicate) const noexcept {
    const size_t count = count_.load(std::memory_order_relaxed);
    for (size_t selector = 0; selector < count; ++selector) {
      if (predicate(entry_at(selector).target)) return true;
    }
    return false;
  }

 private:
  struct Entry {
    InsertAutomationTarget target{};
    std::atomic<bool> live{false};
  };
  struct Chunk {
    std::array<Entry, kChunkSize> entries{};
  };

  Entry& entry_at(size_t selector) noexcept {
    return chunks_[selector / kChunkSize]->entries[selector % kChunkSize];
  }
  const Entry& entry_at(size_t selector) const noexcept {
    return chunks_[selector / kChunkSize]->entries[selector % kChunkSize];
  }

  std::array<std::unique_ptr<Chunk>, kCapacity / kChunkSize> chunks_{};
  std::atomic<size_t> count_{0};
};

}  // namespace sonare::engine
