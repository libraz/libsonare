#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace sonare::detail {

struct Id3RawTags {
  std::vector<std::pair<std::string, std::string>> v2;
  std::vector<std::pair<std::string, std::string>> v1;
};

/// Appends a code point from a WAV or ID3 tag as UTF-8.
void append_tag_utf8(std::string& out, uint32_t codepoint);

Id3RawTags read_id3_raw_tags(const uint8_t* data, size_t size);

}  // namespace sonare::detail
