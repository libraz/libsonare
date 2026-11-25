#pragma once

#include "support/float_roundoff.h"
#include "util/json.h"

namespace sonare::test {

inline bool json_within_roundoff(const util::json::Value& actual,
                                 const util::json::Value& expected) {
  if (expected.is_number()) {
    if (!actual.is_number()) return false;
    const double a = actual.as_number();
    const double b = expected.as_number();
    // JSON decodes to double, but the measurements come from float DSP.
    const double tolerance =
        4 * std::numeric_limits<float>::epsilon() * std::max(std::abs(a), std::abs(b));
    return std::abs(a - b) <= tolerance;
  }
  if (expected.is_array()) {
    if (!actual.is_array() || actual.size() != expected.size()) return false;
    for (std::size_t i = 0; i < actual.size(); ++i) {
      if (!json_within_roundoff(actual[i], expected[i])) return false;
    }
    return true;
  }
  if (expected.is_object()) {
    if (!actual.is_object() || actual.size() != expected.size()) return false;
    for (const auto& [key, value] : expected.as_object()) {
      const auto* found = actual.find(key);
      if (!found || !json_within_roundoff(*found, value)) return false;
    }
    return true;
  }
  if (expected.is_string()) return actual.is_string() && actual.as_string() == expected.as_string();
  if (expected.is_bool()) return actual.is_bool() && actual.as_bool() == expected.as_bool();
  return actual.is_null();
}

}  // namespace sonare::test
