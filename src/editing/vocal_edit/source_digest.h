#pragma once

/// @file source_digest.h
/// @brief The one canonical digest of mono float32 source PCM.
///
/// Each sample's IEEE-754 bits are hashed little-endian whatever the host byte order, so a
/// session's source descriptor and a project's PCM digest compare equal across machines.
/// Header-only so project code built without the pitch editor shares it.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "util/sha256.h"

namespace sonare::editing::vocal_edit {

/// @brief SHA-256 over @p count samples, each serialized as little-endian float32 bits.
inline std::array<uint8_t, 32> digest_source_pcm(const float* samples, std::size_t count) {
  util::Sha256 hash;
  for (std::size_t i = 0; i < count; ++i) {
    uint32_t bits = 0;
    std::memcpy(&bits, &samples[i], sizeof(bits));
    const uint8_t bytes[4] = {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8),
                              static_cast<uint8_t>(bits >> 16), static_cast<uint8_t>(bits >> 24)};
    hash.update(bytes, sizeof(bytes));
  }
  return hash.finalize();
}

}  // namespace sonare::editing::vocal_edit
