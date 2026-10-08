#pragma once

/// @file instrument_automation_destinations.h
/// @brief Grow-only table behind the slot field of a reserved instrument-automation id.
///
/// Each entry is one destination id, minted when an instrument is first bound to
/// it and never reused for another destination, so an id keeps naming its
/// destination across an unbind/rebind. Minting at bind rather than at resolve
/// gives two engines that receive the same bindings the same numbering, which is
/// what lets a worklet resolve on its main-thread mirror and send the number to
/// the processor's engine. Entries are written on the control thread and
/// published by a release store of the count; the audio thread acquires the
/// count and indexes an entry directly. Storage grows in fixed chunks.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include "engine/instrument_automation_id.h"

namespace sonare::engine {

class InstrumentAutomationDestinationTable {
 public:
  /// One entry per value of the id's 13-bit slot field.
  static constexpr size_t kCapacity = static_cast<size_t>(kInstrumentSlotMask) + 1u;
  static constexpr size_t kChunkSize = 256;

  /// CONTROL thread. Entries that can still be minted.
  size_t remaining() const noexcept { return kCapacity - count_.load(std::memory_order_relaxed); }

  /// CONTROL thread. Slot of @p destination_id, or -1.
  int64_t find(uint32_t destination_id) const noexcept {
    const size_t count = count_.load(std::memory_order_relaxed);
    for (size_t slot = 0; slot < count; ++slot) {
      if (entry_at(slot) == destination_id) return static_cast<int64_t>(slot);
    }
    return -1;
  }

  /// CONTROL thread. Mints a slot for @p destination_id unless it has one. False
  /// when the table is full or a chunk cannot be allocated.
  bool ensure(uint32_t destination_id) noexcept {
    if (find(destination_id) >= 0) return true;
    const size_t count = count_.load(std::memory_order_relaxed);
    if (count >= kCapacity) return false;
    std::unique_ptr<Chunk>& chunk = chunks_[count / kChunkSize];
    if (chunk == nullptr) {
      chunk.reset(new (std::nothrow) Chunk());
      if (chunk == nullptr) return false;
    }
    chunk->destinations[count % kChunkSize] = destination_id;
    // Release: the destination and its chunk must be visible before the count reaches it.
    count_.store(count + 1, std::memory_order_release);
    return true;
  }

  /// Any thread. The destination behind @p slot; false for an unminted slot.
  bool destination(uint32_t slot, uint32_t* out) const noexcept {
    if (slot >= count_.load(std::memory_order_acquire)) return false;
    *out = entry_at(slot);
    return true;
  }

 private:
  struct Chunk {
    std::array<uint32_t, kChunkSize> destinations{};
  };

  uint32_t entry_at(size_t slot) const noexcept {
    return chunks_[slot / kChunkSize]->destinations[slot % kChunkSize];
  }

  std::array<std::unique_ptr<Chunk>, kCapacity / kChunkSize> chunks_{};
  std::atomic<size_t> count_{0};
};

}  // namespace sonare::engine
