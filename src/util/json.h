// Copyright 2026 libsonare contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <ios>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifndef SONARE_JSON_NAMESPACE
#define SONARE_JSON_NAMESPACE sonare::util::json
#endif

namespace SONARE_JSON_NAMESPACE {

class JsonError : public std::runtime_error {
 public:
  JsonError(std::string message, std::size_t position)
      : std::runtime_error(message + " at byte " + std::to_string(position)), position_(position) {}

  std::size_t position() const noexcept { return position_; }

 private:
  std::size_t position_ = 0;
};

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
 public:
  using Variant = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  Value() : value_(nullptr) {}
  Value(std::nullptr_t) : value_(nullptr) {}
  Value(bool value) : value_(value) {}
  Value(int value) : value_(static_cast<double>(value)) {}
  Value(float value) : value_(static_cast<double>(value)) {}
  Value(double value) : value_(value) {}
  Value(const char* value) : value_(std::string(value)) {}
  Value(std::string value) : value_(std::move(value)) {}
  Value(Array value) : value_(std::move(value)) {}
  Value(Object value) : value_(std::move(value)) {}

  bool is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
  bool is_bool() const noexcept { return std::holds_alternative<bool>(value_); }
  bool is_number() const noexcept { return std::holds_alternative<double>(value_); }
  bool is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
  bool is_array() const noexcept { return std::holds_alternative<Array>(value_); }
  bool is_object() const noexcept { return std::holds_alternative<Object>(value_); }

  bool as_bool() const { return std::get<bool>(value_); }
  double as_number() const { return std::get<double>(value_); }
  float as_float() const { return static_cast<float>(std::get<double>(value_)); }
  int as_int() const { return static_cast<int>(std::get<double>(value_)); }
  const std::string& as_string() const { return std::get<std::string>(value_); }
  const Array& as_array() const { return std::get<Array>(value_); }
  const Object& as_object() const { return std::get<Object>(value_); }
  Array& as_array() { return std::get<Array>(value_); }
  Object& as_object() { return std::get<Object>(value_); }

  const Value& operator[](std::size_t index) const { return as_array().at(index); }
  const Value& operator[](const std::string& key) const { return as_object().at(key); }

  bool contains(const std::string& key) const {
    return is_object() && as_object().find(key) != as_object().end();
  }

  const Value* find(const std::string& key) const {
    if (!is_object()) return nullptr;
    const auto& object = as_object();
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
  }

  std::size_t size() const {
    if (is_array()) return as_array().size();
    if (is_object()) return as_object().size();
    return 0;
  }

 private:
  Variant value_;
};

inline void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

inline std::string escape_string(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 2);
  const char* hex = "0123456789abcdef";
  for (unsigned char byte : value) {
    switch (byte) {
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
          out.push_back(static_cast<char>(byte));
        }
        break;
    }
  }
  return out;
}

class Parser {
 public:
  explicit Parser(const std::string& text, std::size_t max_depth = 128,
                  bool reject_duplicate_keys = false)
      : text_(text), max_depth_(max_depth), reject_duplicate_keys_(reject_duplicate_keys) {}

  Value parse_document() {
    // Tolerate a UTF-8 BOM at the start. Windows editors and a few external
    // tools emit "\xEF\xBB\xBF" prefix; the canonical JSON path otherwise
    // returns the unhelpful "unexpected JSON character" error.
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEFu &&
        static_cast<unsigned char>(text_[1]) == 0xBBu &&
        static_cast<unsigned char>(text_[2]) == 0xBFu) {
      pos_ = 3;
    }
    skip_ws();
    Value value = parse_value(0);
    skip_ws();
    if (pos_ != text_.size()) fail("trailing data after JSON document");
    return value;
  }

 private:
  Value parse_value(std::size_t depth) {
    if (depth > max_depth_) fail("JSON nesting depth exceeded");
    skip_ws();
    if (pos_ >= text_.size()) fail("unexpected end of JSON");
    const char c = text_[pos_];
    if (c == '{') return parse_object(depth + 1);
    if (c == '[') return parse_array(depth + 1);
    if (c == '"') return parse_string();
    if (c == 't') return parse_literal("true", Value(true));
    if (c == 'f') return parse_literal("false", Value(false));
    if (c == 'n') return parse_literal("null", Value(nullptr));
    if (c == '-' || is_digit(c)) return parse_number();
    fail("unexpected JSON character");
  }

  Value parse_object(std::size_t depth) {
    Object object;
    expect('{');
    skip_ws();
    if (consume('}')) return object;
    while (true) {
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != '"') fail("expected object key string");
      std::string key = parse_string().as_string();
      expect(':');
      // Default is last-write-wins (std::map::operator[]). When the caller
      // opted into strict mode, reject a repeated key explicitly — schemas
      // that demand a single authoritative value per field (e.g. the EQ band
      // C API) rely on this to fail fast on malformed input.
      if (reject_duplicate_keys_ && object.count(key) != 0) {
        fail("duplicate JSON object key");
      }
      object[std::move(key)] = parse_value(depth);
      skip_ws();
      if (consume('}')) break;
      expect(',');
    }
    return object;
  }

  Value parse_array(std::size_t depth) {
    Array array;
    expect('[');
    skip_ws();
    if (consume(']')) return array;
    while (true) {
      array.push_back(parse_value(depth));
      skip_ws();
      if (consume(']')) break;
      expect(',');
    }
    return array;
  }

  Value parse_string() {
    expect('"');
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (static_cast<unsigned char>(c) < 0x20) fail("control character in JSON string");
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) fail("unterminated JSON escape");
      const char escaped = text_[pos_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escaped);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          std::uint32_t cp = parse_hex4();
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
              fail("missing low surrogate in JSON string");
            }
            pos_ += 2;
            const std::uint32_t low = parse_hex4();
            if (low < 0xDC00 || low > 0xDFFF) fail("invalid low surrogate in JSON string");
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            fail("unexpected low surrogate in JSON string");
          }
          append_utf8(out, cp);
          break;
        }
        default:
          fail("invalid JSON string escape");
      }
    }
    fail("unterminated JSON string");
  }

  Value parse_number() {
    const std::size_t start = pos_;
    // Optional leading minus; the next char (digit) is enforced below.
    (void)consume_raw('-');
    if (consume_raw('0')) {
      if (pos_ < text_.size() && is_digit(text_[pos_])) fail("leading zero in JSON number");
    } else {
      if (pos_ >= text_.size() || !is_digit(text_[pos_])) fail("expected JSON number");
      while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
    }
    if (consume_raw('.')) {
      if (pos_ >= text_.size() || !is_digit(text_[pos_])) fail("expected fraction digits");
      while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || !is_digit(text_[pos_])) fail("expected exponent digits");
      while (pos_ < text_.size() && is_digit(text_[pos_])) ++pos_;
    }
    // Locale-independent parse. std::stod honors LC_NUMERIC; DAW plugin hosts
    // sometimes override that to e.g. de_DE which interprets "," as the decimal
    // separator and would break "1.5". std::from_chars<double> has spotty
    // toolchain support (libc++ < LLVM 20, older Emscripten), so we imbue an
    // istringstream with the classic locale: guaranteed by C++17, portable to
    // every compiler we target.
    std::istringstream ss(text_.substr(start, pos_ - start));
    ss.imbue(std::locale::classic());
    double value = 0.0;
    ss >> value;
    if (!ss || ss.peek() != std::char_traits<char>::eof()) fail("invalid JSON number");
    return value;
  }

  Value parse_literal(const char* literal, Value value) {
    const std::string token(literal);
    if (text_.compare(pos_, token.size(), token) != 0) fail("invalid JSON literal");
    pos_ += token.size();
    return value;
  }

  std::uint32_t parse_hex4() {
    if (pos_ + 4 > text_.size()) fail("truncated unicode escape");
    std::uint32_t cp = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      cp <<= 4;
      if (c >= '0' && c <= '9')
        cp |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        cp |= static_cast<std::uint32_t>(10 + c - 'a');
      else if (c >= 'A' && c <= 'F')
        cp |= static_cast<std::uint32_t>(10 + c - 'A');
      else
        fail("invalid unicode escape");
    }
    return cp;
  }

  void skip_ws() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' ||
                                   text_[pos_] == '\r' || text_[pos_] == '\t')) {
      ++pos_;
    }
  }

  bool consume(char c) {
    skip_ws();
    return consume_raw(c);
  }

  bool consume_raw(char c) {
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect(char c) {
    skip_ws();
    if (!consume_raw(c)) fail(std::string("expected JSON character '") + c + "'");
  }

  static bool is_digit(char c) { return c >= '0' && c <= '9'; }

  [[noreturn]] void fail(const std::string& message) const { throw JsonError(message, pos_); }

  const std::string& text_;
  std::size_t pos_ = 0;
  std::size_t max_depth_ = 128;
  bool reject_duplicate_keys_ = false;
};

inline Value parse(const std::string& text, std::size_t max_depth = 128) {
  return Parser(text, max_depth, /*reject_duplicate_keys=*/false).parse_document();
}

/// @brief Same as @ref parse but rejects objects with duplicate keys.
/// @details Useful for schemas where ambiguity matters (e.g. an EQ band JSON
///          with two `"type"` entries is almost certainly malformed input
///          that the caller should be told about, rather than silently
///          taking the last value).
inline Value parse_strict(const std::string& text, std::size_t max_depth = 128) {
  return Parser(text, max_depth, /*reject_duplicate_keys=*/true).parse_document();
}

inline void dump_value(const Value& value, std::ostringstream& out) {
  if (value.is_null()) {
    out << "null";
  } else if (value.is_bool()) {
    out << (value.as_bool() ? "true" : "false");
  } else if (value.is_number()) {
    // RFC 8259 forbids NaN/Infinity as JSON numbers; std::ostringstream would
    // emit "nan"/"inf" which no spec-compliant parser accepts (including the
    // one in this header). Fail loudly rather than silently producing invalid
    // output that breaks on the next parse round-trip.
    const double number = value.as_number();
    if (!std::isfinite(number)) {
      throw JsonError("cannot serialize non-finite JSON number", 0);
    }
    // max_digits10 (17 for IEEE-754 double) is the minimum precision that
    // guarantees a lossless roundtrip via decimal text. setprecision(15) was
    // exact only for ≤15 significant digits and corrupted full-precision
    // coefficients (e.g. filter design values) on dump→parse cycles.
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << number;
  } else if (value.is_string()) {
    out << '"' << escape_string(value.as_string()) << '"';
  } else if (value.is_array()) {
    out << '[';
    const auto& array = value.as_array();
    for (std::size_t i = 0; i < array.size(); ++i) {
      if (i) out << ',';
      dump_value(array[i], out);
    }
    out << ']';
  } else {
    out << '{';
    bool first = true;
    for (const auto& [key, child] : value.as_object()) {
      if (!first) out << ',';
      first = false;
      out << '"' << escape_string(key) << "\":";
      dump_value(child, out);
    }
    out << '}';
  }
}

inline std::string dump(const Value& value) {
  std::ostringstream out;
  // Imbue classic locale so number formatting uses "." as the decimal separator
  // regardless of the process LC_NUMERIC (matches the parser's behavior).
  out.imbue(std::locale::classic());
  dump_value(value, out);
  return out.str();
}

inline void dump_value_pretty(const Value& value, std::ostringstream& out, int indent,
                              int current_depth) {
  const auto write_indent = [&](int depth) {
    for (int i = 0; i < depth * indent; ++i) out << ' ';
  };
  if (value.is_array()) {
    const auto& array = value.as_array();
    if (array.empty()) {
      out << "[]";
      return;
    }
    out << "[\n";
    for (std::size_t i = 0; i < array.size(); ++i) {
      write_indent(current_depth + 1);
      dump_value_pretty(array[i], out, indent, current_depth + 1);
      if (i + 1 != array.size()) out << ',';
      out << '\n';
    }
    write_indent(current_depth);
    out << ']';
    return;
  }
  if (value.is_object()) {
    const auto& object = value.as_object();
    if (object.empty()) {
      out << "{}";
      return;
    }
    out << "{\n";
    std::size_t emitted = 0;
    for (const auto& [key, child] : object) {
      write_indent(current_depth + 1);
      out << '"' << escape_string(key) << "\": ";
      dump_value_pretty(child, out, indent, current_depth + 1);
      if (++emitted != object.size()) out << ',';
      out << '\n';
    }
    write_indent(current_depth);
    out << '}';
    return;
  }
  dump_value(value, out);
}

/// @brief Serialize @p value with newlines and @p indent spaces of nesting.
/// @details Convenient for human-readable schemas/example files. Reuses the
///          same NaN/Inf rejection as @ref dump.
inline std::string dump_pretty(const Value& value, int indent = 2) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  dump_value_pretty(value, out, indent, 0);
  return out.str();
}

}  // namespace SONARE_JSON_NAMESPACE
