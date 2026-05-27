#pragma once

#include <string>

namespace sonare::util {

inline std::string escape_json_string(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  const char* hex = "0123456789abcdef";
  for (char c : value) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (byte < 0x20) {
          out += "\\u00";
          out += hex[(byte >> 4) & 0x0F];
          out += hex[byte & 0x0F];
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

}  // namespace sonare::util
