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

Id3RawTags read_id3_raw_tags(const uint8_t* data, size_t size);

}  // namespace sonare::detail
