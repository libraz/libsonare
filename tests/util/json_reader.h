/// @file json_reader.h
/// @brief Test JSON reader adapter backed by the shared util JSON parser.

#pragma once

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "util/json.h"

namespace sonare::test {

using JsonValue = sonare::util::json::Value;
using JsonNull = std::nullptr_t;
using JsonArray = sonare::util::json::Array;
using JsonObject = sonare::util::json::Object;

class JsonReader {
 public:
  static JsonValue parse(const std::string& json) { return sonare::util::json::parse(json); }

  static JsonValue parse_file(const std::string& path) {
    const std::string resolved_path = resolve_reference_path(path);
    std::ifstream file(resolved_path);
    if (!file.is_open()) {
      throw std::runtime_error("Cannot open file: " + resolved_path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
  }

 private:
  static std::string resolve_reference_path(const std::string& path) {
    constexpr const char* prefix = "tests/librosa/reference/";
    constexpr size_t prefix_len = 24;

    if (path.rfind(prefix, 0) != 0) return path;

    const char* override_dir = std::getenv("SONARE_LIBROSA_REFERENCE_DIR");
    if (override_dir == nullptr || override_dir[0] == '\0') return path;

    std::string dir(override_dir);
    if (!dir.empty() && dir.back() != '/') dir.push_back('/');
    return dir + path.substr(prefix_len);
  }
};

}  // namespace sonare::test
