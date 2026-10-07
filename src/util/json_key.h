#pragma once

/// @file json_key.h
/// @brief Key rules shared by every reader that reports or refuses an unknown JSON key.

#include <string>

namespace sonare::util::json {

/// @brief True for an annotation key (`$`- or `x-`-prefixed), which a reader never
///        reports or refuses as unknown.
inline bool is_annotation_key(const std::string& key) {
  return key.compare(0, 1, "$") == 0 || key.compare(0, 2, "x-") == 0;
}

/// @brief Renders a key for a one-line message: control characters are escaped as
///        `\xNN` so a key cannot split a newline-joined channel into forged entries.
inline std::string escape_key(const std::string& key) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  for (const char c : key) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7f) {
      out += "\\x";
      out += kHex[u >> 4];
      out += kHex[u & 0xf];
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace sonare::util::json
