#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonare {

/// Maps a standard base64 character to its 0..63 value, or 0xFF when the
/// character is not part of the standard alphabet. Returning a sentinel (rather
/// than reading out of bounds) lets the decoder report malformed input via its
/// bool result instead of faulting.
inline uint8_t base64_char_value(char c) {
  if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 26);
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  return 0xFF;
}

/// Decodes a standard-alphabet base64 string into raw bytes. Returns false (and
/// leaves *out empty) on any malformed length, character, or padding so callers
/// can fail gracefully. Padding may only appear in the final quad, as '=' alone
/// or '==' together.
inline bool base64_decode(const std::string& text, std::vector<uint8_t>* out) {
  out->clear();
  if (text.size() % 4 != 0) return false;
  out->reserve((text.size() / 4) * 3);
  for (size_t i = 0; i < text.size(); i += 4) {
    const char c0 = text[i];
    const char c1 = text[i + 1];
    const char c2 = text[i + 2];
    const char c3 = text[i + 3];
    const uint8_t v0 = base64_char_value(c0);
    const uint8_t v1 = base64_char_value(c1);
    if (v0 == 0xFF || v1 == 0xFF) return false;
    const bool pad2 = (c2 == '=');
    const bool pad3 = (c3 == '=');
    // Padding may only appear in the final quad, c3 alone or c2+c3 together.
    if ((pad2 || pad3) && i + 4 != text.size()) return false;
    if (pad2 && !pad3) return false;
    uint32_t triple = (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12);
    out->push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
    if (!pad2) {
      const uint8_t v2 = base64_char_value(c2);
      if (v2 == 0xFF) return false;
      triple |= static_cast<uint32_t>(v2) << 6;
      out->push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
      if (!pad3) {
        const uint8_t v3 = base64_char_value(c3);
        if (v3 == 0xFF) return false;
        triple |= static_cast<uint32_t>(v3);
        out->push_back(static_cast<uint8_t>(triple & 0xFF));
      }
    }
  }
  return true;
}

}  // namespace sonare
