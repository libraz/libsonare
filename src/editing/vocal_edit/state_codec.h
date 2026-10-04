#pragma once

/// @file state_codec.h
/// @brief Strict little-endian SVE1 value-state codec for vocal editing.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "editing/vocal_edit/types.h"

namespace sonare::editing::vocal_edit {

inline constexpr uint32_t kVocalStateSchemaVersion = 1;

/// @brief Persisted value state. Runtime PCM, caches, drafts and history are
/// deliberately absent from this type and from the SVE1 wire format.
struct VocalPersistedState {
  VocalSessionId session_id{};
  uint64_t next_note_id = 1;
  VocalRevision committed_revision = 0;
  SourceDescriptor source{};
  int64_t output_length_samples = 0;
  VocalAnalysisData analysis{};
  VocalEditState edit_state{};
  RenderSettings render_settings{};
};

/// @brief Encodes one validated SVE1 state using canonical little-endian bytes.
std::vector<uint8_t> encode_vocal_state(const VocalPersistedState& state);

/// @brief Decodes one complete SVE1 state and rejects malformed input.
VocalPersistedState decode_vocal_state(const uint8_t* bytes, size_t size);

inline VocalPersistedState decode_vocal_state(const std::vector<uint8_t>& bytes) {
  return decode_vocal_state(bytes.data(), bytes.size());
}

}  // namespace sonare::editing::vocal_edit
