#pragma once

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

#include "mastering/api/named_processor.h"

namespace sonare::mastering::api::detail {

class JsonParamReader {
 public:
  explicit JsonParamReader(const std::string& text) : text_(text) {}

  std::vector<Param> parse_flat_object_document(bool allow_empty) {
    std::vector<Param> params;
    skip_ws();
    if (pos_ >= text_.size()) {
      if (allow_empty) return params;
      fail("expected JSON object");
    }
    params = parse_flat_object();
    skip_ws();
    if (!at_end()) fail("trailing data after JSON object");
    return params;
  }

  std::vector<Param> parse_flat_object() {
    std::vector<Param> params;
    expect('{');
    while (!consume('}')) {
      std::string key = parse_string();
      expect(':');
      params.push_back(Param{std::move(key), parse_bool_or_number()});
      if (!consume(',')) {
        expect('}');
        break;
      }
    }
    return params;
  }

  std::string parse_string() {
    skip_ws();
    expect_raw('"');
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (c == '\\') {
        if (pos_ >= text_.size()) fail("unterminated JSON escape");
        const char escaped = text_[pos_++];
        if (escaped == '"' || escaped == '\\' || escaped == '/') {
          out.push_back(escaped);
        } else if (escaped == 'n') {
          out.push_back('\n');
        } else if (escaped == 't') {
          out.push_back('\t');
        } else {
          fail("unsupported JSON string escape");
        }
      } else {
        out.push_back(c);
      }
    }
    fail("unterminated JSON string");
  }

  double parse_bool_or_number() {
    skip_ws();
    if (peek("true")) {
      pos_ += 4;
      return 1.0;
    }
    if (peek("false")) {
      pos_ += 5;
      return 0.0;
    }
    return parse_number();
  }

  double parse_number() {
    skip_ws();
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (start == pos_) fail("expected JSON number");
    return std::stod(text_.substr(start, pos_ - start));
  }

  bool consume(char c) {
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  void expect(char c) {
    skip_ws();
    expect_raw(c);
  }

  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  bool at_end() const noexcept { return pos_ == text_.size(); }

 private:
  [[noreturn]] void fail(const std::string& message) const { throw std::invalid_argument(message); }

  void expect_raw(char c) {
    if (pos_ >= text_.size() || text_[pos_] != c) {
      fail(std::string("expected JSON character: ") + c);
    }
    ++pos_;
  }

  bool peek(const char* literal) const {
    const std::string value(literal);
    return text_.compare(pos_, value.size(), value) == 0;
  }

  const std::string& text_;
  size_t pos_ = 0;
};

inline std::vector<Param> parse_flat_json_params(const std::string& text, bool allow_empty) {
  JsonParamReader reader(text);
  return reader.parse_flat_object_document(allow_empty);
}

}  // namespace sonare::mastering::api::detail
