/// @file json_reader.h
/// @brief Simple JSON reader for test reference data.

#pragma once

#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace sonare {
namespace test {

class JsonValue;

using JsonNull = std::nullptr_t;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

/// @brief JSON value container.
class JsonValue {
 public:
  using Variant = std::variant<JsonNull, bool, double, std::string, JsonArray, JsonObject>;

  JsonValue() : value_(nullptr) {}
  JsonValue(std::nullptr_t) : value_(nullptr) {}
  JsonValue(bool v) : value_(v) {}
  JsonValue(int v) : value_(static_cast<double>(v)) {}
  JsonValue(double v) : value_(v) {}
  JsonValue(const char* v) : value_(std::string(v)) {}
  JsonValue(const std::string& v) : value_(v) {}
  JsonValue(std::string&& v) : value_(std::move(v)) {}
  JsonValue(const JsonArray& v) : value_(v) {}
  JsonValue(JsonArray&& v) : value_(std::move(v)) {}
  JsonValue(const JsonObject& v) : value_(v) {}
  JsonValue(JsonObject&& v) : value_(std::move(v)) {}

  bool is_null() const { return std::holds_alternative<JsonNull>(value_); }
  bool is_bool() const { return std::holds_alternative<bool>(value_); }
  bool is_number() const { return std::holds_alternative<double>(value_); }
  bool is_string() const { return std::holds_alternative<std::string>(value_); }
  bool is_array() const { return std::holds_alternative<JsonArray>(value_); }
  bool is_object() const { return std::holds_alternative<JsonObject>(value_); }

  bool as_bool() const { return std::get<bool>(value_); }
  double as_number() const { return std::get<double>(value_); }
  float as_float() const { return static_cast<float>(std::get<double>(value_)); }
  int as_int() const { return static_cast<int>(std::get<double>(value_)); }
  const std::string& as_string() const { return std::get<std::string>(value_); }
  const JsonArray& as_array() const { return std::get<JsonArray>(value_); }
  const JsonObject& as_object() const { return std::get<JsonObject>(value_); }

  /// @brief Array access operator.
  const JsonValue& operator[](size_t index) const { return std::get<JsonArray>(value_).at(index); }

  /// @brief Object access operator.
  const JsonValue& operator[](const std::string& key) const {
    return std::get<JsonObject>(value_).at(key);
  }

  /// @brief Check if object contains key.
  bool contains(const std::string& key) const {
    if (!is_object()) return false;
    const auto& obj = std::get<JsonObject>(value_);
    return obj.find(key) != obj.end();
  }

  /// @brief Get array size.
  size_t size() const {
    if (is_array()) return std::get<JsonArray>(value_).size();
    if (is_object()) return std::get<JsonObject>(value_).size();
    return 0;
  }

 private:
  Variant value_;
};

/// @brief Simple JSON parser for test data.
class JsonReader {
 public:
  /// @brief Parse JSON from string.
  static JsonValue parse(const std::string& json) {
    JsonReader reader(json);
    return reader.parse_value();
  }

  /// @brief Parse JSON from file.
  static JsonValue parse_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
      throw std::runtime_error("Cannot open file: " + path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
  }

 private:
  explicit JsonReader(const std::string& json) : json_(json), pos_(0) {}

  JsonValue parse_value() {
    skip_whitespace();
    if (pos_ >= json_.size()) {
      throw std::runtime_error("Unexpected end of JSON");
    }

    char c = json_[pos_];
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    if (c == '-' || std::isdigit(c)) return parse_number();

    throw std::runtime_error(std::string("Unexpected character: ") + c);
  }

  JsonValue parse_object() {
    JsonObject obj;
    expect('{');
    skip_whitespace();

    if (json_[pos_] == '}') {
      ++pos_;
      return obj;
    }

    while (true) {
      skip_whitespace();
      std::string key = parse_string().as_string();
      skip_whitespace();
      expect(':');
      JsonValue value = parse_value();
      obj[key] = std::move(value);

      skip_whitespace();
      if (json_[pos_] == '}') {
        ++pos_;
        break;
      }
      expect(',');
    }
    return obj;
  }

  JsonValue parse_array() {
    JsonArray arr;
    expect('[');
    skip_whitespace();

    if (json_[pos_] == ']') {
      ++pos_;
      return arr;
    }

    while (true) {
      arr.push_back(parse_value());
      skip_whitespace();
      if (json_[pos_] == ']') {
        ++pos_;
        break;
      }
      expect(',');
    }
    return arr;
  }

  JsonValue parse_string() {
    expect('"');
    std::string result;
    while (pos_ < json_.size() && json_[pos_] != '"') {
      if (json_[pos_] == '\\') {
        ++pos_;
        if (pos_ >= json_.size()) break;
        switch (json_[pos_]) {
          case '"':
          case '\\':
          case '/':
            result += json_[pos_];
            break;
          case 'b':
            result += '\b';
            break;
          case 'f':
            result += '\f';
            break;
          case 'n':
            result += '\n';
            break;
          case 'r':
            result += '\r';
            break;
          case 't':
            result += '\t';
            break;
          case 'u':
            // Skip unicode escape for simplicity
            pos_ += 4;
            result += '?';
            break;
          default:
            result += json_[pos_];
        }
      } else {
        result += json_[pos_];
      }
      ++pos_;
    }
    expect('"');
    return result;
  }

  JsonValue parse_number() {
    size_t start = pos_;
    if (json_[pos_] == '-') ++pos_;

    while (pos_ < json_.size() && std::isdigit(json_[pos_])) ++pos_;

    if (pos_ < json_.size() && json_[pos_] == '.') {
      ++pos_;
      while (pos_ < json_.size() && std::isdigit(json_[pos_])) ++pos_;
    }

    if (pos_ < json_.size() && (json_[pos_] == 'e' || json_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < json_.size() && (json_[pos_] == '+' || json_[pos_] == '-')) {
        ++pos_;
      }
      while (pos_ < json_.size() && std::isdigit(json_[pos_])) ++pos_;
    }

    return std::stod(json_.substr(start, pos_ - start));
  }

  JsonValue parse_bool() {
    if (json_.substr(pos_, 4) == "true") {
      pos_ += 4;
      return true;
    }
    if (json_.substr(pos_, 5) == "false") {
      pos_ += 5;
      return false;
    }
    throw std::runtime_error("Invalid boolean");
  }

  JsonValue parse_null() {
    if (json_.substr(pos_, 4) == "null") {
      pos_ += 4;
      return nullptr;
    }
    throw std::runtime_error("Invalid null");
  }

  void skip_whitespace() {
    while (pos_ < json_.size() && (json_[pos_] == ' ' || json_[pos_] == '\t' ||
                                   json_[pos_] == '\n' || json_[pos_] == '\r')) {
      ++pos_;
    }
  }

  void expect(char c) {
    if (pos_ >= json_.size() || json_[pos_] != c) {
      throw std::runtime_error(std::string("Expected '") + c + "'");
    }
    ++pos_;
  }

  const std::string& json_;
  size_t pos_;
};

}  // namespace test
}  // namespace sonare
