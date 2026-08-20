#pragma once

/// @file coremidi_output_state.h
/// @brief SDK-free retry state for bounded CoreMIDI transmission: SysEx7
///        payloads and fixed-size UMP batches.

#include <cstddef>
#include <cstdint>
#include <utility>

#include "midi/ump.h"

namespace sonare::host::backends::detail {

enum class SysExFlushStatus {
  kComplete,
  kRetry,
  kInvalid,
};

/// Sends a SysEx7 payload in bounded batches without advancing the committed
/// packet cursor until a batch is accepted. A failed sender therefore resumes
/// at the first unsent packet and never duplicates a successful prefix.
template <typename SendBatch>
SysExFlushStatus flush_sysex7_payload(const uint8_t* payload, size_t payload_size, uint8_t group,
                                      size_t* packet_position, midi::Ump* scratch,
                                      size_t scratch_capacity, SendBatch&& send_batch) noexcept {
  if (packet_position == nullptr || scratch == nullptr || scratch_capacity == 0) {
    return SysExFlushStatus::kInvalid;
  }
  midi::SysEx7Packetizer packetizer(payload, payload_size, group);
  if (!packetizer.valid() || !packetizer.seek_packet(*packet_position)) {
    return SysExFlushStatus::kInvalid;
  }

  while (packetizer.remaining_packets() > 0) {
    midi::SysEx7Packetizer next = packetizer;
    const size_t count = next.read(scratch, scratch_capacity);
    if (count == 0) return SysExFlushStatus::kInvalid;
    if (!std::forward<SendBatch>(send_batch)(scratch, count)) {
      *packet_position = packetizer.packet_position();
      return SysExFlushStatus::kRetry;
    }
    packetizer = next;
    *packet_position = packetizer.packet_position();
  }
  return SysExFlushStatus::kComplete;
}

enum class BatchFlushStatus {
  kSent,
  kRejected,
  kRetry,
};

struct BatchFlushResult {
  BatchFlushStatus status = BatchFlushStatus::kRejected;
  /// Events packed into the list. Non-zero only for kSent / kRetry.
  size_t batch_count = 0;
};

/// Packs up to `available` adjacent fixed-size events into one event list and
/// sends it. `add_event(index)` reports whether the event at `index` joined the
/// list; it returns false for an event that cannot be batched here (a SysEx or
/// empty record), for a full list, and for a packet the SDK refuses.
///
/// The head event is the case that matters: refused on an empty list, it can
/// never be packed, so retrying it would block every event behind it forever.
/// That is reported as kRejected, which the caller consumes like an invalid
/// SysEx. A refused `send_list()` on a non-empty list is a transport failure and
/// stays retryable (kRetry), leaving the batch queued.
template <typename AddEvent, typename SendList>
BatchFlushResult flush_fixed_batch(size_t available, AddEvent&& add_event,
                                   SendList&& send_list) noexcept {
  BatchFlushResult result;
  while (result.batch_count < available && add_event(result.batch_count)) ++result.batch_count;
  if (result.batch_count == 0) {
    result.status = BatchFlushStatus::kRejected;
    return result;
  }
  result.status =
      std::forward<SendList>(send_list)() ? BatchFlushStatus::kSent : BatchFlushStatus::kRetry;
  return result;
}

}  // namespace sonare::host::backends::detail
