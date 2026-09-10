#include "sonare_cli_json.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>

JsonBuilder::JsonBuilder() {
  // A std::ostringstream formats through the global C++ locale, not through
  // LC_NUMERIC, so a host that has run std::locale::global with a de_DE-style
  // locale makes every `--json` command emit `"lufs": -14,5` -- syntactically
  // invalid JSON that no parser accepts, from a CLI that reports success. The
  // matching policy for the core serializer is in util/json.h.
  ss_.imbue(std::locale::classic());
}

JsonBuilder& JsonBuilder::begin_object() {
  append_separator();
  ss_ << "{";
  needs_comma_.push_back(false);
  return *this;
}

JsonBuilder& JsonBuilder::end_object() {
  ss_ << "}";
  needs_comma_.pop_back();
  if (!needs_comma_.empty()) needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::begin_array() {
  append_separator();
  ss_ << "[";
  needs_comma_.push_back(false);
  return *this;
}

JsonBuilder& JsonBuilder::end_array() {
  ss_ << "]";
  needs_comma_.pop_back();
  if (!needs_comma_.empty()) needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::key(const std::string& k) {
  append_separator();
  ss_ << "\"" << escape(k) << "\": ";
  needs_comma_.back() = false;
  return *this;
}

JsonBuilder& JsonBuilder::value(const std::string& v) {
  append_separator();
  ss_ << "\"" << escape(v) << "\"";
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(const char* v) { return value(std::string(v)); }

JsonBuilder& JsonBuilder::value(int v) {
  append_separator();
  ss_ << v;
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(size_t v) {
  append_separator();
  ss_ << v;
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(float v) {
  append_separator();
  // RFC 8259 forbids NaN/Infinity as JSON numbers; emit `null` (as util/json.h
  // does) so a non-finite reading -- e.g. a -inf LUFS/true-peak for a fully
  // silent input -- yields valid JSON that json.loads/jq/JSON.parse can read.
  if (std::isfinite(v)) {
    ss_ << std::setprecision(std::numeric_limits<float>::max_digits10) << v;
  } else {
    ss_ << "null";
  }
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(double v) {
  append_separator();
  // See value(float): non-finite numbers serialize as JSON null, not "nan"/"inf".
  if (std::isfinite(v)) {
    ss_ << std::setprecision(std::numeric_limits<double>::max_digits10) << v;
  } else {
    ss_ << "null";
  }
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::value(bool v) {
  append_separator();
  ss_ << (v ? "true" : "false");
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::null_value() {
  append_separator();
  ss_ << "null";
  needs_comma_.back() = true;
  return *this;
}

JsonBuilder& JsonBuilder::kv(const std::string& k, const std::string& v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, const char* v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, int v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, size_t v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, float v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, double v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::kv(const std::string& k, bool v) { return key(k).value(v); }

JsonBuilder& JsonBuilder::float_array(const std::vector<float>& arr) {
  begin_array();
  for (float v : arr) value(v);
  end_array();
  return *this;
}

std::string JsonBuilder::build() const { return ss_.str(); }

void JsonBuilder::print() const { std::cout << ss_.str() << "\n"; }

void JsonBuilder::append_separator() {
  if (!needs_comma_.empty() && needs_comma_.back()) {
    ss_ << ", ";
  }
}

std::string JsonBuilder::escape(const std::string& s) {
  std::string result;
  result.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (c < 0x20u) {
          constexpr char kHex[] = "0123456789abcdef";
          result += "\\u00";
          result += kHex[(c >> 4u) & 0x0Fu];
          result += kHex[c & 0x0Fu];
        } else {
          result += static_cast<char>(c);
        }
    }
  }
  return result;
}
