#include "midi/sysex_framing.h"

namespace sonare::midi {

namespace {

constexpr uint8_t kSysExStartByte = 0xF0u;
constexpr uint8_t kSysExEndByte = 0xF7u;

}  // namespace

std::vector<uint8_t> sysex_payload_from_body(const uint8_t* body, size_t size, bool terminated) {
  std::vector<uint8_t> payload;
  if (body == nullptr) size = 0;
  const bool frame_start = size > 0 && is_sysex_start_byte(body[0]);
  const bool frame_end = terminated || (size > 0 && is_sysex_end_byte(body[size - 1]));
  payload.reserve(size + (frame_start ? 1u : 0u) + (frame_end ? 1u : 0u));
  if (frame_start) payload.push_back(kSysExStartByte);
  if (size > 0) payload.insert(payload.end(), body, body + size);
  if (frame_end) payload.push_back(kSysExEndByte);
  return payload;
}

SmfSysExAssembler::Outcome SmfSysExAssembler::feed(uint8_t status, const uint8_t* bytes,
                                                   size_t size, double ppq) {
  Outcome outcome;
  if (bytes == nullptr) size = 0;
  if (is_sysex_start_byte(status)) {
    // A new message; an unfinished one cannot be continued by it.
    outcome.abandoned = interrupt();
    bytes_.assign(bytes, bytes + size);
    ppq_ = ppq;
    pending_ = true;
  } else if (pending_) {
    // Continuation packet. An empty one carries nothing and finishes nothing.
    bytes_.insert(bytes_.end(), bytes, bytes + size);
  } else {
    // Independent escape: its bytes are the whole message. An empty escape sends nothing,
    // and one carrying its own F0 has that byte as framing.
    if (size == 0) return outcome;
    const size_t skip = is_sysex_start_byte(bytes[0]) ? 1u : 0u;
    bytes_.assign(bytes + skip, bytes + size);
    ppq_ = ppq;
    completed_ = true;
    outcome.completed = true;
    return outcome;
  }
  if (!bytes_.empty() && is_sysex_end_byte(bytes_.back())) {
    pending_ = false;
    completed_ = true;
    outcome.completed = true;
  }
  return outcome;
}

uint32_t SmfSysExAssembler::interrupt() noexcept {
  if (!pending_) return 0;
  pending_ = false;
  bytes_.clear();
  return 1;
}

std::vector<uint8_t> SmfSysExAssembler::take(double* ppq) {
  if (ppq != nullptr) *ppq = ppq_;
  if (!completed_) return {};
  completed_ = false;
  const bool terminated = !bytes_.empty() && is_sysex_end_byte(bytes_.back());
  const size_t body_size = bytes_.size() - (terminated ? 1u : 0u);
  std::vector<uint8_t> payload = sysex_payload_from_body(bytes_.data(), body_size, terminated);
  bytes_.clear();
  return payload;
}

}  // namespace sonare::midi
