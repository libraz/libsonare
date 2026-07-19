#pragma once

/// @file coremidi_output_state.h
/// @brief SDK-free retry state for bounded CoreMIDI SysEx7 transmission.

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

}  // namespace sonare::host::backends::detail
